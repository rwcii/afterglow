// entropy.h — RNG + slow runtime drift of boot constants
//
// Wraps esp_random for the stochastic machinery (eviction dropout, selection
// random term, RSSI walk, jitter). ag_entropy_drift_tick implements the
// mitigation: slowly random-walk per-node boot constants (weights, p_center
// distribution, dropout rate) so a single stationary node's behavioral
// constants do not stay fixed for long periods.
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "afterglow_config.h" // afterglow_config_t

#ifdef __cplusplus
extern "C" {
#endif

// Seed/init the RNG layer. Call once at boot.
esp_err_t ag_entropy_init(void);

uint32_t ag_rand_u32(void);
float    ag_rand_uniform(float lo, float hi);   // U[lo,hi)
float    ag_rand_gauss(float mu, float sigma);   // N(mu, sigma)

// Slowly walk boot-level config constants on an hours-scale schedule.
// Called periodically from the main loop; mutates cfg in place within ranges.
void ag_entropy_drift_tick(afterglow_config_t *cfg);

#ifdef __cplusplus
}
#endif
