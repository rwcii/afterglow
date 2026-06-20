// config.h — Afterglow tunable configuration ( `config`)
//
// One struct holding ALL tunables, persisted in NVS, clamp-on-load.
// Defaults are conservative (less airtime, smaller pools, more entropy than
// hardware max). Field groups mirror Groups A–F.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AG_SCAN_ALTERNATE = 0 } ag_scan_mode_t;
typedef enum { AG_FAST_SLOW_AND_FLAG = 0, AG_FAST_HARD_DROP, AG_FAST_LOWER_FLOOR } ag_fast_policy_t;
typedef enum { AG_TXAD_PER_CLASS = 0, AG_TXAD_STRIP, AG_TXAD_REWRITE } ag_txad_policy_t;

typedef struct {
    // Group A — Scan/capture (A)
    ag_scan_mode_t scan_mode;
    uint16_t cycle_period_ms;       // 1000
    uint16_t ble_window_ms, ble_interval_ms; // 100 / 400
    uint16_t wifi_dwell_ms;         // 150
    uint8_t  wifi_channels[3];      // {1,6,11}
    bool     modem_sleep;

    // Group B — Pool/eviction (B)
    uint16_t store_cap;             // 2048 (512-4096)
    uint16_t carry_cap;             // 64 (40-96)
    uint8_t  capacity_soft_pct;     // 75
    float    ttl_lognorm_lo_min, ttl_lognorm_hi_min; // 3, 25
    float    ttl_ext_factor;        // 1.5
    float    ttl_ext_cap_min;       // 35
    float    dropout_base_rate;     // 0.015/sweep
    float    dropout_weibull_k;     // 1.5
    uint32_t dropout_sweep_ms;      // 30000 (20000-45000)
    uint16_t seen_set_lru;          // 4096

    // Group C — Selection (5-term) & lifecycle (C)
    float    w_rec, w_rssi, w_rnd, w_cls, w_div; // 0.30/0.15/0.25/0.35/0.20
    float    tau_sel_s;             // 120 (60-180)
    uint8_t  replay_min_sightings;  // 3 (single eligibility threshold)
    bool     own_device_exclude;    // true
    uint32_t own_device_window_ms;  // 600000
    uint8_t  depart_gap_mult;       // 5 (3-8)

    // Group D — Replay (D)
    uint8_t  ble_adv_sets;          // 1 (legacy single instance)
    uint8_t  max_concurrent_ghosts; // 8 (4-16) — rotation-set size
    uint16_t rotate_ms;             // 750
    bool     match_interval;        // true
    uint8_t  interval_jitter_pct;   // 3 (NEVER equal)
    uint32_t spawn_jitter_ms_max;   // 60000
    bool     wifi_beacons_enabled;  // FALSE (pending E3/IDF gate)
    bool     ble_enabled;           // true
    ag_fast_policy_t fast_cadence_policy; // SLOW_AND_FLAG

    // Group E — TX entropy (E)
    bool     txentropy_enabled;     // true
    int8_t   ble_min_lvl, ble_max_lvl; // N12 / P9 (enum indices)
    float    ble_pcenter_lo, ble_pcenter_hi; // U[-6,+6]
    float    wifi_min_dbm, wifi_max_dbm;     // 5 / 16.5
    float    wifi_pcenter_lo, wifi_pcenter_hi; // U[8.5,14]
    float    sigma_step;            // 2.0
    float    k_revert;              // 0.1
    float    p_shadow;              // 0.03
    float    shadow_sigma;          // 8
    uint8_t  walk_step_max;         // 1
    uint16_t t_walk_ms;             // 2000 ±25%
    ag_txad_policy_t txpower_ad_policy; // PER_CLASS
    bool     ambient_adaptive;      // true

    // Group F — Mesh (off by default) (F)
    bool     mesh_enabled;          // FALSE
    bool     mesh_ble_only;         // true
    float    mesh_transfer_fraction;// 0.15
    uint8_t  mesh_max_records_per_contact; // 16
    uint8_t  mesh_ttl_init;         // 3
    uint32_t mesh_contact_cooldown_ms; // 120000
    bool     mesh_decrement_age_on_hop; // true
    uint16_t mesh_seen_set;         // 4096
} afterglow_config_t;

// Populate cfg with the conservative defaults.
void afterglow_config_defaults(afterglow_config_t *cfg);

// Load from NVS into cfg, falling back to defaults for missing keys, then
// clamp every field to its documented range. Returns ESP_OK on success.
esp_err_t afterglow_config_load(afterglow_config_t *cfg);

// Persist cfg to NVS.
esp_err_t afterglow_config_save(const afterglow_config_t *cfg);

#ifdef __cplusplus
}
#endif
