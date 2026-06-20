// ag_subset.h — 5-term composite subset-selection score (portable)
//
//   score = w_rec*f_recency + w_rssi*f_rssi + w_rnd*U(0,1)
//         + w_cls*class_prior(cls) - w_div*crowding(cls)
//
// Used for (a) which observed beacons to carry, and (b) which to hand a peer on
// mesh contact. w_rssi is deliberately LOW (0.15) so the operator's own
// high-RSSI gear does not dominate selection; w_cls dominates (durable,
// reproducible identities are preferred). Weights are re-rolled per node at
// boot within documented ranges and drift slowly at runtime.
//
// NOTE on weights: the positive terms sum to 1.05 and the diversity term is
// subtractive (0.20), so the composite is NOT normalized to [0,1]. That is
// intentional — this is a RANKING score (compared across records), not a
// probability. Percentile rank is what the eviction/selection logic consumes.
//
// Pure: randomness for the U(0,1) term injected via ag_prng_t.
#pragma once

#include "ag_core/ag_prng.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float w_rec;    // recency (default 0.30, range 0.20-0.40)
    float w_rssi;   // signal (default 0.15, range 0.10-0.25 — LOW on purpose)
    float w_rnd;    // random (default 0.25, range 0.15-0.35)
    float w_cls;    // class prior (default 0.35, range 0.25-0.45 — dominant)
    float w_div;    // diversity (default 0.20, range 0.10-0.30 — subtractive)
    float tau_sel_s; // recency time constant (default 120 s, range 60-180)
} ag_subset_weights_t;

ag_subset_weights_t ag_subset_defaults(void);

// Roll a per-node weight set uniformly within the documented ranges. Used at
// boot and slowly walked by the entropy drift.
ag_subset_weights_t ag_subset_roll(ag_prng_t *rng);

// Class prior. cls uses the pool's ag_beacon_class_t values, but to
// stay ESP-free this takes the raw enum int (0=TENTATIVE,1=STATIC_RANDOM
// 2=NRPA,3=RPA,4=PUBLIC,5=WIFI — see pool.h).
float ag_subset_class_prior(int cls);

// f_rssi = clamp((rssi_ewma + 95)/50, 0, 1).
float ag_subset_f_rssi(int rssi_ewma_dbm);

// f_recency = exp(-age/tau_sel).
float ag_subset_f_recency(const ag_subset_weights_t *w, float age_s);

// Full composite score. `crowding` in [0,1] is the local density of the
// record's class (higher = more crowded → penalized for diversity). The random
// term is drawn from rng.
float ag_subset_score(const ag_subset_weights_t *w, ag_prng_t *rng,
                      float age_s, int rssi_ewma_dbm, int cls, float crowding);

#ifdef __cplusplus
}
#endif
