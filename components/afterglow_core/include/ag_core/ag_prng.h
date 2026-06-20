// ag_prng.h — deterministic, injectable PRNG (portable, zero ESP deps)
//
// The privacy-critical math (eviction dropout, subset random term, RSSI walk
// mesh jitter) needs randomness. To make all of it reproducible under test, the
// randomness is threaded through an explicit ag_prng_t state rather than a
// global. Production seeds it from esp_random (the `entropy` component wraps
// this); tests seed it with a fixed value to get deterministic, replayable runs.
//
// Algorithm: xoshiro256** — fast, well-distributed, 256-bit state. Not for
// cryptographic use (none of the privacy properties here depend on CSPRNG
// strength; they depend on distribution shape and per-node entropy).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t s[4];
    // cached normal sample (Box-Muller produces two at a time)
    double   gauss_cache;
    int      gauss_has_cache;
} ag_prng_t;

// Seed from a single 64-bit value (splitmix64-expanded into the 256-bit state).
void ag_prng_seed(ag_prng_t *p, uint64_t seed);

uint32_t ag_prng_u32(ag_prng_t *p);
uint64_t ag_prng_u64(ag_prng_t *p);

// U[0,1) double.
double ag_prng_unit(ag_prng_t *p);

// U[lo,hi) float.
float ag_prng_uniform(ag_prng_t *p, float lo, float hi);

// N(mu, sigma) float (Box-Muller).
float ag_prng_gauss(ag_prng_t *p, float mu, float sigma);

#ifdef __cplusplus
}
#endif
