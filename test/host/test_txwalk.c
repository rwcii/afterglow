// test_txwalk.c — RSSI random-walk properties.
//
// Core property: the walk must not emit a constant signal level — a fixed node
// whose ghosts radiate at a stable RSSI has near-zero variance. So we assert
// the walk produces real variance over time, stays bounded (steps are not
// jumps), and the long-run mean reverts toward p_center.
#include "ag_core/ag_txwalk.h"
#include "test_util.h"

static double mean(const float *x, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s += x[i];
    return s / n;
}

static double variance(const float *x, int n)
{
    double m = mean(x, n), s = 0;
    for (int i = 0; i < n; i++) s += (x[i] - m) * (x[i] - m);
    return s / n;
}

int main(void)
{
    TEST_BEGIN("txwalk");
    ag_prng_t rng;
    ag_prng_seed(&rng, 0xC0FFEEu);

    for (int proto = 0; proto <= 1; proto++) {
        ag_txwalk_params_t prm = ag_txwalk_defaults(proto);
        prm.p_center = ag_txwalk_roll_center(&rng, proto);

        const int N = 4000;
        static float trace[4000];
        float v = prm.p_center;
        for (int i = 0; i < N; i++) {
            trace[i] = ag_txwalk_step(&prm, &rng, &v);
            // bounded to the working window
            CHECK(trace[i] >= prm.p_min - 1e-3f);
            CHECK(trace[i] <= prm.p_max + 1e-3f);
        }

        // (1) signal-level variance must be clearly > 0.
        double var = variance(trace, N);
        CHECK_MSG(var > 1.0, "proto %d: RSSI variance %.3f too low — output is "
                  "near-constant", proto, var);

        // (2) mean reversion: long-run mean near p_center (within the window).
        double m = mean(trace, N);
        CHECK_MSG(fabs(m - prm.p_center) < 4.0,
                  "proto %d: mean %.2f drifted from p_center %.2f",
                  proto, m, prm.p_center);

        // (3) steps are a walk, not teleports: most consecutive deltas are
        // small (shadowing makes a few large, so allow a tail).
        int big = 0;
        for (int i = 1; i < N; i++) {
            float d = trace[i] - trace[i - 1];
            if (d < 0) d = -d;
            if (d > 12.0f) big++;   // > ~4 ladder steps in one update
        }
        CHECK_MSG(big < N / 20, "proto %d: %d/%d steps were jumps (walk should "
                  "be mostly small steps)", proto, big, N);

        // (4) it must actually explore more than one ladder level.
        int n = 0;
        const float *ladder = ag_txwalk_ladder(proto, &n);
        int seen_levels[16] = {0};
        for (int i = 0; i < N; i++) {
            seen_levels[ag_txwalk_quantize(ladder, n, trace[i])] = 1;
        }
        int distinct = 0;
        for (int i = 0; i < n; i++) distinct += seen_levels[i];
        CHECK_MSG(distinct >= 3, "proto %d: only %d distinct levels used",
                  proto, distinct);
    }

    // (5) determinism: same seed → identical trace.
    {
        ag_prng_t a, b;
        ag_prng_seed(&a, 42);
        ag_prng_seed(&b, 42);
        ag_txwalk_params_t prm = ag_txwalk_defaults(0);
        float va = 0, vb = 0;
        for (int i = 0; i < 100; i++) {
            CHECK(ag_txwalk_step(&prm, &a, &va) == ag_txwalk_step(&prm, &b, &vb));
        }
    }

    TEST_SUMMARY();
}
