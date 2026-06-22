// ag_txwalk.c — mean-reverting Gaussian RSSI walk + ladder quantization.
#include "ag_core/ag_txwalk.h"

// BLE working window N12..P9 in 3 dB steps (16 hardware levels, ~8 used).
// Full N24..P20 exists for rare shadow excursions; the working ladder is the
// documented 8-level window.
static const float k_ble_ladder[] = { -12, -9, -6, -3, 0, 3, 6, 9 };
// Wi-Fi verified ~11-level set, working window 5..16.5 dBm.
static const float k_wifi_ladder[] = { 5, 7, 8.5f, 11, 13, 14, 15, 16.5f };

const float *ag_txwalk_ladder(int proto, int *n)
{
    if (proto == 1) {
        *n = (int)(sizeof(k_wifi_ladder) / sizeof(k_wifi_ladder[0]));
        return k_wifi_ladder;
    }
    *n = (int)(sizeof(k_ble_ladder) / sizeof(k_ble_ladder[0]));
    return k_ble_ladder;
}

ag_txwalk_params_t ag_txwalk_defaults(int proto)
{
    ag_txwalk_params_t prm;
    prm.k = 0.10f;
    prm.sigma_step = 2.0f;
    prm.p_shadow = 0.03f;
    prm.shadow_sigma = 8.0f;
    if (proto == 1) {
        prm.p_min = 5.0f;
        prm.p_max = 16.5f;
        prm.p_center = 11.0f;   // window midpoint; caller rolls per ghost
    } else {
        prm.p_min = -12.0f;
        prm.p_max = 9.0f;
        prm.p_center = 0.0f;
    }
    return prm;
}

float ag_txwalk_roll_center(ag_prng_t *rng, int proto)
{
    // : BLE p_center U[-6,+6]; Wi-Fi U[8.5,14].
    return (proto == 1) ? ag_prng_uniform(rng, 8.5f, 14.0f)
                        : ag_prng_uniform(rng, -6.0f, 6.0f);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float ag_txwalk_step(const ag_txwalk_params_t *prm, ag_prng_t *rng, float *p_virt)
{
    float v = *p_virt;
    v += ag_prng_gauss(rng, 0.0f, prm->sigma_step);
    v -= prm->k * (v - prm->p_center);
    // Rare heavy-tail shadowing jump (body/corner fades).
    if (ag_prng_unit(rng) < prm->p_shadow) {
        v += ag_prng_gauss(rng, 0.0f, prm->shadow_sigma);
    }
    v = clampf(v, prm->p_min, prm->p_max);
    *p_virt = v;
    return v;
}

float ag_txwalk_ambient_sigma(float ambient_dev_db)
{
    // Map an observed ambient RSSI deviation (dB) to a per-step walk sigma,
    // held in a sane band. 0.3 attenuates the observed spread; the [0.8, 4.0]
    // clamp keeps the synthesized walk neither frozen nor implausibly jumpy.
    float sigma = ambient_dev_db * 0.3f;
    if (sigma < 0.8f) sigma = 0.8f;
    if (sigma > 4.0f) sigma = 4.0f;
    return sigma;
}

int ag_txwalk_quantize(const float *ladder, int n, float p_virt)
{
    int best = 0;
    float best_d = -1.0f;
    for (int i = 0; i < n; i++) {
        float d = ladder[i] - p_virt;
        if (d < 0) d = -d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}
