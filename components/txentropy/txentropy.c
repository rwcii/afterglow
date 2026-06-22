// txentropy.c — mean-reverting RSSI walk → hardware ladder level
#include "txentropy.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "pool.h"               // pool_count / pool_record_at for ambient sampling
#include "ag_core/ag_txwalk.h"  // portable walk math (host-tested)
#include "esp_log.h"

static const char *TAG = "txentropy";

// Shared per-node PRNG for the walk. Production seeds this from esp_random via
// the entropy layer at init; the SAME ag_txwalk_step runs host-native in
// test/host/test_txwalk.c under a fixed seed.
static ag_prng_t s_rng;

// Adaptive walk variance, derived from the spread of currently-observed
// sources. Default (pedestrian) prior until enough real sources are seen.
static float s_ambient_sigma = 2.0f;

esp_err_t txentropy_init(void)
{
    ag_prng_seed(&s_rng, ag_rand_u32() | ((uint64_t)ag_rand_u32() << 32));
    ESP_LOGI(TAG, "txentropy init");
    return ESP_OK;
}

int8_t txentropy_level_for_ghost(ag_beacon_record_t *ghost, ag_proto_t proto)
{
    if (!ghost) return 0;
    // Delegate the privacy-critical walk to ag_core. The single
    // round-robin slot gives per-beacon RSSI control despite the global power
    // register; backend maps the returned index to esp_ble_tx_power_set
    // esp_wifi_set_max_tx_power in the pre-TX hook.
    int p = (proto == AG_PROTO_WIFI) ? 1 : 0;
    ag_txwalk_params_t prm = ag_txwalk_defaults(p);
    prm.p_center = ghost->p_center;
    prm.sigma_step = s_ambient_sigma; // ambient-adaptive (txentropy_update_ambient)
    float v = ag_txwalk_step(&prm, &s_rng, &ghost->p_virt);
    int n = 0;
    const float *ladder = ag_txwalk_ladder(p, &n);
    return (int8_t)ag_txwalk_quantize(ladder, n, v);
}

void txentropy_update_ambient(void)
{
    // Drive the walk step from the average per-source TEMPORAL RSSI variability
    // — how much real sources actually move over time — not the spatial
    // spread between sources sitting at different distances. The two diverge in
    // the case that matters most: a quiet room of steady devices at varied
    // ranges has a large spatial spread but near-zero temporal motion, and
    // ghosts should stay calm there, not walk hard. Averaging rssi_dev_ewma
    // keeps sigma LOW in that case. Fall back to the pedestrian prior when too
    // few real sources are observed.
    uint16_t n = pool_count();
    if (n < 6) { s_ambient_sigma = 2.0f; return; }

    double sum_dev = 0.0;
    uint16_t used = 0;
    for (uint16_t i = 0; i < n; i++) {
        const ag_beacon_record_t *r = pool_record_at(i);
        if (!r) continue;
        sum_dev += (double)r->rssi_dev_ewma;
        used++;
    }
    if (used < 6) { s_ambient_sigma = 2.0f; return; }
    float ambient_dev = (float)(sum_dev / used);
    s_ambient_sigma = ag_txwalk_ambient_sigma(ambient_dev);
    ESP_LOGD(TAG, "ambient sigma=%.2f from %u sources (mean dev=%.2f dB)",
             s_ambient_sigma, used, ambient_dev);
#ifdef AG_ONAIR_TEST
    // Test-only ground-truth hook for the on-air rig: the derived sigma and its
    // input. Mirrors the other ONAIR hooks; production path never compiles this.
    ESP_LOGI(TAG, "ONAIR ambient sigma=%.3f used=%u dev=%.3f",
             s_ambient_sigma, used, ambient_dev);
#endif
}
