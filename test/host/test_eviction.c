// test_eviction.c — combined eviction properties.
//
// Core property: there must be NO static, characteristic eviction threshold —
// the pool's decay must not expose a fixed algorithmic edge. So we assert: TTL
// draws are spread (not a spike), drop ages have real variance across seeds,
// decay is bounded (nothing lives forever), and weak/old records die faster
// than strong/fresh ones (but not on a cliff).
#include "ag_core/ag_eviction.h"
#include "test_util.h"

// Simulate one record's lifetime under repeated sweeps; return the sweep index
// at which it was evicted (or -1 if it survived the horizon).
static int lifetime_sweeps(const ag_evict_params_t *prm, ag_prng_t *rng,
                           float base_ttl_s, float sweep_s,
                           float fill_frac, float score_pct, int max_sweeps)
{
    for (int k = 1; k <= max_sweeps; k++) {
        float age = k * sweep_s;
        if (ag_evict_decide(prm, rng, age, base_ttl_s, fill_frac, score_pct)) {
            return k;
        }
    }
    return -1;
}

int main(void)
{
    TEST_BEGIN("eviction");
    ag_evict_params_t prm = ag_evict_defaults();

    // (1) base-TTL draws are log-normal-spread, NOT a uniform block or a spike.
    {
        ag_prng_t rng;
        ag_prng_seed(&rng, 1);
        const int N = 5000;
        double sum = 0, sumsq = 0;
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < N; i++) {
            float t = ag_evict_draw_base_ttl(&prm, &rng);
            CHECK(t >= prm.ttl_min_s - 1e-3f && t <= prm.ttl_max_s + 1e-3f);
            sum += t; sumsq += (double)t * t;
            if (t < lo) lo = t;
            if (t > hi) hi = t;
        }
        double m = sum / N;
        double var = sumsq / N - m * m;
        CHECK_MSG(var > 5000.0, "TTL draws too concentrated (var=%.0f) — the "
                  "distribution has a sharp edge", var);
        CHECK_MSG((hi - lo) > 600.0, "TTL range only %.0fs — too narrow", hi - lo);
    }

    // (2) drop ages vary across seeds (stochastic, not deterministic threshold).
    {
        const int TRIALS = 400;
        static int ages[400];
        double sum = 0;
        int survived = 0;
        for (int i = 0; i < TRIALS; i++) {
            ag_prng_t rng;
            ag_prng_seed(&rng, 1000 + i);
            float base_ttl = 540.0f;            // fixed, so variance is from dropout
            int k = lifetime_sweeps(&prm, &rng, base_ttl, 30.0f,
                                    0.5f, 0.5f, 400);
            ages[i] = k;
            if (k < 0) { survived++; continue; }
            sum += k;
        }
        int counted = TRIALS - survived;
        CHECK_MSG(counted > TRIALS / 2, "too many survived the horizon (%d)", survived);
        double m = sum / counted;
        double var = 0;
        for (int i = 0; i < TRIALS; i++) {
            if (ages[i] < 0) continue;
            var += (ages[i] - m) * (ages[i] - m);
        }
        var /= counted;
        CHECK_MSG(var > 4.0, "drop-age variance %.2f too low — decay looks "
                  "like a fixed threshold", var);
    }

    // (3) bounded: even a top-score record eventually dies (no immortality).
    {
        int survived = 0;
        for (int i = 0; i < 200; i++) {
            ag_prng_t rng;
            ag_prng_seed(&rng, 7000 + i);
            int k = lifetime_sweeps(&prm, &rng, 1500.0f, 30.0f, 0.5f, 1.0f, 4000);
            if (k < 0) survived++;
        }
        CHECK_MSG(survived < 20, "%d/200 immortal records — decay not bounded",
                  survived);
    }

    // (4) capacity pressure: above the knee, low-score records evict more than
    //     high-score ones; below the knee, nobody evicts on capacity.
    {
        CHECK(ag_evict_capacity_prob(&prm, 0.50f, 0.1f) == 0.0f); // below knee
        float weak = ag_evict_capacity_prob(&prm, 0.98f, 0.05f);
        float strong = ag_evict_capacity_prob(&prm, 0.98f, 0.95f);
        CHECK_MSG(weak > strong, "capacity eviction should spare high-score "
                  "records (weak=%.3f strong=%.3f)", weak, strong);
    }

    // (5) weak/old die faster than strong/fresh (monotone-ish, not a cliff).
    {
        ag_prng_t rng;
        ag_prng_seed(&rng, 9);
        double h_weak_old = 0, h_strong_fresh = 0;
        for (int i = 0; i < 2000; i++) {
            h_weak_old += ag_evict_dropout_hazard(&prm, &rng, 2.0f, 0.1f);
            h_strong_fresh += ag_evict_dropout_hazard(&prm, &rng, 0.1f, 0.9f);
        }
        CHECK_MSG(h_weak_old > h_strong_fresh * 2.0,
                  "weak/old hazard (%.2f) should exceed strong/fresh (%.2f)",
                  h_weak_old, h_strong_fresh);
    }

    TEST_SUMMARY();
}
