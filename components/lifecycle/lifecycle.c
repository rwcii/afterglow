// lifecycle.c — per-record presence tracking (hardware wrapper).
// Per-record decisions are the portable, host-tested lifecycle_logic; this
// wrapper walks the pool, supplies the clock/RNG/config, applies the own-device
// exclusion, and advances each record's presence (departed) state. Record
// lifetime is owned by the pool eviction sweep's per-record TTL.
#include "lifecycle.h"
#include "lifecycle_logic.h"
#include "pool.h"
#include "pool_logic.h"         // ag_pool_rec_id (successor id recompute)
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

        // Advance the per-record presence state: ag_life_tick_record sets
        // AG_FLAG_DEPARTING once the source has been silent past its
        // cadence-scaled gap (and the flag is cleared in pool admission the
        // moment the source is observed again). The departed flag gates emission
        // — a record is re-emitted only while its source is absent. Record
        // lifetime is governed solely by the eviction sweep's per-record TTL
        // (age from first_seen), so departure no longer clears eligibility or
        // retires the record here.
        ag_life_tick_record(r, t, s_cfg.depart_gap_mult);

        // Address rotation — independent of presence and TTL. A ROTATING
        // (NRPA-class) ghost swaps to a fresh NRPA address on its per-ghost timer
        // and keeps doing so until the lineage's TTL completes (the successor
        // inherits first_seen/base_ttl, so eviction still kills the lineage). The
        // rotation is decoupled from the departure/grace event entirely.
        if (s_cfg.rotation_enabled &&
            ag_life_rotation_mode(r->cls) == AG_LIFE_ROTATING) {
            if (r->next_rotate_ms == 0u) {
                // Arm the first rotation when the ghost becomes rotation-managed.
                r->next_rotate_ms = t + ag_life_draw_rotate_ms(&s_rng);
            } else if (ag_life_rotation_due(r, t)) {
                ag_beacon_record_t succ;
                ag_life_make_successor(r, &succ, ag_rand_u32(), t);
                succ.rec_id = ag_pool_rec_id(succ.addr_type, succ.orig_addr,
                                             succ.payload, succ.payload_len);
                succ.next_rotate_ms = t + ag_life_draw_rotate_ms(&s_rng);
#ifdef AG_ONAIR_TEST
                // Rig hook: announce the address swap so the harness can confirm a
                // ROTATING ghost churns on-air and the lineage keeps its TTL.
                ESP_LOGI(TAG, "ONAIR rotate old=%02x:%02x:%02x:%02x:%02x:%02x "
                         "new=%02x:%02x:%02x:%02x:%02x:%02x first_seen=%u next=%u",
                         r->orig_addr[0], r->orig_addr[1], r->orig_addr[2],
                         r->orig_addr[3], r->orig_addr[4], r->orig_addr[5],
                         succ.orig_addr[0], succ.orig_addr[1], succ.orig_addr[2],
                         succ.orig_addr[3], succ.orig_addr[4], succ.orig_addr[5],
                         (unsigned)succ.first_seen_ms, (unsigned)succ.next_rotate_ms);
#endif
                *r = succ;  // rotate in place; keeps the round-robin slot/eligibility
            }
        }
    }
}
