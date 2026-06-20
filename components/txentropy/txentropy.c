// txentropy.c — mean-reverting RSSI walk → hardware ladder level
#include "txentropy.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "pool.h"               // pool_count / pool_record_at for ambient sampling
#include "ag_core/ag_txwalk.h"  // portable walk math (host-tested)
#include "esp_log.h"
#include <math.h>

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
    // Sample the std-dev of rssi_ewma across live pool records and set the walk
    // step toward it, so the synthesized signal variation tracks the room rather
    // than a fixed pedestrian figure. Fall back to the prior when too sparse.
    uint16_t n = pool_count();
    if (n < 6) { s_ambient_sigma = 2.0f; return; }

    double sum = 0.0, sumsq = 0.0;
    uint16_t used = 0;
    for (uint16_t i = 0; i < n; i++) {
        const ag_beacon_record_t *r = pool_record_at(i);
        if (!r) continue;
        double v = (double)r->rssi_ewma;
        sum += v; sumsq += v * v; used++;
    }
    if (used < 6) { s_ambient_sigma = 2.0f; return; }
    double mean = sum / used;
    double var = sumsq / used - mean * mean;
    double sd = var > 0.0 ? sqrt(var) : 0.0;

    // Map the observed spread to a per-step sigma in a sane band [0.8, 4.0] dB.
    float sigma = (float)(sd * 0.3);
    if (sigma < 0.8f) sigma = 0.8f;
    if (sigma > 4.0f) sigma = 4.0f;
    s_ambient_sigma = sigma;
    ESP_LOGD(TAG, "ambient sigma=%.2f from %u sources", s_ambient_sigma, used);
}
