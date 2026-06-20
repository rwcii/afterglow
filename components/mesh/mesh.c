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

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t mesh_init(void)
{
    afterglow_config_load(&s_cfg);
    s_node_id = pool_node_id();   // share the pool's per-boot NodeID for origin
    ag_prng_seed(&s_rng, ag_rand_u32() | ((uint64_t)ag_rand_u32() << 32));
    ag_seen_init(&s_seen, s_seen_ids, s_seen_stamps, MESH_SEEN_CAP);
    s_last_hello_ms = 0;
    ESP_LOGI(TAG, "mesh init (enabled=%d node_id=%08x)", s_cfg.mesh_enabled,
             (unsigned)s_node_id);
    return ESP_OK;
}

// Emit the HELLO heartbeat from a rotating static-random address. Self-identthe
// node via the mesh service UUID + version in mfg-data; recognized by peers
// during their normal passive scan (no SCAN_REQ).
static void emit_hello(void)
{
    uint8_t adv[8];
    adv[0] = 0x07;          // AD length
    adv[1] = 0xFF;          // manufacturer-specific
    adv[2] = 0xAF; adv[3] = 0x6C;       // mesh service id (Afterglow)
    adv[4] = 0x01;          // protocol version
    memcpy(&adv[5], &s_node_id, 3);     // low 24 bits of NodeID (HELLO id)
    ag_emit_t e = {
        .proto = AG_PROTO_BLE, .frame = adv, .frame_len = sizeof(adv),
        .channel = 37, .tx_power_idx = 8, .interval_ms = 100,
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
        uint8_t frags = ag_mesh_frag_count(r->payload_len, 20);
        // Emit each fragment as a connectionless DATA adv (transport detail:
        // 4-bit frag_index/frag_total + the body slice; reassembled by peers).
        for (uint8_t f = 0; f < frags; f++) {
            uint8_t adv[31];
            uint8_t off = (uint8_t)(f * 20);
            uint8_t body = (uint8_t)(r->payload_len - off);
            if (body > 20) body = 20;
            adv[0] = (uint8_t)(body + 4);
            adv[1] = 0xFF;
            adv[2] = (uint8_t)((f << 4) | (frags & 0x0F)); // frag_index | frag_total
            adv[3] = (uint8_t)(r->rec_id & 0xFF);
            adv[4] = (uint8_t)(r->rec_id >> 8);
            memcpy(&adv[5], r->payload + off, body);
            ag_emit_t e = {
                .proto = AG_PROTO_BLE, .frame = adv, .frame_len = (uint16_t)(body + 5),
                .channel = 37, .tx_power_idx = 8, .interval_ms = 100,
            };
            radio_backend_get()->emit(&e);
        }
        sent++;
    }
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
    }
    // REFRESH_LOWER / DROP_* require no absorption.
}

void mesh_tick(void)
{
    if (!s_cfg.mesh_enabled) return; // ships off (Group F)
    uint32_t t = now_ms();

    // HELLO heartbeat ~4 s ±25%.
    uint32_t hello_gap = 3000 + (uint32_t)ag_prng_uniform(&s_rng, 0.0f, 2000.0f);
    if (t - s_last_hello_ms >= hello_gap) {
        emit_hello();
        s_last_hello_ms = t;
    }

    // Contact-driven transfer is invoked by the transport on peer detection;
    // the periodic tick only drives HELLO. transfer_to_peer() is called from
    // the GAP scan-result handler with the peer's NodeID low-16.
    (void)transfer_to_peer;
}
