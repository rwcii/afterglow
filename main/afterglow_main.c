// afterglow_main.c — Afterglow firmware entry point
//
// Wires the modules together and runs the cooperative loop. The single-radio
// backend serializes capture and replay through one time-division
// scheduler; on a dual-radio backend these run concurrently.
//
// Build order maps to phases: P1 capture, P2 replay, P3 pool+lifecycle
// P4 mesh. Phases that are not yet validated ship disabled by default
// (replay.wifi_beacons_enabled=false, mesh.enabled=false — ).
#include "esp_log.h"
#include "nvs_flash.h"
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

    const radio_backend_t *radio = radio_backend_get();
    ESP_LOGI(TAG, "radio backend: %s (concurrent=%d)",
             radio->name, radio->concurrent_capture_replay);
    ESP_ERROR_CHECK(radio->init());

    // P3 storage slab in PSRAM.
    ESP_ERROR_CHECK(pool_init(&s_cfg));
    ESP_ERROR_CHECK(classifier_init(&s_cfg));

    // P1: start capturing. capture glues radio_backend -> pool/classifier.
    ESP_ERROR_CHECK(capture_start(&s_cfg));

    // P2/P3: replay + tx-power entropy.
    ESP_ERROR_CHECK(txentropy_init(&s_cfg));
    ESP_ERROR_CHECK(replay_init(&s_cfg));

    // P4: mesh (disabled by default until validated).
    if (s_cfg.mesh.enabled) {
        ESP_ERROR_CHECK(mesh_init(&s_cfg));
    }

    // Cooperative loop. On the single-radio backend, each tick yields radio
    // segments per the scheduler's airtime budget; the heavy lifting
    // lives in the modules' own tasks/timers. This loop drives the slow
    // periodic work: eviction sweeps, lifecycle, entropy drift.
    while (true) {
        pool_evict_sweep();
        lifecycle_tick();
        ag_entropy_drift_tick();   // slow boot-constant drift
        if (s_cfg.mesh.enabled) {
            mesh_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
