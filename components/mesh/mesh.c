// mesh.c — connectionless gossip with loop/amplification bounds (hardware
// wrapper). Selection/fragmentation/carry-eligibility are the portable,
// host-tested mesh_logic; the inbound-record gate is ag_core's ag_mesh_evaluate.
// This wrapper owns the BLE adv transport, the contact table, and the clock.
// Ships disabled (mesh_enabled=false).
#include "mesh.h"
#include "mesh_logic.h"
#include "radio_backend.h"
#include "pool.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "ag_core/ag_meshguard.h" // portable TTL/origin/seen-set guards (host-tested)
#include "ag_core/ag_prng.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "mesh";

static afterglow_config_t s_cfg;
static uint32_t s_node_id; // 32-bit per-boot random, RAM only
static ag_prng_t s_rng;
static uint32_t s_last_hello_ms;

// LRU seen-set backing (sized by config; static here for the scaffold). The
// guard logic itself is host-tested in test/host/test_meshguard.c.
#define MESH_SEEN_CAP 4096
static uint16_t s_seen_ids[MESH_SEEN_CAP];
static uint32_t s_seen_stamps[MESH_SEEN_CAP];
static ag_seen_t s_seen;

// Mesh service id (manufacturer-specific 0xFF AD). The protocol version
// (AG_MESH_VERSION, in mesh_logic.h) is carried on both HELLO and every DATA
// fragment and gated on receive; v2 added addr_type + origin_lo16 to the DATA
// frame (see the DATA layout below).
#define MESH_SVC_LO 0xAF
#define MESH_SVC_HI 0x6C

// Connectionless transfer reliability: each DATA fragment is enqueued this many
// times (the transport is unacked and a record absorbs only if every fragment
// lands). The arbiter already broadcasts each enqueued frame several times
// within its dwell (across all 3 channels), so a small repeat count adds
// temporal diversity across the peer's scan windows without flooding the serial
// adv queue — over-repeating just overflows it and sheds the tail fragments.
#define MESH_FRAG_REPEAT 2

// Contact table: peers seen recently, with a per-peer cooldown so we don't
// re-transfer to the same node on every HELLO. The slot type and the cooldown/
// eviction decision are the portable, host-tested ag_contact_* logic; this
// wrapper just owns the storage and the clock.
#define MESH_CONTACTS 8
static ag_contact_t s_contacts[MESH_CONTACTS];

// Fragment reassembly buffer: a single in-flight record per rec_id slot. The
// assemble/have-mask/complete state machine is the host-tested ag_reasm_t in
// mesh_logic; this wrapper owns the slot storage and rebuilds the pool record
// on completion.
#define MESH_REASM 8
static ag_reasm_t s_reasm[MESH_REASM];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

#ifdef AG_ONAIR_TEST
// Seed one carry-eligible record so a HELLO-triggered transfer actually fires on
// the rig (a fresh boot has no multi-sweep-persistent records yet). Compiled in
// only for the on-air transfer rig; never in a normal build.
static void onair_seed_record(void)
{
    ag_beacon_record_t r;
    memset(&r, 0, sizeof(r));
    r.proto = AG_PROTO_BLE;
    r.cls = AG_CLASS_STATIC_RANDOM_BLE;
    r.flags = AG_FLAG_REPLAY_ELIGIBLE;   // carry needs replay-eligible...
    r.obs_count = 3;                     // ...and multi-sweep persistence (>=2).
    r.addr_type = 1;                     // random addr type (feeds rec_id)
    r.hop_ttl = 0;                       // fresh air capture: own origin, ttl0 ok
    r.origin_node = s_node_id;           // we are the first air-capturer
    // A two-fragment payload (>16 B) exercises real fragmentation/reassembly.
    // The first 6 bytes are the AdvA (== payload[0..6], feeds rec_id); mix this
    // node's id into the AdvA + tail so the two rig boards seed DISTINCT records
    // (identical seeds would collide and the peer would REFRESH_LOWER, not
    // ACCEPT, masking the transfer).
    uint8_t SEED[24] = {
        0xC0, 0xFF, 0xEE,
        (uint8_t)(s_node_id), (uint8_t)(s_node_id >> 8), (uint8_t)(s_node_id >> 16),
        0x02, 0x01, 0x06, 0x07, 0x08, 0x09,
        0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13,
        (uint8_t)(s_node_id), (uint8_t)(s_node_id >> 24),
    };
    memcpy(r.orig_addr, SEED, 6);
    memcpy(r.payload, SEED, sizeof(SEED));
    r.payload_len = (uint8_t)sizeof(SEED);
    uint32_t t = now_ms();
    r.first_seen_ms = t;
    r.last_seen_ms = t;
    int idx = pool_insert_record(&r, false);  // local record: compute rec_id
    const ag_beacon_record_t *p = (idx >= 0) ? pool_record_at((uint16_t)idx) : NULL;
    ESP_LOGI(TAG, "ONAIR mesh seed rec=%04x len=%u",
             p ? p->rec_id : 0xFFFF, (unsigned)sizeof(SEED));
}
#endif

esp_err_t mesh_init(void)
{
    afterglow_config_load(&s_cfg);
#ifdef AG_ONAIR_TEST
    // On-air test hook (compiled in only for the tools/onair-test rig): mesh.c
    // keeps its own config copy, so force-enable it here too — the rig drives the
    // HELLO exchange + contact-table cooldown through mesh_try_consume/mesh_tick,
    // both of which gate on this copy. Mesh still ships disabled in production.
    s_cfg.mesh_enabled = true;
    // The default 0.15 transfer fraction rounds a single seeded record down to
    // zero; force full selection so the one seeded record actually transfers.
    s_cfg.mesh_transfer_fraction = 1.0f;
    // Shorten the per-peer contact cooldown so a re-offer (and its seen-set
    // dedup) is observable within a short rig window, not the 120 s production
    // gap. The dedup path itself is unchanged.
    s_cfg.mesh_contact_cooldown_ms = 15000;
#endif
    s_node_id = pool_node_id();   // share the pool's per-boot NodeID for origin
    ag_prng_seed(&s_rng, ag_rand_u32() | ((uint64_t)ag_rand_u32() << 32));
    ag_seen_init(&s_seen, s_seen_ids, s_seen_stamps, MESH_SEEN_CAP);
    s_last_hello_ms = 0;
    ESP_LOGI(TAG, "mesh init (enabled=%d node_id=%08x)", s_cfg.mesh_enabled,
             (unsigned)s_node_id);
#ifdef AG_ONAIR_TEST
    onair_seed_record();
#endif
    return ESP_OK;
}

// Mesh frame layout, inside a manufacturer-specific (0xFF) AD structure:
//   [len][0xFF][SVC_LO][SVC_HI][type][version][ ...type-specific... ]
// type 0x01 = HELLO: [nodeid_lo24:3][version:1]
// type 0x02 = DATA:  [version:1][ttl:1][frag_byte:1][rec_id:2][addr_type:1]
//             [origin_lo16:2][body...]  (frag_byte = frag_index<<4 | frag_total).
//             version (AG_MESH_VERSION) comes FIRST, before any layout-dependent
//             field, so the gate can read it without already knowing the layout;
//             it is gated on receive so a peer speaking a different layout is
//             rejected rather than misparsed. Its value lives in a reserved high
//             band (see AG_MESH_VERSION) so it cannot be mistaken for the `ttl`
//             an older versionless DATA frame carried at this same offset. addr_type lets
//             the receiver recompute the SAME rec_id the seen-set deduped on
//             (rec_id = hash(addr_type||orig_addr||payload)); origin_lo16 carries
//             the first air-capturer's NodeID low-16 so the return-to-source /
//             own-origin guards hold across hops.
#define MESH_TYPE_HELLO 0x01
#define MESH_TYPE_DATA  0x02

// AD framing bytes between the length byte and the type byte: 0xFF, SVC_LO,
// SVC_HI.
#define MESH_AD_HDR 3
// HELLO type-specific header bytes, from the `type` byte through version:
// type, nodeid_lo24 (3), version = 5. The version sits LAST (its historical
// position; the layout is fixed-size so a fixed offset is unambiguous) — the
// gate keys on its value. One constant ties the emit length, the parse guard,
// and the version-byte index together so they cannot drift apart.
#define MESH_HELLO_HDR 5
// Total bytes after the AD length byte for a HELLO (svc framing + HELLO header).
#define MESH_HELLO_LEN (MESH_AD_HDR + MESH_HELLO_HDR)
// DATA type-specific header bytes, from the `type` byte through origin_hi:
// type, version, ttl, frag_byte, rec_lo, rec_hi, addr_type, origin_lo,
// origin_hi = 9.
#define MESH_DATA_HDR 9
// Bytes after the AD length byte that precede the body (svc framing + DATA
// header). The length byte itself (adv[0]) is set to this + body length, and the
// parser keys body_len off ad_len - this.
#define MESH_DATA_PRE (MESH_AD_HDR + MESH_DATA_HDR)

// Emit the HELLO heartbeat. Self-identifies the node via the mesh service id +
// type in mfg-data; recognized by peers during their normal passive scan.
static void emit_hello(void)
{
    uint8_t adv[1 + MESH_HELLO_LEN];
    adv[0] = MESH_HELLO_LEN; // AD length (bytes after this one)
    adv[1] = 0xFF;          // manufacturer-specific
    adv[2] = MESH_SVC_LO; adv[3] = MESH_SVC_HI;
    adv[4] = MESH_TYPE_HELLO;
    memcpy(&adv[5], &s_node_id, 3);     // low 24 bits of NodeID
    adv[MESH_HELLO_LEN] = AG_MESH_VERSION;   // version byte: last field of HELLO
    ag_emit_t e = {
        .proto = AG_PROTO_BLE, .frame = adv, .frame_len = sizeof(adv),
        .channel = 37, .tx_power_idx = 8, .interval_ms = 100, .priority = true,
    };
    radio_backend_get()->emit(&e);
}

// Pool-index getter for the host-tested iterator selection: the pool exposes
// records by index, not as one contiguous slab.
static const ag_beacon_record_t *pool_getter(void *ctx, uint16_t i)
{
    (void)ctx;
    return pool_record_at(i);
}

// Fragment one carry-eligible record over connectionless DATA bursts to a peer.
static void emit_record(const ag_beacon_record_t *r)
{
    // Carry TTL: a fresh air-captured record (hop_ttl 0) starts at TTL_INIT;
    // a relayed record carries its remaining hop_ttl. ttl==0 is replay-only.
    uint8_t carry_ttl = r->hop_ttl ? r->hop_ttl : AG_TTL_INIT;
    uint16_t origin_lo16 = (uint16_t)(r->origin_node & 0xFFFFu);
    uint8_t frags = ag_mesh_frag_count(r->payload_len, AG_MESH_FRAG_BODY);
#ifdef AG_ONAIR_TEST
    ESP_LOGI(TAG, "ONAIR mesh data-tx rec=%04x frags=%u", r->rec_id, frags);
#endif
    for (uint8_t f = 0; f < frags; f++) {
        uint8_t adv[31];
        uint8_t off = (uint8_t)(f * AG_MESH_FRAG_BODY);
        uint8_t body = (uint8_t)(r->payload_len - off);
        if (body > AG_MESH_FRAG_BODY) body = AG_MESH_FRAG_BODY;
        // [len][0xFF][SVC_LO][SVC_HI][DATA][version][ttl][frag][rec_lo][rec_hi]
        // [addr_type][origin_lo][origin_hi][body]
        adv[0]  = (uint8_t)(MESH_DATA_PRE + body); // bytes after the length byte
        adv[1]  = 0xFF;
        adv[2]  = MESH_SVC_LO; adv[3] = MESH_SVC_HI;
        adv[4]  = MESH_TYPE_DATA;
        adv[5]  = AG_MESH_VERSION;
        adv[6]  = carry_ttl;
        adv[7]  = (uint8_t)((f << 4) | (frags & 0x0F));
        adv[8]  = (uint8_t)(r->rec_id & 0xFF);
        adv[9]  = (uint8_t)(r->rec_id >> 8);
        adv[10] = r->addr_type;
        adv[11] = (uint8_t)(origin_lo16 & 0xFF);
        adv[12] = (uint8_t)(origin_lo16 >> 8);
        memcpy(&adv[1 + MESH_DATA_PRE], r->payload + off, body);
        // Connectionless DATA is unacknowledged and a record absorbs only if ALL
        // its fragments land, so loss of any one fragment loses the whole record.
        // Enqueue each fragment MESH_FRAG_REPEAT times: the arbiter gives each a
        // short-interval, multi-broadcast dwell, and the repeats spread it across
        // distinct (and the peer's Wi-Fi-interrupted) scan windows.
        // interval_ms=30 → the arbiter broadcasts ~4x per dwell on all 3 channels.
        ag_emit_t e = {
            .proto = AG_PROTO_BLE, .frame = adv,
            .frame_len = (uint16_t)(1 + MESH_DATA_PRE + body),
            .channel = 37, .tx_power_idx = 8, .interval_ms = 30, .priority = true,
        };
        for (uint8_t rep = 0; rep < MESH_FRAG_REPEAT; rep++) {
            radio_backend_get()->emit(&e);
        }
    }
}

// Hand a peer a recency-weighted random subset of carry-eligible records,
// fragmented over connectionless DATA bursts. peer_lo16 is the contacted peer's
// NodeID low-16 (records from that origin are never returned to it). The subset
// selection routes through the host-tested ag_mesh_select_subset_fn so what
// ships == what is tested (recency-weighted sampling + the transfer fraction).
// Upper bound on the per-contact selection buffer: config clamps
// mesh_max_records_per_contact to [8,32], so a fixed 32-slot buffer always
// covers the cap.
#define MESH_SELECT_MAX 32

static void transfer_to_peer(uint16_t peer_lo16)
{
    uint16_t n = pool_count();
    uint8_t cap = s_cfg.mesh_max_records_per_contact;
    if (cap == 0) return;
    uint16_t sel[MESH_SELECT_MAX];
    uint8_t out_max = (uint8_t)(cap < MESH_SELECT_MAX ? cap : MESH_SELECT_MAX);
    uint8_t sent = ag_mesh_select_subset_fn(pool_getter, NULL, n,
                                            (uint16_t)(s_node_id & 0xFFFFu),
                                            peer_lo16,
                                            s_cfg.mesh_transfer_fraction,
                                            cap, &s_rng, sel, out_max);
    for (uint8_t k = 0; k < sent; k++) {
        const ag_beacon_record_t *r = pool_record_at(sel[k]);
        if (r) emit_record(r);
    }
    if (sent) ESP_LOGI(TAG, "transferred %u records to peer %04x", sent, peer_lo16);
}

// Gate one fully-reassembled inbound record against the loop/amplification
// guards, and absorb it into the pool on accept. Called by the transport when a
// DATA record from a peer completes reassembly.
void mesh_absorb_inbound(ag_beacon_record_t *rec, uint8_t inbound_ttl,
                         uint32_t origin_node)
{
    bool in_pool = false;
    uint8_t pool_ttl = 0;
    uint16_t n = pool_count();
    for (uint16_t i = 0; i < n; i++) {
        const ag_beacon_record_t *p = pool_record_at(i);
        if (p && p->rec_id == rec->rec_id) { in_pool = true; pool_ttl = p->hop_ttl; break; }
    }
    uint8_t out_ttl = 0;
    // origin_node arrives as the wire origin_lo16 (low-16 of the first
    // air-capturer's NodeID); compare own-origin on the same 16-bit field so the
    // guard is real across hops (a true own-origin record returns with our lo16).
    // ag_mesh_evaluate treats origin==0 as "unknown, skip the own-origin check",
    // so bias both sides into a nonzero subspace (set bit 16) — otherwise a node
    // whose lo16 is 0 would never recognize its own-origin records. The DATA path
    // always carries an origin, so there is no genuine "unknown" case to lose.
    uint32_t origin_keyed = origin_node | 0x10000u;
    uint32_t self_keyed   = (s_node_id & 0xFFFFu) | 0x10000u;
    ag_mesh_verdict_t v = ag_mesh_evaluate(&s_seen, rec->rec_id, inbound_ttl,
                                           origin_keyed, self_keyed,
                                           in_pool, pool_ttl, &out_ttl);
#ifdef AG_ONAIR_TEST
    // Ground-truth verdict line for the on-air transfer rig. wire_rec_id is the
    // rec_id the wire/seen-set keyed on; the round-trip check is that the pool
    // recomputes the SAME id (the addr_type-on-the-wire dedup fix).
    static const char *const VN[] = {"ACCEPT", "REFRESH_LOWER", "DROP_TTL",
                                     "DROP_SEEN", "DROP_OWN_ORIGIN"};
    ESP_LOGI(TAG, "ONAIR mesh absorb rec=%04x verdict=%s", rec->rec_id,
             ((int)v >= 0 && (int)v < 5) ? VN[v] : "?");
#endif
    if (v == AG_MESH_ACCEPT) {
        rec->hop_ttl = out_ttl;
        rec->origin_node = origin_node;   // preserve first air-capturer
        int idx = pool_insert_record(rec, true);  // trust the carried wire rec_id
#ifdef AG_ONAIR_TEST
        // The record carries the wire rec_id (the stable id the seen-set deduped
        // on); pool_insert_record trusts it rather than recomputing, so the pool
        // id equals the wire id — the dedup-correctness regression.
        const ag_beacon_record_t *p = (idx >= 0) ? pool_record_at((uint16_t)idx) : NULL;
        ESP_LOGI(TAG, "ONAIR mesh recid-check wire=%04x pool=%04x", rec->rec_id,
                 p ? p->rec_id : 0xFFFF);
#else
        (void)idx;
#endif
        ESP_LOGI(TAG, "absorbed rec_id %04x (ttl->%u)", rec->rec_id, out_ttl);
    }
    // REFRESH_LOWER / DROP_* require no absorption.
}

// Record a peer contact; return true if the per-peer cooldown has elapsed (so
// the caller should transfer to it now). Thin wrapper over the host-tested
// contact-table logic; the storage and clock live here.
static bool contact_seen(uint32_t peer_lo24, uint32_t t)
{
    return ag_contact_should_transfer(s_contacts, MESH_CONTACTS, peer_lo24, t,
                                      s_cfg.mesh_contact_cooldown_ms);
}

// Accept a DATA fragment; on the last missing fragment, reassemble and absorb.
// The assemble/have-mask/complete logic is the host-tested ag_reasm_t; this
// wrapper owns the slot array and rebuilds the pool record on completion.
static void on_data_fragment(uint8_t ttl, uint8_t frag_byte, uint16_t rec_id,
                             uint8_t addr_type, uint32_t origin_node,
                             const uint8_t *body, uint8_t body_len)
{
    int slot = ag_reasm_slot_for(s_reasm, MESH_REASM, rec_id);
    ag_reasm_t *rb = &s_reasm[slot];
    ag_reasm_verdict_t rv = ag_reasm_add(rb, rec_id, frag_byte, body, body_len);
    if (rv == AG_REASM_BADFRAG) return;
#ifdef AG_ONAIR_TEST
    ESP_LOGI(TAG, "ONAIR mesh frag-rx rec=%04x idx=%u of=%u", rec_id,
             (unsigned)(frag_byte >> 4), (unsigned)(frag_byte & 0x0F));
#endif
    if (rv != AG_REASM_COMPLETE) return;

#ifdef AG_ONAIR_TEST
    ESP_LOGI(TAG, "ONAIR mesh reasm-complete rec=%04x", rec_id);
#endif
    ag_beacon_record_t rec = {0};
    rec.proto = AG_PROTO_BLE;
    rec.cls = AG_CLASS_TENTATIVE;
    rec.payload_len = rb->payload_len;
    memcpy(rec.payload, rb->payload, rb->payload_len);
    // Reconstruct identity from the payload prefix (AdvA was the first 6 B of the
    // captured frame; carried in the payload here) plus the addr_type carried on
    // the wire, so the pool's rec_id recompute reproduces the wire rec_id the
    // seen-set deduped on.
    memcpy(rec.orig_addr, rb->payload, 6);
    rec.addr_type = addr_type;
    rec.rec_id = rec_id;
    ag_reasm_reset(rb);   // free the slot
    mesh_absorb_inbound(&rec, ttl, origin_node);
}

// Inspect a captured BLE frame. If it is an Afterglow mesh frame (HELLO/DATA),
// consume it (drive contact/transfer or reassembly) and return true so the
// capture path does NOT admit it to the pool as an ordinary beacon.
bool mesh_try_consume(const ag_capture_t *cap)
{
    if (!s_cfg.mesh_enabled || cap->proto != AG_PROTO_BLE) return false;
    // Captured BLE frame is AdvA(6) + AdvData; the AD list starts at offset 6.
    if (cap->frame_len < 6 + 5) return false;
    const uint8_t *ad = cap->frame + 6;
    uint8_t ad_len = ad[0];
    if (cap->frame_len < (uint16_t)(6 + 1 + ad_len)) return false;
    if (ad[1] != 0xFF || ad[2] != MESH_SVC_LO || ad[3] != MESH_SVC_HI) return false;

    uint8_t type = ad[4];
    uint32_t t = now_ms();
    if (type == MESH_TYPE_HELLO && ad_len >= MESH_HELLO_LEN) {
        // ad: [0xFF][SVC_LO][SVC_HI][type][lo24_0][lo24_1][lo24_2][version]
        uint8_t peer_version = ad[MESH_HELLO_LEN];
        // A peer speaking a different wire version has a layout we cannot parse
        // safely; ignore its HELLO entirely (no contact-table entry, no transfer)
        // so we never emit DATA it would misparse nor accept DATA from it.
        if (!ag_mesh_version_ok(peer_version)) {
#ifdef AG_ONAIR_TEST
            ESP_LOGI(TAG, "ONAIR mesh hello-badver self=%06x ver=%02x",
                     (unsigned)(s_node_id & 0xFFFFFF), (unsigned)peer_version);
#endif
            return true;   // recognized mesh frame, wrong version — consume, drop
        }
        uint32_t peer = (uint32_t)ad[5] | ((uint32_t)ad[6] << 8) | ((uint32_t)ad[7] << 16);
        if (peer == (s_node_id & 0xFFFFFF)) return true;   // our own HELLO
#ifdef AG_ONAIR_TEST
        // On-air test hook (compiled in only for the tools/onair-test rig): emit
        // ground-truth discovery lines so the host harness can assert each node
        // discovers the other's NodeID and that a re-HELLO inside the cooldown is
        // gated. self/peer are NodeID low-24 (matches the HELLO mfg-data field).
        ESP_LOGI(TAG, "ONAIR mesh hello-rx self=%06x peer=%06x",
                 (unsigned)(s_node_id & 0xFFFFFF), (unsigned)peer);
#endif
        if (contact_seen(peer, t)) {
            ESP_LOGI(TAG, "HELLO from peer %06x -> transferring", (unsigned)peer);
#ifdef AG_ONAIR_TEST
            ESP_LOGI(TAG, "ONAIR mesh peer-discovered self=%06x peer=%06x",
                     (unsigned)(s_node_id & 0xFFFFFF), (unsigned)peer);
#endif
            transfer_to_peer((uint16_t)(peer & 0xFFFF));
        }
#ifdef AG_ONAIR_TEST
        else {
            ESP_LOGI(TAG, "ONAIR mesh cooldown-gated self=%06x peer=%06x",
                     (unsigned)(s_node_id & 0xFFFFFF), (unsigned)peer);
        }
#endif
        return true;
    }
    if (type == MESH_TYPE_DATA && ad_len >= MESH_DATA_PRE) {
        // ad_len counts the bytes after the length byte (svc framing + DATA
        // header + body). DATA header: [type][version][ttl][frag][rec_lo]
        // [rec_hi][addr_type][origin_lo][origin_hi].
        uint8_t frame_version = ad[5];
        // Reject a DATA frame whose wire version we don't speak: its field layout
        // differs (a v1 frame has no version/addr_type/origin bytes here), so
        // parsing it as v2 would shift the body and corrupt reassembly/dedup.
        if (!ag_mesh_version_ok(frame_version)) {
#ifdef AG_ONAIR_TEST
            ESP_LOGI(TAG, "ONAIR mesh data-badver ver=%02x", (unsigned)frame_version);
#endif
            return true;   // recognized mesh frame, wrong version — consume, drop
        }
        uint8_t ttl = ad[6];
        uint8_t frag_byte = ad[7];
        uint16_t rec_id = (uint16_t)(ad[8] | (ad[9] << 8));
        uint8_t addr_type = ad[10];
        uint32_t origin_lo16 = (uint32_t)(ad[11] | (ad[12] << 8));
        const uint8_t *body = &ad[1 + MESH_DATA_PRE];
        uint8_t body_len = (uint8_t)(ad_len - MESH_DATA_PRE);
        // origin_lo16 is the first air-capturer's NodeID low-16, carried so the
        // return-to-source / own-origin guards hold across hops.
        on_data_fragment(ttl, frag_byte, rec_id, addr_type, origin_lo16,
                         body, body_len);
        return true;
    }
    return true;  // recognized mesh frame, unknown subtype — still consume it
}

void mesh_tick(void)
{
    if (!s_cfg.mesh_enabled) return; // ships off (Group F)
    uint32_t t = now_ms();

    // HELLO heartbeat ~4 s ±25%. Contact/transfer and reassembly are driven by
    // mesh_try_consume() from the capture path, not from this tick.
    uint32_t hello_gap = 3000 + (uint32_t)ag_prng_uniform(&s_rng, 0.0f, 2000.0f);
    if (t - s_last_hello_ms >= hello_gap) {
        emit_hello();
        s_last_hello_ms = t;
    }
}
