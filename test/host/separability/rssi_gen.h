// rssi_gen.h — shared RSSI stream generators for the separability tests.
//
// Both the per-source separability test and the population-level aggregate test
// model the same two RSSI streams: a reference moving-device walk and the
// ag_core-generated walk. Keep the generators here (one definition) so the two
// tests cannot silently diverge on the OU coefficients or the tx-walk mapping.
#pragma once

#include "ag_core/ag_txwalk.h"
#include "ag_core/ag_prng.h"

// Reference RSSI series for a moving device: an OU-like walk with natural
// variance, generated independently of ag_core as the comparison baseline.
static inline void gen_real_rssi(ag_prng_t *r, double *out, int n, double center)
{
    double v = center;
    for (int i = 0; i < n; i++) {
        v += ag_prng_gauss(r, 0.0f, 2.2f) - 0.12f * (v - center);
        if (ag_prng_unit(r) < 0.04) v += ag_prng_gauss(r, 0.0f, 7.5f);
        if (v < -95) v = -95;
        if (v > -30) v = -30;
        out[i] = v;
    }
}

// Generated RSSI via the actual ag_core walk, mapped to an absolute RSSI by
// treating tx-power deltas as path-loss-equivalent dB around a center.
static inline void gen_ghost_rssi(ag_prng_t *r, double *out, int n, double center)
{
    ag_txwalk_params_t prm = ag_txwalk_defaults(0);
    prm.p_center = ag_txwalk_roll_center(r, 0);
    float p = prm.p_center;
    for (int i = 0; i < n; i++) {
        float dp = ag_txwalk_step(&prm, r, &p);
        out[i] = center + (dp - prm.p_center); // power variation → RSSI variation
    }
}
