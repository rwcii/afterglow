// pool.c — PSRAM slab + admission + eviction sweep
#include "pool.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "ag_core/ag_eviction.h" // portable eviction model (host-tested)
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "pool";

static ag_beacon_record_t *s_slab;   // PSRAM-resident fixed-slot array
static uint16_t s_capacity;
static uint16_t s_count;

esp_err_t pool_init(void)
{
    afterglow_config_t cfg;
    afterglow_config_load(&cfg);
    s_capacity = cfg.store_cap;

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

esp_err_t pool_admit(const ag_capture_t *cap)
{
    (void)cap;
    // TODO(P1): compute rec_id; merge into existing record (update rssi_ewma,
    // obs_count, last_seen) or admit a new slot on FIRST sighting.
    // Copy payload from cap->frame (valid only this call).
    return ESP_OK;
}

void pool_evict_sweep(void)
{
    // The combined model (capacity sigmoid + log-normal TTL + Weibull dropout)
    // lives in ag_core and is host-tested in test/host/test_eviction.c. The
    // per-record decision is ag_evict_decide(&prm, &rng, age_s, base_ttl_s
    // fill_frac, score_percentile).
    // TODO(P3): iterate s_slab, compute age/score_percentile/fill_frac per
    // record, draw base_ttl_s at admission via ag_evict_draw_base_ttl, and
    // mark-free records where ag_evict_decide returns true. Sweep cadence
    // 30 s ±15%. The ag_prng_t is owned by the entropy layer.
    ESP_LOGD(TAG, "evict sweep TODO (delegates to ag_evict_decide)");
}

uint16_t pool_count(void) { return s_count; }
uint16_t pool_capacity(void) { return s_capacity; }

const ag_beacon_record_t *pool_record_at(uint16_t idx)
{
    return (idx < s_count) ? &s_slab[idx] : NULL;
}
