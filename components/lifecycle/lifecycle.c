// lifecycle.c — rotation-vs-departure successor model (hardware wrapper).
// Per-record decisions are the portable, host-tested lifecycle_logic; this
// wrapper walks the pool, supplies the clock/RNG/config, and applies the
// own-device exclusion + successor insertion.
#include "lifecycle.h"
#include "lifecycle_logic.h"
#include "pool.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "ag_core/ag_prng.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lifecycle";

static afterglow_config_t s_cfg;
static ag_prng_t s_rng;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t lifecycle_init(void)
{
    afterglow_config_load(&s_cfg);
    ag_prng_seed(&s_rng, ag_rand_u32() | ((uint64_t)ag_rand_u32() << 32));
    ESP_LOGI(TAG, "lifecycle init");
    return ESP_OK;
}

void lifecycle_tick(void)
{
    uint32_t t = now_ms();
    uint16_t n = pool_count();

    for (uint16_t i = 0; i < n; i++) {
        ag_beacon_record_t *r = pool_record_mut(i);
        if (!r) continue;

        // Own-device exclusion: co-located operator gear is never replayed.
        if (s_cfg.own_device_exclude &&
            ag_life_is_own_device(r, t, s_cfg.own_device_window_ms)) {
            r->flags &= (uint8_t)~AG_FLAG_REPLAY_ELIGIBLE;
            continue;
        }

        ag_life_action_t act =
            ag_life_tick_record(r, t, s_cfg.depart_gap_mult, &s_rng);

        if (act == AG_LIFE_EXPIRE) {
            // Rotation model: for sources that behaved phone-like (a rotating
            // class), pair the retirement with a fresh successor carrying a
            // continuous RSSI trajectory. Static beacons just fade (the eviction
            // sweep collects them).
            if (r->cls == AG_CLASS_RPA_BLE || r->cls == AG_CLASS_NRPA_BLE) {
                ag_beacon_record_t child;
                ag_life_make_successor(r, &child, ag_rand_u32(), t);
                pool_insert_record(&child);   // recomputes rec_id
            }
            // Mark non-eligible; TTL/dropout in the eviction sweep frees the slot.
            r->flags &= (uint8_t)~AG_FLAG_REPLAY_ELIGIBLE;
            r->flags |= AG_FLAG_DEPARTING;
        }
    }
}
