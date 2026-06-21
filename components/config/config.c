// config.c — defaults / NVS load-save / clamp-on-load
#include "afterglow_config.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "config";

#define AG_NVS_NAMESPACE "afterglow"
#define AG_NVS_BLOB_KEY  "cfg"
// Bump when the struct layout changes incompatibly; a mismatch falls back to
// defaults rather than loading a stale blob.
#define AG_CFG_VERSION   1u

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
        .require_broadcast_only = true, .require_beacon_payload = true,
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

// Range helpers (portable; no hardware deps).
#define CLAMP_U(v, lo, hi)  do { if ((v) < (lo)) (v) = (lo); else if ((v) > (hi)) (v) = (hi); } while (0)
#define CLAMP_F(v, lo, hi)  do { if ((v) < (lo)) (v) = (lo); else if ((v) > (hi)) (v) = (hi); } while (0)

void afterglow_config_clamp(afterglow_config_t *cfg)
{
    // Group A — Scan/capture.
    CLAMP_U(cfg->cycle_period_ms, 250, 5000);
    CLAMP_U(cfg->ble_window_ms, 20, cfg->ble_interval_ms ? cfg->ble_interval_ms : 1000);
    CLAMP_U(cfg->ble_interval_ms, 50, 2000);
    CLAMP_U(cfg->wifi_dwell_ms, 50, 500);

    // Group B — Pool/eviction.
    CLAMP_U(cfg->store_cap, 512, 4096);
    CLAMP_U(cfg->carry_cap, 40, 96);
    CLAMP_U(cfg->capacity_soft_pct, 50, 95);
    CLAMP_F(cfg->ttl_lognorm_lo_min, 1.0f, cfg->ttl_lognorm_hi_min);
    CLAMP_F(cfg->ttl_lognorm_hi_min, cfg->ttl_lognorm_lo_min, 60.0f);
    CLAMP_F(cfg->ttl_ext_factor, 1.0f, 3.0f);
    CLAMP_F(cfg->ttl_ext_cap_min, cfg->ttl_lognorm_hi_min, 90.0f);
    CLAMP_F(cfg->dropout_base_rate, 0.001f, 0.10f);
    CLAMP_F(cfg->dropout_weibull_k, 0.5f, 4.0f);
    CLAMP_U(cfg->dropout_sweep_ms, 20000, 45000);
    CLAMP_U(cfg->seen_set_lru, 1024, 16384);

    // Group C — Selection & lifecycle. Weights stay within documented ranges;
    // the score is a ranking (not normalized to 1), so no renormalization.
    CLAMP_F(cfg->w_rec, 0.20f, 0.40f);
    CLAMP_F(cfg->w_rssi, 0.10f, 0.25f);
    CLAMP_F(cfg->w_rnd, 0.15f, 0.35f);
    CLAMP_F(cfg->w_cls, 0.25f, 0.45f);
    CLAMP_F(cfg->w_div, 0.10f, 0.30f);
    CLAMP_F(cfg->tau_sel_s, 60.0f, 180.0f);
    CLAMP_U(cfg->replay_min_sightings, 2, 8);
    CLAMP_U(cfg->own_device_window_ms, 60000, 3600000);
    CLAMP_U(cfg->depart_gap_mult, 3, 8);

    // Group D — Replay.
    CLAMP_U(cfg->ble_adv_sets, 1, 1);              // single legacy instance
    CLAMP_U(cfg->max_concurrent_ghosts, 4, 16);
    CLAMP_U(cfg->rotate_ms, 250, 2000);
    CLAMP_U(cfg->interval_jitter_pct, 1, 25);      // NEVER 0 (would pin equal)
    if (cfg->spawn_jitter_ms_max > 300000) cfg->spawn_jitter_ms_max = 300000; // floor is 0 (unsigned)
    // require_broadcast_only / require_beacon_payload are bools — already in range;
    // both restored to true on every defaults load (conservative factory reset).

    // Group E — TX entropy.
    CLAMP_U(cfg->ble_min_lvl, -24, cfg->ble_max_lvl);
    CLAMP_U(cfg->ble_max_lvl, cfg->ble_min_lvl, 20);
    CLAMP_F(cfg->wifi_min_dbm, 2.0f, cfg->wifi_max_dbm);
    CLAMP_F(cfg->wifi_max_dbm, cfg->wifi_min_dbm, 20.0f);
    CLAMP_F(cfg->sigma_step, 0.5f, 6.0f);
    CLAMP_F(cfg->k_revert, 0.02f, 0.5f);
    CLAMP_F(cfg->p_shadow, 0.0f, 0.2f);
    CLAMP_F(cfg->shadow_sigma, 1.0f, 16.0f);
    CLAMP_U(cfg->walk_step_max, 1, 3);
    CLAMP_U(cfg->t_walk_ms, 500, 8000);

    // Group F — Mesh.
    CLAMP_F(cfg->mesh_transfer_fraction, 0.10f, 0.25f);
    CLAMP_U(cfg->mesh_max_records_per_contact, 8, 32);
    CLAMP_U(cfg->mesh_ttl_init, 2, 4);
    CLAMP_U(cfg->mesh_contact_cooldown_ms, 60000, 180000);
    CLAMP_U(cfg->mesh_seen_set, 1024, 16384);
}

esp_err_t afterglow_config_load(afterglow_config_t *cfg)
{
    afterglow_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(AG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        // Layout: [u32 version][afterglow_config_t blob]. A version or size
        // mismatch leaves the defaults in place (no stale-blob load).
        uint32_t ver = 0;
        size_t vlen = sizeof(ver);
        afterglow_config_t stored;
        size_t blen = sizeof(stored);
        if (nvs_get_u32(h, "ver", &ver) == ESP_OK && ver == AG_CFG_VERSION &&
            nvs_get_blob(h, AG_NVS_BLOB_KEY, &stored, &blen) == ESP_OK &&
            blen == sizeof(stored)) {
            *cfg = stored;
            ESP_LOGI(TAG, "config loaded from NVS (v%u)", (unsigned)ver);
        } else {
            ESP_LOGW(TAG, "NVS config absent/mismatched — using defaults");
        }
        (void)vlen;
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs_open(read) failed (%d) — using defaults", (int)err);
    }

    // Always clamp: defaults, partial, or full blob alike.
    afterglow_config_clamp(cfg);
    return ESP_OK;
}

esp_err_t afterglow_config_save(const afterglow_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(AG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(write) failed (%d)", (int)err);
        return err;
    }
    err = nvs_set_u32(h, "ver", AG_CFG_VERSION);
    if (err == ESP_OK) err = nvs_set_blob(h, AG_NVS_BLOB_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "config saved to NVS");
    else ESP_LOGE(TAG, "config save failed (%d)", (int)err);
    return err;
}
