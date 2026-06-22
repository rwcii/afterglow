// ag_txwalk.h — per-ghost mean-reverting RSSI random walk (portable)
//
// Each ghost owns a continuous virtual TX power p_virt that performs a
// mean-reverting Gaussian random walk with occasional shadowing jumps:
//
//   p_virt += N(0, sigma_step) - k*(p_virt - p_center)   [+ rare shadow]
//   p_virt = clamp(p_virt, p_min, p_max)
//
// quantized to the nearest hardware ladder step at emission. A fixed node whose
// ghosts emit at constant RSSI has near-zero signal-level variance; the walk
// instead gives every ghost signal-level variation consistent with a moving
// device.
//
// Pure: no ESP/FreeRTOS deps. Randomness is injected via ag_prng_t.
#pragma once

#include "ag_core/ag_prng.h"

#ifdef __cplusplus
extern "C" {
#endif

// Walk parameters (defaults shown; randomized per-node/per-ghost in production).
typedef struct {
    float k;            // mean-reversion strength (default 0.10)
    float sigma_step;   // per-update Gaussian step std, dB (default 2.0, sub-step)
    float p_center;     // per-ghost reversion target, dBm
    float p_min, p_max; // working window, dBm (BLE -12..+9; Wi-Fi 5..16.5)
    float p_shadow;     // per-update shadowing probability (default 0.03)
    float shadow_sigma; // shadowing jump std, dB (default 8.0)
} ag_txwalk_params_t;

// Sensible default params for a protocol window. proto: 0=BLE, 1=Wi-Fi.
// p_center is left at the window midpoint; the caller should randomize it
// per ghost via ag_txwalk_roll_center.
ag_txwalk_params_t ag_txwalk_defaults(int proto);

// Roll a per-ghost p_center uniformly within the documented center range
// (BLE U[-6,+6]; Wi-Fi U[8.5,14]). proto: 0=BLE, 1=Wi-Fi.
float ag_txwalk_roll_center(ag_prng_t *rng, int proto);

// Advance one walk update. Returns the new p_virt (also stored via *p_virt).
float ag_txwalk_step(const ag_txwalk_params_t *prm, ag_prng_t *rng, float *p_virt);

// Map an observed ambient RSSI deviation (dB) to a per-step walk sigma, held in
// the documented [0.8, 4.0] dB band. The input should be a TEMPORAL
// deviation (how much sources move over time), not a cross-source spatial spread.
float ag_txwalk_ambient_sigma(float ambient_dev_db);

// Quantize a virtual power to the nearest level on a discrete hardware ladder.
// `ladder` is an ascending array of `n` dBm values. Returns the chosen index.
int ag_txwalk_quantize(const float *ladder, int n, float p_virt);

// The documented hardware ladders. Returns a pointer to a
// static ascending array and writes its length to *n. proto: 0=BLE, 1=Wi-Fi.
const float *ag_txwalk_ladder(int proto, int *n);

#ifdef __cplusplus
}
#endif
