// entropy.c — esp_random-backed RNG + boot-constant drift
#include "entropy.h"
#include <math.h>
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "entropy";

esp_err_t ag_entropy_init(void)
{
    // esp_random is HW-backed once RF (Wi-Fi/BT) is up; before that it falls
    // back to a bootloader seed. We initialize after radio start.
    ESP_LOGI(TAG, "entropy init");
    return ESP_OK;
}

uint32_t ag_rand_u32(void)
{
    return esp_random();
}

float ag_rand_uniform(float lo, float hi)
{
    return lo + (hi - lo) * ((float)esp_random() / (float)UINT32_MAX);
}

float ag_rand_gauss(float mu, float sigma)
{
    // Box-Muller; u1 kept off zero to avoid log(0).
    float u1 = ag_rand_uniform(1e-7f, 1.0f);
    float u2 = ag_rand_uniform(0.0f, 1.0f);
    float z = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
    return mu + sigma * z;
}

void ag_entropy_drift_tick(afterglow_config_t *cfg)
{
    if (!cfg) return;
    // Hours-scale random walk: each call nudges a few boot constants by a small
    // Gaussian step, then re-clamps the whole struct to its documented ranges.
    // The caller schedules this rarely (minutes-to-hours), so the constants
    // wander slowly rather than staying fixed for the node's whole uptime.
    const float STEP = 0.01f; // small per-tick nudge in weight units
    cfg->w_rec  += ag_rand_gauss(0.0f, STEP);
    cfg->w_rssi += ag_rand_gauss(0.0f, STEP);
    cfg->w_rnd  += ag_rand_gauss(0.0f, STEP);
    cfg->w_cls  += ag_rand_gauss(0.0f, STEP);
    cfg->w_div  += ag_rand_gauss(0.0f, STEP);
    cfg->tau_sel_s        += ag_rand_gauss(0.0f, 1.0f);
    cfg->dropout_base_rate += ag_rand_gauss(0.0f, 0.0005f);
    cfg->ble_pcenter_lo   += ag_rand_gauss(0.0f, 0.2f);
    cfg->ble_pcenter_hi   += ag_rand_gauss(0.0f, 0.2f);

    afterglow_config_clamp(cfg);
    ESP_LOGD(TAG, "drift tick applied");
}
