// mesh.c — connectionless gossip with loop/amplification bounds
#include "mesh.h"
#include "radio_backend.h"
#include "pool.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "ag_core/ag_meshguard.h" // portable TTL/origin/seen-set guards (host-tested)
#include "esp_log.h"

static const char *TAG = "mesh";

static afterglow_config_t s_cfg;
static uint32_t s_node_id; // 32-bit per-boot random, RAM only

// LRU seen-set backing (sized by config; static here for the scaffold). The
// guard logic itself is host-tested in test/host/test_meshguard.c.
#define MESH_SEEN_CAP 4096
static uint16_t s_seen_ids[MESH_SEEN_CAP];
static uint32_t s_seen_stamps[MESH_SEEN_CAP];
static ag_seen_t s_seen;

esp_err_t mesh_init(void)
{
    afterglow_config_load(&s_cfg);
    s_node_id = ag_rand_u32(); // per-boot random, never hardware-derived
    ag_seen_init(&s_seen, s_seen_ids, s_seen_stamps, MESH_SEEN_CAP);
    ESP_LOGI(TAG, "mesh init (enabled=%d node_id=%08x)", s_cfg.mesh_enabled,
             (unsigned)s_node_id);
    return ESP_OK;
}

void mesh_tick(void)
{
    if (!s_cfg.mesh_enabled) return; // ships off ( Group F)
    // TODO(P4): HELLO heartbeat (4s +-25% from rotating static-random addr)
    // contact table + cooldown; on contact select recency-weighted random
    // subset (transfer_fraction, capped at max_records_per_contact); fragmented
    // DATA + reassembly. Each inbound record is gated by the host-tested
    // ag_mesh_evaluate(&s_seen, rec_id, inbound_ttl, origin_node, s_node_id
    // already_in_pool, pool_ttl, &out_ttl) — which enforces hop-TTL decrement
    // own-origin refusal, and LRU-seen dedup (no resurrection). Bounded to
    // AG_TTL_INIT distinct nodes.
    ESP_LOGD(TAG, "mesh tick TODO (gated by ag_mesh_evaluate)");
}
