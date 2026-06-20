// capture.c — backend callback → copy-to-internal-RAM → pool + classifier
#include "capture.h"
#include "radio_backend.h"
#include "pool.h"
#include "classifier.h"
#include "esp_log.h"

static const char *TAG = "capture";

// Invoked by the radio backend per observed beacon. cap->frame is backend-owned
// and valid only for this call, so pool_admit must copy what it keeps.
static void on_capture(const ag_capture_t *cap, void *user)
{
    (void)user;
    pool_admit(cap);
    // TODO(P1): locate the resulting record and call classifier_observe(rec).
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
