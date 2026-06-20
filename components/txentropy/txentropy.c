// txentropy.c — mean-reverting RSSI walk → hardware ladder level
#include "txentropy.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "ag_core/ag_txwalk.h" // portable walk math (host-tested)
#include "esp_log.h"

static const char *TAG = "txentropy";

// Shared per-node PRNG for the walk. Production seeds this from esp_random via
// the entropy layer at init; the SAME ag_txwalk_step runs host-native in
// test/host/test_txwalk.c under a fixed seed.
static ag_prng_t s_rng;

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
    // TODO(P3): apply ambient-adaptive sigma_step/k (txentropy_update_ambient)
    // before stepping.
    float v = ag_txwalk_step(&prm, &s_rng, &ghost->p_virt);
    int n = 0;
    const float *ladder = ag_txwalk_ladder(p, &n);
    return (int8_t)ag_txwalk_quantize(ladder, n, v);
}

void txentropy_update_ambient(void)
{
    // TODO(P3): recompute sigma_step/k/shadow from the empirical rssi_ewma
    // variance of observed real sources; fall back to pedestrian
    // prior when too few sources seen.
    ESP_LOGD(TAG, "ambient update TODO");
}
