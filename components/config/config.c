// config.c — defaults / NVS load-save / clamp-on-load
#include "afterglow_config.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config";

#define AG_NVS_NAMESPACE "afterglow"

void afterglow_config_defaults(afterglow_config_t *cfg)
{
    //  Group A–F conservative defaults.
    *cfg = (afterglow_config_t){
        .scan_mode = AG_SCAN_ALTERNATE,
        .cycle_period_ms = 1000,
        .ble_window_ms = 100, .ble_interval_ms = 400,
        .wifi_dwell_ms = 150, .wifi_channels = {1, 6, 11},
        .modem_sleep = true,

        .store_cap = 2048, .carry_cap = 64, .capacity_soft_pct = 75,
        .ttl_lognorm_lo_min = 3.0f, .ttl_lognorm_hi_min = 25.0f,
        .ttl_ext_factor = 1.5f, .ttl_ext_cap_min = 35.0f,
        .dropout_base_rate = 0.015f, .dropout_weibull_k = 1.5f,
        .dropout_sweep_ms = 30000, .seen_set_lru = 4096,

        .w_rec = 0.30f, .w_rssi = 0.15f, .w_rnd = 0.25f, .w_cls = 0.35f, .w_div = 0.20f,
        .tau_sel_s = 120.0f, .replay_min_sightings = 3,
        .own_device_exclude = true, .own_device_window_ms = 600000, .depart_gap_mult = 5,

        .ble_adv_sets = 1, .max_concurrent_ghosts = 8, .rotate_ms = 750,
        .match_interval = true, .interval_jitter_pct = 3, .spawn_jitter_ms_max = 60000,
        .wifi_beacons_enabled = false, .ble_enabled = true,
        .fast_cadence_policy = AG_FAST_SLOW_AND_FLAG,

        .txentropy_enabled = true,
        .ble_min_lvl = -12, .ble_max_lvl = 9,
        .ble_pcenter_lo = -6.0f, .ble_pcenter_hi = 6.0f,
        .wifi_min_dbm = 5.0f, .wifi_max_dbm = 16.5f,
        .wifi_pcenter_lo = 8.5f, .wifi_pcenter_hi = 14.0f,
        .sigma_step = 2.0f, .k_revert = 0.1f, .p_shadow = 0.03f, .shadow_sigma = 8.0f,
        .walk_step_max = 1, .t_walk_ms = 2000,
        .txpower_ad_policy = AG_TXAD_PER_CLASS, .ambient_adaptive = true,

        .mesh_enabled = false, .mesh_ble_only = true, .mesh_transfer_fraction = 0.15f,
        .mesh_max_records_per_contact = 16, .mesh_ttl_init = 3,
        .mesh_contact_cooldown_ms = 120000, .mesh_decrement_age_on_hop = true,
        .mesh_seen_set = 4096,
    };
}

esp_err_t afterglow_config_load(afterglow_config_t *cfg)
{
    afterglow_config_defaults(cfg);
    // TODO(P1): read overrides from NVS (nvs_open/nvs_get_*), then clamp each
    // field to its documented range before returning.
    ESP_LOGI(TAG, "config loaded (defaults; NVS load TODO)");
    return ESP_OK;
}

esp_err_t afterglow_config_save(const afterglow_config_t *cfg)
{
    (void)cfg;
    // TODO(P1): nvs_open(AG_NVS_NAMESPACE) + nvs_set_blob + nvs_commit.
    ESP_LOGI(TAG, "config save TODO");
    return ESP_OK;
}
