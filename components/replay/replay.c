// replay.c — round-robin ghost emitter
#include "replay.h"
#include "radio_backend.h"
#include "pool.h"
#include "classifier.h"
#include "txentropy.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "esp_log.h"

static const char *TAG = "replay";

static afterglow_config_t s_cfg;
static uint16_t s_rr_cursor; // round-robin position over the eligible set

esp_err_t replay_init(void)
{
    afterglow_config_load(&s_cfg);
    s_rr_cursor = 0;
    ESP_LOGI(TAG, "replay init (ble=%d wifi=%d ghosts=%u rotate=%ums)",
             s_cfg.ble_enabled, s_cfg.wifi_beacons_enabled,
             s_cfg.max_concurrent_ghosts, s_cfg.rotate_ms);
    return ESP_OK;
}

void replay_tick(void)
{
    // TODO(P2): advance s_rr_cursor to the next REPLAY_ELIGIBLE ghost
    // (classifier_replay_eligible), regenerate its random AdvA, build ag_emit_t
    // {frame=payload, interval=interval_q +- interval_jitter_pct (never equal)
    // tx_power_idx=txentropy_level_for_ghost(...)}, then radio_backend_get->emit.
    // For Wi-Fi: strip FCS + overwrite TSF (H') and seq-control (H) per-TX, only
    // when s_cfg.wifi_beacons_enabled.
    (void)s_rr_cursor;
    ESP_LOGD(TAG, "replay tick TODO");
}
