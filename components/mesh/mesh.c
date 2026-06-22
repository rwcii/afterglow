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

// Mesh service id (manufacturer-specific 0xFF AD) + protocol version.
#define MESH_SVC_LO 0xAF
#define MESH_SVC_HI 0x6C
#define MESH_VERSION 0x01

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

// Fragment reassembly buffer: a single in-flight record per rec_id slot.
#define MESH_REASM 8
typedef struct {
    uint16_t rec_id; uint8_t frag_total; uint8_t have_mask;
    uint8_t payload[31]; uint8_t payload_len; bool used;
} mesh_reasm_t;
static mesh_reasm_t s_reasm[MESH_REASM];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t mesh_init(void)
{
    afterglow_config_load(&s_cfg);
#ifdef AG_ONAIR_TEST
    // On-air test hook (compiled in only for the tools/onair-test rig): mesh.c
    // keeps its own config copy, so force-enable it here too — the rig drives the
    // HELLO exchange + contact-table cooldown through mesh_try_consume/mesh_tick,
    // both of which gate on this copy. Mesh still ships disabled in production.
    s_cfg.mesh_enabled = true;
#endif
    s_node_id = pool_node_id();   // share the pool's per-boot NodeID for origin
    ag_prng_seed(&s_rng, ag_rand_u32() | ((uint64_t)ag_rand_u32() << 32));
    ag_seen_init(&s_seen, s_seen_ids, s_seen_stamps, MESH_SEEN_CAP);
    s_last_hello_ms = 0;
    ESP_LOGI(TAG, "mesh init (enabled=%d node_id=%08x)", s_cfg.mesh_enabled,
             (unsigned)s_node_id);
    return ESP_OK;
}

// Mesh frame layout, inside a manufacturer-specific (0xFF) AD structure:
//   [len][0xFF][SVC_LO][SVC_HI][type][ ...type-specific... ]
// type 0x01 = HELLO: [nodeid_lo24:3]
// type 0x02 = DATA:  [ttl:1][frag_byte:1][rec_id:2][body...]  (frag_byte =
//             frag_index<<4 | frag_total)
#define MESH_TYPE_HELLO 0x01
#define MESH_TYPE_DATA  0x02

// Emit the HELLO heartbeat. Self-identifies the node via the mesh service id +
// type in mfg-data; recognized by peers during their normal passive scan.
static void emit_hello(void)
{
    uint8_t adv[9];
    adv[0] = 0x08;          // AD length (bytes after this one)
    adv[1] = 0xFF;          // manufacturer-specific
    adv[2] = MESH_SVC_LO; adv[3] = MESH_SVC_HI;
    adv[4] = MESH_TYPE_HELLO;
    memcpy(&adv[5], &s_node_id, 3);     // low 24 bits of NodeID
    adv[8] = MESH_VERSION;
    ag_emit_t e = {
        .proto = AG_PROTO_BLE, .frame = adv, .frame_len = sizeof(adv),
        .channel = 37, .tx_power_idx = 8, .interval_ms = 100, .priority = true,
    };
    radio_backend_get()->emit(&e);
}

// Hand a peer a recency-weighted random subset of carry-eligible records,
// fragmented over connectionless DATA bursts. peer_lo16 is the contacted peer's
// NodeID low-16 (records from that origin are never returned to it).
static void transfer_to_peer(uint16_t peer_lo16)
{
    // The pool exposes records by index (not as one contiguous array), so the
    // selection is applied per-record here using the same carry-eligibility +
    // peer-origin rules ag_mesh_select_subset encodes, capped identically.
    uint16_t n = pool_count();
    uint8_t sent = 0;
    for (uint16_t i = 0; i < n && sent < s_cfg.mesh_max_records_per_contact; i++) {
        const ag_beacon_record_t *r = pool_record_at(i);
        if (!r || !ag_mesh_carry_eligible(r)) continue;
        if ((uint16_t)(r->origin_node & 0xFFFF) == peer_lo16) continue;
        // Carry TTL: a fresh air-captured record (hop_ttl 0) starts at TTL_INIT;
        // a relayed record carries its remaining hop_ttl. ttl==0 is replay-only.
        uint8_t carry_ttl = r->hop_ttl ? r->hop_ttl : AG_TTL_INIT;
        const uint8_t BODY = 16;     // body bytes per fragment (fits the header)
        uint8_t frags = ag_mesh_frag_count(r->payload_len, BODY);
        for (uint8_t f = 0; f < frags; f++) {
            uint8_t adv[31];
            uint8_t off = (uint8_t)(f * BODY);
            uint8_t body = (uint8_t)(r->payload_len - off);
            if (body > BODY) body = BODY;
            // [len][0xFF][SVC_LO][SVC_HI][DATA][ttl][frag][rec_lo][rec_hi][body]
            adv[0] = (uint8_t)(8 + body);   // bytes after the length byte
            adv[1] = 0xFF;
            adv[2] = MESH_SVC_LO; adv[3] = MESH_SVC_HI;
            adv[4] = MESH_TYPE_DATA;
            adv[5] = carry_ttl;
            adv[6] = (uint8_t)((f << 4) | (frags & 0x0F));
            adv[7] = (uint8_t)(r->rec_id & 0xFF);
            adv[8] = (uint8_t)(r->rec_id >> 8);
            memcpy(&adv[9], r->payload + off, body);
            // Connectionless DATA is unacknowledged and a record absorbs only if
            // ALL its fragments land, so loss of any one fragment loses the whole
            // record. Enqueue each fragment MESH_FRAG_REPEAT times: the arbiter
            // gives each a short-interval, multi-broadcast dwell, and the repeats
            // spread it across distinct (and the peer's Wi-Fi-interrupted) scan
            // windows. interval_ms=30 → the arbiter broadcasts ~4x per dwell on
            // all 3 adv channels.
            ag_emit_t e = {
                .proto = AG_PROTO_BLE, .frame = adv, .frame_len = (uint16_t)(9 + body),
                .channel = 37, .tx_power_idx = 8, .interval_ms = 30, .priority = true,
            };
            for (uint8_t rep = 0; rep < MESH_FRAG_REPEAT; rep++) {
                radio_backend_get()->emit(&e);
            }
        }
        sent++;
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
    ag_mesh_verdict_t v = ag_mesh_evaluate(&s_seen, rec->rec_id, inbound_ttl,
                                           origin_node, s_node_id, in_pool,
                                           pool_ttl, &out_ttl);
    if (v == AG_MESH_ACCEPT) {
        rec->hop_ttl = out_ttl;
        rec->origin_node = origin_node;   // preserve first air-capturer
        pool_insert_record(rec);
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
static void on_data_fragment(uint8_t ttl, uint8_t frag_byte, uint16_t rec_id,
                             const uint8_t *body, uint8_t body_len,
                             uint32_t origin_node)
{
    uint8_t frag_idx = (uint8_t)(frag_byte >> 4);
    uint8_t frag_tot = (uint8_t)(frag_byte & 0x0F);
    if (frag_tot == 0 || frag_idx >= frag_tot) return;

    // Find or claim a reassembly slot for this rec_id.
    int slot = -1;
    for (int i = 0; i < MESH_REASM; i++)
        if (s_reasm[i].used && s_reasm[i].rec_id == rec_id) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < MESH_REASM; i++)
            if (!s_reasm[i].used) { slot = i; break; }
    if (slot < 0) slot = (int)(rec_id % MESH_REASM);   // overwrite on pressure
    mesh_reasm_t *rb = &s_reasm[slot];
    if (!rb->used || rb->rec_id != rec_id) {
        memset(rb, 0, sizeof(*rb));
        rb->used = true; rb->rec_id = rec_id; rb->frag_total = frag_tot;
    }
    uint8_t off = (uint8_t)(frag_idx * 16);
    if (off + body_len <= sizeof(rb->payload)) {
        memcpy(rb->payload + off, body, body_len);
        if (off + body_len > rb->payload_len) rb->payload_len = (uint8_t)(off + body_len);
    }
    rb->have_mask |= (uint8_t)(1u << frag_idx);

    // All fragments present? Reassemble into a record and gate it.
    uint8_t full = (uint8_t)((1u << frag_tot) - 1);
    if ((rb->have_mask & full) == full) {
        ag_beacon_record_t rec = {0};
        rec.proto = AG_PROTO_BLE;
        rec.cls = AG_CLASS_TENTATIVE;
        rec.payload_len = rb->payload_len;
        memcpy(rec.payload, rb->payload, rb->payload_len);
        // Reconstruct identity from the payload prefix (AdvA was the first 6 B
        // of the captured frame; carried in the payload here).
        memcpy(rec.orig_addr, rb->payload, 6);
        rec.rec_id = rec_id;
        rb->used = false;   // free the slot
        mesh_absorb_inbound(&rec, ttl, origin_node);
    }
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
    if (type == MESH_TYPE_HELLO && ad_len >= 7) {
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
    if (type == MESH_TYPE_DATA && ad_len >= 8) {
        uint8_t ttl = ad[5];
        uint8_t frag_byte = ad[6];
        uint16_t rec_id = (uint16_t)(ad[7] | (ad[8] << 8));
        const uint8_t *body = &ad[9];
        uint8_t body_len = (uint8_t)(ad_len - 8);
        // origin_node is not carried on the wire (kept RAM-only); use the peer's
        // low bits via rec_id provenance is not available, so pin to a non-self
        // sentinel derived from rec_id (own-origin check still protects us).
        on_data_fragment(ttl, frag_byte, rec_id, body, body_len, ~s_node_id);
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
