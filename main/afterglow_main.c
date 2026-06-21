// afterglow_main.c — Afterglow firmware entry point
//
// Wires the modules together and runs the cooperative loop. The single-radio
// backend serializes capture and replay through one time-division
// scheduler; on a dual-radio backend these run concurrently.
//
// Build order maps to phases: P1 capture, P2 replay, P3 pool+lifecycle,
// P4 mesh. Optional phases default off in config and are enabled per
// deployment (replay.wifi_beacons_enabled, mesh.enabled).
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "afterglow_config.h"
#include "radio_backend.h"
#include "entropy.h"
#include "pool.h"
#include "classifier.h"
#include "capture.h"
#include "replay.h"
#include "txentropy.h"
#include "lifecycle.h"
#include "mesh.h"

static const char *TAG = "afterglow";

static afterglow_config_t s_cfg;

void app_main(void)
{
    ESP_LOGI(TAG, "Afterglow starting");

    // NVS first — config lives here (clamp-on-load).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(afterglow_config_load(&s_cfg));
    ag_entropy_init();

    // The Wi-Fi stack posts events to the default event loop; without it the
    // controller fails every event post (ESP_ERR_NO_MEM). netif must also be up
    // before esp_wifi_init even in NULL mode.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const radio_backend_t *radio = radio_backend_get();
    ESP_LOGI(TAG, "radio backend: %s (concurrent=%d)",
             radio->name, radio->concurrent_capture_replay);
    ESP_ERROR_CHECK(radio->init());

    // Storage slab in PSRAM + classifier state.
    ESP_ERROR_CHECK(pool_init());
    ESP_ERROR_CHECK(classifier_init());

    // P1: start capturing. capture glues radio_backend -> pool/classifier.
    ESP_ERROR_CHECK(capture_start());

    // P2/P3: replay + tx-power entropy + lifecycle.
    ESP_ERROR_CHECK(txentropy_init());
    ESP_ERROR_CHECK(replay_init());
    ESP_ERROR_CHECK(lifecycle_init());

    // P4: mesh (optional; enabled via config).
    if (s_cfg.mesh_enabled) {
        ESP_ERROR_CHECK(mesh_init());
    }

    // Replay cadence: drive one round-robin slot every rotate_ms. The single
    // radio time-shares scan and replay, so this is best-effort.
    const TickType_t replay_period = pdMS_TO_TICKS(s_cfg.rotate_ms ? s_cfg.rotate_ms : 750);
    TickType_t last_slow = xTaskGetTickCount();
#ifdef AG_ONAIR_TEST
    // On-air test hook: run the lifecycle/eviction sweep often so a departure is
    // detected within seconds, keeping the on-air test fast.
    const TickType_t slow_period = pdMS_TO_TICKS(2000);
#else
    const TickType_t slow_period = pdMS_TO_TICKS(s_cfg.dropout_sweep_ms ? s_cfg.dropout_sweep_ms : 30000);
#endif
    uint32_t drift_counter = 0;
    TickType_t last_stat = xTaskGetTickCount();

    while (true) {
        // Fast: emit one ghost per rotate_ms slot.
        replay_tick();

        // Periodic pool telemetry (~5 s): observed-beacon population size.
        if (xTaskGetTickCount() - last_stat >= pdMS_TO_TICKS(5000)) {
#ifdef AG_ONAIR_TEST
            // On-air test hook (compiled in only for the tools/onair-test rig):
            // census the eligibility pipeline so the host harness can wait for a
            // source to promote and to depart.
            uint16_t pc = pool_count();
            uint16_t elig = 0, dep = 0, repl = 0;
            for (uint16_t i = 0; i < pc; i++) {
                const ag_beacon_record_t *r = pool_record_at(i);
                if (!r) continue;
                if (r->flags & AG_FLAG_REPLAY_ELIGIBLE) elig++;
                if (r->flags & AG_FLAG_DEPARTING) dep++;
                if (classifier_replay_eligible(r)) repl++;
            }
            ESP_LOGI(TAG, "ONAIR census pool=%u elig=%u dep=%u replayable=%u",
                     pc, elig, dep, repl);
            // Report the lifecycle state of any test-stimulus record so the
            // harness can see it captured, promoted, and departed. orig_addr holds
            // the AdvA as the backend surfaces it: esp_bd_addr_t / MSB-first, so
            // the stimulus body 22:33:44:55 lands in orig_addr[1..4] in that order
            // and orig_addr[0] is the subtype octet (matching the STIM log format).
            for (uint16_t i = 0; i < pc; i++) {
                const ag_beacon_record_t *r = pool_record_at(i);
                if (!r) continue;
                if (r->orig_addr[1] == 0x22 && r->orig_addr[2] == 0x33 &&
                    r->orig_addr[3] == 0x44 && r->orig_addr[4] == 0x55) {
                    ESP_LOGI(TAG, "ONAIR stim addr=%02x:%02x:%02x:%02x:%02x:%02x "
                             "cls=%u obs=%u elig=%d dep=%d repl=%d",
                             r->orig_addr[0], r->orig_addr[1], r->orig_addr[2],
                             r->orig_addr[3], r->orig_addr[4], r->orig_addr[5],
                             r->cls, r->obs_count,
                             (r->flags & AG_FLAG_REPLAY_ELIGIBLE) != 0,
                             (r->flags & AG_FLAG_DEPARTING) != 0,
                             classifier_replay_eligible(r));
                }
            }
#else
            ESP_LOGI(TAG, "pool: %u/%u records", pool_count(), pool_capacity());
#endif
            last_stat = xTaskGetTickCount();
        }

        // Slow periodic work (eviction sweep cadence): eviction, lifecycle,
        // ambient variance, and occasional boot-constant drift.
        if (xTaskGetTickCount() - last_slow >= slow_period) {
            pool_evict_sweep();
            lifecycle_tick();
            txentropy_update_ambient();
            if (++drift_counter % 120 == 0) {   // hours-scale at 30 s sweeps
                ag_entropy_drift_tick(&s_cfg);
            }
            last_slow = xTaskGetTickCount();
        }

        if (s_cfg.mesh_enabled) mesh_tick();

        vTaskDelay(replay_period);
    }
}
