// ag_eviction.h — combined eviction model (portable).
//
// A record dies if ANY of three mechanisms fires on a sweep. Combining them
// avoids a single fixed eviction threshold:
//
//   1. Capacity (soft, score-ranked): above 0.75*carry_cap, evict with
//      probability sigmoid(4*excess)*(1 - score_percentile). Never clean LRU.
//   2. Age (per-record log-normal TTL): base_ttl drawn log-normal over
//      [3,25] min (not uniform — uniform would create sharp edges).
//   3. Stochastic dropout: Weibull-shaped age hazard (k~1.5) scaled by a base
//      rate, score/age modulated, jittered ±20%.
//
// Combined, the effective half-life smears across ~10-40 min. Pure: randomness
// injected via ag_prng_t.
#pragma once

#include <stdbool.h>
#include "ag_core/ag_prng.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tunable eviction parameters (with default values noted per field).
typedef struct {
    // Capacity
    float soft_fill_frac;   // begin pressure at this fraction of carry_cap (0.75)
    // Age TTL (log-normal over [ttl_min_s, ttl_max_s])
    float ttl_min_s;        // 180 (3 min)
    float ttl_max_s;        // 1500 (25 min)
    float ttl_median_s;     // 540 (~9 min) — sets the log-normal location
    // Weibull stochastic dropout
    float weibull_k;        // shape, ~1.5 (rising hazard with age)
    float base_rate;        // per-sweep base hazard (0.015 = 1.5%/sweep)
    float mod_weak_old;     // hazard multiplier ceiling for weak/old (3.0)
    float mod_strong_fresh; // hazard multiplier floor for strong/fresh (0.4)
    float jitter_frac;      // ±fraction applied to the final hazard (0.20)
} ag_evict_params_t;

ag_evict_params_t ag_evict_defaults(void);

// Draw a per-record base TTL (seconds) from the log-normal over [min,max].
// Called once when a record is admitted. Log-normal, never uniform.
float ag_evict_draw_base_ttl(const ag_evict_params_t *prm, ag_prng_t *rng);

// Capacity eviction probability for a record given current fill and its score
// percentile in [0,1] (1 = best). Returns 0 below the soft-fill knee.
float ag_evict_capacity_prob(const ag_evict_params_t *prm,
                             float fill_frac, float score_percentile);

// Weibull-shaped stochastic-dropout hazard for this sweep, given the record's
// normalized age (age_s / base_ttl_s) and a score percentile in [0,1].
// Returns a probability in [0,1]; randomness (jitter) injected via rng.
float ag_evict_dropout_hazard(const ag_evict_params_t *prm, ag_prng_t *rng,
                              float norm_age, float score_percentile);

// One full eviction decision for a record on a sweep. Combines all three
// mechanisms (capacity OR age-expiry OR stochastic dropout). Returns true if
// the record should be evicted this sweep.
bool ag_evict_decide(const ag_evict_params_t *prm, ag_prng_t *rng,
                     float age_s, float base_ttl_s,
                     float fill_frac, float score_percentile);

#ifdef __cplusplus
}
#endif
