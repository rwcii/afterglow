// capture.c — backend callback → copy-to-internal-RAM → pool + classifier
#include "capture.h"
#include "radio_backend.h"
#include "pool.h"
#include "classifier.h"
#include "esp_log.h"

static const char *TAG = "capture";

// Invoked by the radio backend per observed beacon. cap->frame is backend-owned
// and valid only for this call, so pool_admit must copy what it keeps. Runs on
// the single capture task, so reading pool_last_admitted right after admit is
// race-free.
static void on_capture(const ag_capture_t *cap, void *user)
{
    (void)user;
    if (pool_admit(cap) != ESP_OK) return;
    int idx = pool_last_admitted();
    if (idx < 0) return;
    ag_beacon_record_t *rec = pool_record_mut((uint16_t)idx);
    if (rec) classifier_observe(rec);
}

esp_err_t capture_start(void)
{
    const radio_backend_t *be = radio_backend_get();
    ESP_LOGI(TAG, "capture_start via backend '%s'", be->name);
    return be->capture_start(on_capture, NULL);
}

esp_err_t capture_stop(void)
{
    return radio_backend_get()->capture_stop();
}
