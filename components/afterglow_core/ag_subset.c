// ag_subset.c — composite subset-selection score.
#include "ag_core/ag_subset.h"
#include <math.h>

// Class enum mirrors pool.h ag_beacon_class_t (kept as raw ints here to stay
// ESP-free): 0=TENTATIVE 1=STATIC_RANDOM 2=NRPA 3=RPA 4=PUBLIC 5=WIFI.
enum {
    CLS_TENTATIVE = 0,
    CLS_STATIC_RANDOM_BLE,
    CLS_NRPA_BLE,
    CLS_RPA_BLE,
    CLS_PUBLIC_BLE,
    CLS_WIFI,
};

ag_subset_weights_t ag_subset_defaults(void)
{
    ag_subset_weights_t w;
    w.w_rec = 0.30f;
    w.w_rssi = 0.15f;
    w.w_rnd = 0.25f;
    w.w_cls = 0.35f;
    w.w_div = 0.20f;
    w.tau_sel_s = 120.0f;
    return w;
}

ag_subset_weights_t ag_subset_roll(ag_prng_t *rng)
{
    ag_subset_weights_t w;
    w.w_rec = ag_prng_uniform(rng, 0.20f, 0.40f);
    w.w_rssi = ag_prng_uniform(rng, 0.10f, 0.25f);
    w.w_rnd = ag_prng_uniform(rng, 0.15f, 0.35f);
    w.w_cls = ag_prng_uniform(rng, 0.25f, 0.45f);
    w.w_div = ag_prng_uniform(rng, 0.10f, 0.30f);
    w.tau_sel_s = ag_prng_uniform(rng, 60.0f, 180.0f);
    return w;
}

float ag_subset_class_prior(int cls)
{
    switch (cls) {
        case CLS_STATIC_RANDOM_BLE: return 1.0f;   // durable + bit-reproducible
        case CLS_WIFI:              return 0.8f;   // durable BSSID
        case CLS_PUBLIC_BLE:        return 0.7f;   // durable payload, addr mismatch
        case CLS_NRPA_BLE:          return 0.7f;   // cloneable broadcast identity
        case CLS_TENTATIVE:         return 0.3f;   // first sighting, unproven
        case CLS_RPA_BLE:           return 0.25f;  // uncloneable, self-rotating
        default:                    return 0.3f;
    }
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float ag_subset_f_rssi(int rssi_ewma_dbm)
{
    return clampf(((float)rssi_ewma_dbm + 95.0f) / 50.0f, 0.0f, 1.0f);
}

float ag_subset_f_recency(const ag_subset_weights_t *w, float age_s)
{
    float tau = (w->tau_sel_s > 1e-3f) ? w->tau_sel_s : 120.0f;
    return expf(-age_s / tau);
}

float ag_subset_score(const ag_subset_weights_t *w, ag_prng_t *rng,
                      float age_s, int rssi_ewma_dbm, int cls, float crowding)
{
    float f_rec = ag_subset_f_recency(w, age_s);
    float f_rssi = ag_subset_f_rssi(rssi_ewma_dbm);
    float f_rnd = (float)ag_prng_unit(rng);
    float prior = ag_subset_class_prior(cls);
    float crowd = clampf(crowding, 0.0f, 1.0f);

    return w->w_rec * f_rec
         + w->w_rssi * f_rssi
         + w->w_rnd * f_rnd
         + w->w_cls * prior
         - w->w_div * crowd;
}
