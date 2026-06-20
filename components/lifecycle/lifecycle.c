// lifecycle.c — rotation-vs-departure successor model
#include "lifecycle.h"
#include "pool.h"
#include "afterglow_config.h"
#include "entropy.h"
#include "esp_log.h"

static const char *TAG = "lifecycle";

esp_err_t lifecycle_init(void)
{
    ESP_LOGI(TAG, "lifecycle init");
    return ESP_OK;
}

void lifecycle_tick(void)
{
    // TODO(P3):
    //  - own-device exclusion: skip sources with rssi_ewma > -45 dBm sustained
    //    over own_device_window_ms.
    //  - departure: after depart_gap_mult*interval with no sighting, fade out.
    //  - rotation: for phone-like sources, at expiry spawn a successor ghost
    //    with a fresh static-random/NRPA addr + continuous RSSI trajectory
    //    (death paired with birth), with spawn jitter to decorrelate.
    ESP_LOGD(TAG, "lifecycle tick TODO");
}
