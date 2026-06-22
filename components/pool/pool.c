// pool.c — PSRAM slab + admission + eviction sweep
//
// Thin hardware wrapper: owns the PSRAM slab, the per-node RNG, and the clock.
// All record logic (rec_id, merge, find-or-insert, the sweep) is the portable,
// host-tested pool_logic. The slab is kept COMPACTED — live records occupy
// [0, count) — so pool_logic can treat it as a plain array.
#include "pool.h"
#include "pool_logic.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "ag_core/ag_eviction.h" // portable eviction model (host-tested)
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "pool";

static ag_beacon_record_t *s_slab;   // PSRAM-resident, compacted [0,count)
static uint16_t s_capacity;
static uint16_t s_count;
static uint16_t s_carry_cap;
static ag_evict_params_t s_evp;
static ag_prng_t s_rng;              // per-node RNG for TTL draw + dropout jitter
static uint32_t s_node_id;
static int s_last_idx = -1;          // index touched by the last pool_admit

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t pool_init(void)
{
    afterglow_config_t cfg;
    afterglow_config_load(&cfg);
    s_capacity = cfg.store_cap;
    s_carry_cap = cfg.carry_cap;

    s_evp = ag_evict_defaults();
    s_evp.ttl_min_s = cfg.ttl_lognorm_lo_min * 60.0f;
    s_evp.ttl_max_s = cfg.ttl_lognorm_hi_min * 60.0f;
    s_evp.base_rate = cfg.dropout_base_rate;
    s_evp.weibull_k = cfg.dropout_weibull_k;

    ag_prng_seed(&s_rng, ag_rand_u32() | ((uint64_t)ag_rand_u32() << 32));
    s_node_id = ag_rand_u32();

    // Single preallocated PSRAM slab — no per-record malloc (avoids
    // fragmentation; DMA/ISR never touch this).
    s_slab = heap_caps_malloc((size_t)s_capacity * sizeof(*s_slab), MALLOC_CAP_SPIRAM);
    if (!s_slab) {
        ESP_LOGE(TAG, "PSRAM slab alloc failed (%u records)", s_capacity);
        return ESP_ERR_NO_MEM;
    }
    s_count = 0;
    ESP_LOGI(TAG, "pool slab: %u records, %u bytes PSRAM",
             s_capacity, (unsigned)(s_capacity * sizeof(*s_slab)));
    return ESP_OK;
}

// Extract the observed identity (orig_addr + addr_type) from a captured frame.
// BLE legacy adv: AdvA is the 6 bytes after the 2-byte header; the address-type
// (random/public + sub-type) is carried on the PDU header's TxAdd/type bits —
// the backend surfaces only the raw payload, so we read the AdvA and leave the
// fine type to the classifier. Wi-Fi: BSSID is addr3 in the 802.11 header.
static bool extract_identity(const ag_capture_t *cap, uint8_t out_addr[6],
                             uint8_t *out_addr_type)
{
    if (cap->proto == AG_PROTO_BLE) {
        if (cap->frame_len < 6) return false;
        // Backend hands us AdvData beginning at AdvA for legacy adv.
        for (int i = 0; i < 6; i++) out_addr[i] = cap->frame[i];
        // Static-random vs RPA/NRPA distinction is the classifier's job; record
        // the random/public bit the backend stashes in addr_type via channel? —
        // here we mark "random" (1) by default; classifier refines.
        *out_addr_type = 1;
        return true;
    }
    // Wi-Fi beacon: addr3 (BSSID) starts at offset 16 of the MAC header.
    if (cap->frame_len < 22) return false;
    for (int i = 0; i < 6; i++) out_addr[i] = cap->frame[16 + i];
    *out_addr_type = 0;
    return true;
}

esp_err_t pool_admit(const ag_capture_t *cap)
{
    if (!cap || !cap->frame || cap->frame_len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t addr[6];
    uint8_t addr_type;
    if (!extract_identity(cap, addr, &addr_type)) return ESP_ERR_INVALID_ARG;

    int idx = ag_pool_admit(s_slab, &s_count, s_capacity, cap, addr, addr_type,
                            now_ms(), s_node_id, &s_evp, &s_rng);
    if (idx < 0) {
        // Slab full: run a sweep to reclaim, then retry once.
        ag_pool_evict_sweep(s_slab, &s_count, s_carry_cap, now_ms(), &s_evp, &s_rng);
        idx = ag_pool_admit(s_slab, &s_count, s_capacity, cap, addr, addr_type,
                            now_ms(), s_node_id, &s_evp, &s_rng);
        if (idx < 0) { s_last_idx = -1; return ESP_ERR_NO_MEM; }
    }
    s_last_idx = idx;
    return ESP_OK;
}

// Index of the record touched by the most recent successful pool_admit, or -1.
// Valid only until the next sweep/admit; the single capture task reads it
// immediately after pool_admit to classify the just-observed record.
int pool_last_admitted(void) { return s_last_idx; }

void pool_evict_sweep(void)
{
    uint16_t evicted = ag_pool_evict_sweep(s_slab, &s_count, s_carry_cap,
                                           now_ms(), &s_evp, &s_rng);
    if (evicted) ESP_LOGD(TAG, "evicted %u (live=%u)", evicted, s_count);
}

uint16_t pool_count(void) { return s_count; }
uint16_t pool_capacity(void) { return s_capacity; }
uint32_t pool_node_id(void) { return s_node_id; }

int pool_insert_record(const ag_beacon_record_t *rec, bool trust_rec_id)
{
    if (!rec) return -1;
    if (s_count >= s_capacity) {
        ag_pool_evict_sweep(s_slab, &s_count, s_carry_cap, now_ms(), &s_evp, &s_rng);
        if (s_count >= s_capacity) return -1;
    }
    ag_beacon_record_t *dst = &s_slab[s_count];
    *dst = *rec;
    // Keep a meshed record's carried rec_id (the stable id its sender's seen-set
    // deduped on); (re)compute one only for a freshly built local record.
    if (!trust_rec_id) {
        dst->rec_id = ag_pool_rec_id(dst->addr_type, dst->orig_addr,
                                     dst->payload, dst->payload_len);
    }
    int idx = (int)s_count;
    s_count++;
    return idx;
}

// Mutable borrow for classifier/lifecycle to update record state in place.
ag_beacon_record_t *pool_record_mut(uint16_t idx)
{
    return (idx < s_count) ? &s_slab[idx] : NULL;
}

const ag_beacon_record_t *pool_record_at(uint16_t idx)
{
    return (idx < s_count) ? &s_slab[idx] : NULL;
}
