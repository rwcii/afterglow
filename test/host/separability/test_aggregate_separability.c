// test_aggregate_separability.c — population-level separability characterization.
//
// test_separability.c compares ONE generated stream against ONE reference stream
// along single-source feature axes. This file looks at the POPULATION instead: a
// receiver that sees many emitted sources alongside many reference (real-device)
// sources can try to split the two populations on aggregate features. Three such
// axes are measurable in a single-receiver host model and are CHARACTERIZED here
// — the numbers printed to stderr are the recorded result, not a pass/fail
// threshold. A few directional sanity asserts guard against the model silently
// regressing (e.g. the co-presence gate ceasing to hold), but the measured
// separability values are the deliverable.
//
//   1. RSSI clustering: do the emitted sources' per-source RSSI centers/spreads
//      cluster apart from the reference sources'? (An emitted cohort all radiates
//      from one node's placement and a bounded per-source center roll, so its
//      centers sit tighter than an independently-placed real population.)
//   2. Co-presence timing: an emitted source is served on a round-robin schedule
//      and only while its corresponding real source is ABSENT (the source-present
//      gate). Measure how separable the two populations are on a co-presence
//      overlap scalar.
//   3. Address-type mix: only reproducible address classes are ever emitted, so
//      the emitted population's class mix is intentionally skewed relative to a
//      reference population that also contains the never-reproduced classes.
//      Measure the total-variation distance between the two mixes.
//
// Two further population axes (a shared-oscillator carrier-offset cluster, and
// identical payloads seen from multiple receiver locations) are physical
// properties a single-node, single-receiver host model cannot exercise; they are
// out of scope here.
#include "ag_core/ag_prng.h"
#include "ag_core/ag_eligible.h"
#include "detector.h"
#include "rssi_gen.h"       // shared gen_real_rssi / gen_ghost_rssi
#include "../test_util.h"

#define N 1500           // per-source RSSI samples (matches test_separability.c)
#define M 64             // sources per population (matches the carry-capacity)

// mean of a series.
static double mean_of(const double *x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += x[i];
    return s / (double)n;
}

int main(void)
{
    TEST_BEGIN("aggregate_separability");
    ag_prng_t r;
    ag_prng_seed(&r, 0xA66E6A7Eu);

    static double series[N];

    // ---- Axis 1: population RSSI clustering -------------------------------
    // Build per-source mean RSSI and per-source RSSI spread for both
    // populations. Real sources are independently placed across a wide RSSI
    // range; emitted sources share one node placement, so their centers vary
    // only by the bounded per-source center roll around a common node center.
    static double real_mean[M], ghost_mean[M];
    static double real_sd[M], ghost_sd[M];
    const double NODE_CENTER = -68.0;   // the emitting node's placement
    for (int s = 0; s < M; s++) {
        // a real source sits anywhere in a realistically wide room range.
        double rc = ag_prng_uniform(&r, -88.0f, -48.0f);
        gen_real_rssi(&r, series, N, rc);
        real_mean[s] = mean_of(series, N);
        real_sd[s]   = det_rssi_stability(series, N);
        // an emitted source radiates from the one node; gen_ghost_rssi already
        // rolls a bounded per-source center around the given node center.
        gen_ghost_rssi(&r, series, N, NODE_CENTER);
        ghost_mean[s] = mean_of(series, N);
        ghost_sd[s]   = det_rssi_stability(series, N);
    }

    // Separability of the two populations on per-source mean RSSI: KS over the
    // two mean-distributions, and a pooled two-means split.
    double mean_D    = det_ks_statistic(real_mean, M, ghost_mean, M);
    double mean_Dcr  = det_ks_critical_05(M, M);
    static double pooled_mean[2 * M];
    for (int s = 0; s < M; s++) { pooled_mean[s] = real_mean[s]; pooled_mean[M + s] = ghost_mean[s]; }
    double mean_sep  = det_two_means_separation(pooled_mean, 2 * M);

    fprintf(stderr, "  [rssi-clustering] per-source mean RSSI: KS D=%.3f (crit %.3f), "
            "pooled two-means separation=%.3f\n", mean_D, mean_Dcr, mean_sep);

    // Sanity (not a separability threshold): each emitted source must still vary
    // — the walk is exercised, not frozen at a constant level — else axis 1 would
    // be measuring a degenerate model rather than the real walk.
    double ghost_sd_pop = mean_of(ghost_sd, M);
    double real_sd_pop  = mean_of(real_sd, M);
    CHECK_MSG(ghost_sd_pop > 1.0,
              "emitted per-source RSSI spread %.2f dB is near-constant "
              "(the walk must vary)", ghost_sd_pop);
    fprintf(stderr, "  [rssi-clustering] mean per-source spread: emitted=%.2f dB, "
            "reference=%.2f dB\n", ghost_sd_pop, real_sd_pop);

    // ---- Axis 2: co-presence timing (mutual exclusion) -------------------
    // Model a timeline of T slots. A real source is present in runs; an emitted
    // source is served on a round-robin schedule (one slot per rotation across
    // up-to MAX_GHOSTS) AND only while its real source is ABSENT (the source-
    // present gate). Co-presence overlap = fraction of slots where a source and
    // its own clone are both active. The gate makes this ~0 for emitted sources.
    enum { T = 4096, MAX_GHOSTS = 8 };   // MAX_GHOSTS == max_concurrent_ghosts default
    ag_elig_policy_t pol = ag_elig_defaults();
    static double real_overlap[M], ghost_overlap[M];
    for (int s = 0; s < M; s++) {
        // a per-source present/absent timeline: alternating runs of random length.
        int my_slots = 0;          // this source's round-robin emission opportunities
        int ghost_co = 0;          // of those, clone active while source present
        int real_co = 0;           // of those, source present (honest self co-presence)
        bool present = (ag_prng_unit(&r) < 0.5);
        int run = 1 + (int)ag_prng_uniform(&r, 0.0f, 60.0f);
        for (int t = 0; t < T; t++) {
            if (run-- <= 0) { present = !present; run = 1 + (int)ag_prng_uniform(&r, 0.0f, 60.0f); }
            // round-robin: this source's emit slot comes up every MAX_GHOSTS slots.
            bool rr_slot = ((t % MAX_GHOSTS) == (s % MAX_GHOSTS));
            if (!rr_slot) continue;
            my_slots++;
            // the emitted clone is served in its slot only when the real source is
            // absent — drive the decision through the real gate.
            bool emit = ag_replay_eligible(&pol, AG_ELIG_NRPA,
                            AG_ADV_NONCONN_NONSCAN, /*is_wifi*/false,
                            /*sightings_ok*/true, /*is_own_device*/false,
                            /*source_present*/present);
            if (emit && present) ghost_co++;   // clone active while source present
            if (present) real_co++;            // an honest source coincides with itself
        }
        // Both overlaps share the SAME denominator (this source's emission slots)
        // so the two populations are compared like-for-like: the emitted clone's
        // co-presence is ~0 (the gate suppresses it while present); an honest
        // source's self-co-presence is just its presence fraction over its slots.
        ghost_overlap[s] = (my_slots > 0) ? (double)ghost_co / (double)my_slots : 0.0;
        real_overlap[s]  = (my_slots > 0) ? (double)real_co  / (double)my_slots : 0.0;
    }
    static double pooled_overlap[2 * M];
    for (int s = 0; s < M; s++) { pooled_overlap[s] = real_overlap[s]; pooled_overlap[M + s] = ghost_overlap[s]; }
    double overlap_sep = det_two_means_separation(pooled_overlap, 2 * M);
    double ghost_overlap_pop = mean_of(ghost_overlap, M);

    fprintf(stderr, "  [copresence] emitted-source co-presence overlap=%.4f "
            "(reference=%.4f), pooled two-means separation=%.3f\n",
            ghost_overlap_pop, mean_of(real_overlap, M), overlap_sep);

    // Sanity: the source-present gate must hold — an emitted clone is never
    // active while its source is present, so its overlap is exactly 0. If this
    // regresses, the mutual-exclusion model (and the gate) is broken.
    CHECK_MSG(ghost_overlap_pop == 0.0,
              "emitted co-presence overlap %.4f != 0 (source-present gate must "
              "suppress emission while the source is observed)", ghost_overlap_pop);

    // Round-robin emission cadence is highly regular vs a real source's natural
    // jittered cadence — characterize the contrast (printed, not asserted).
    static double rr_gaps[N], nat_gaps[N];
    double rr_base = (double)(MAX_GHOSTS) * 750.0;   // per-source slot ≈ N·rotate_ms
    double nat_base = 2000.0;                         // a real ~2 s beacon
    for (int i = 0; i < N; i++) {
        rr_gaps[i]  = rr_base + ag_prng_uniform(&r, -3.0f, 3.0f);    // tight round-robin
        nat_gaps[i] = nat_base + ag_prng_uniform(&r, -120.0f, 120.0f); // natural jitter
    }
    double rr_cv  = det_interval_regularity_cv(rr_gaps, N);
    double nat_cv = det_interval_regularity_cv(nat_gaps, N);
    fprintf(stderr, "  [copresence] round-robin cadence CV=%.4f vs natural CV=%.4f\n",
            rr_cv, nat_cv);

    // ---- Axis 3: address-type mix ---------------------------------------
    // The emitted population only carries reproducible address classes; the
    // reference population also includes the never-reproduced classes (rotating-
    // private and public). Drive the emitted mix through the real cloneability
    // predicate, then measure the total-variation distance between the mixes.
    enum { NCLS = 5 };   // ag_elig_class_t has 5 values
    // A realistic ambient BLE population is dominated by rotating-private
    // (RPA) sources, with a tail of static-random, NRPA, public, and Wi-Fi.
    double ref_mix[NCLS] = {
        [AG_ELIG_STATIC_RANDOM] = 0.18,
        [AG_ELIG_NRPA]          = 0.07,
        [AG_ELIG_RPA]           = 0.55,
        [AG_ELIG_PUBLIC]        = 0.12,
        [AG_ELIG_WIFI_BSSID]    = 0.08,
    };
    // The emitted mix: the reference mass restricted to cloneable classes,
    // renormalized. Mass on non-cloneable classes is zero by construction (the
    // gate never reproduces them).
    double emit_mix[NCLS] = {0};
    double clone_mass = 0.0;
    for (int c = 0; c < NCLS; c++)
        if (ag_replay_class_cloneable((ag_elig_class_t)c)) clone_mass += ref_mix[c];
    CHECK_MSG(clone_mass > 0.0, "no cloneable class mass (predicate sanity)");
    for (int c = 0; c < NCLS; c++)
        emit_mix[c] = ag_replay_class_cloneable((ag_elig_class_t)c)
                          ? ref_mix[c] / clone_mass : 0.0;

    // total-variation distance = 1/2 sum |p_i - q_i|.
    double tv = 0.0;
    for (int c = 0; c < NCLS; c++) tv += fabs(ref_mix[c] - emit_mix[c]);
    tv *= 0.5;
    fprintf(stderr, "  [class-mix] total-variation distance(reference, emitted)=%.3f "
            "(emitted carries no rotating-private/public mass)\n", tv);

    // Directional sanity: the class mixes are intentionally divergent — the
    // emitted population is partitionable on address class. This is the opposite
    // polarity of test_separability.c's indistinguishability checks: here a LARGE
    // distance is the expected, deliberate property, and a regression toward 0
    // would mean the cloneability gate stopped restricting the emitted classes.
    CHECK_MSG(tv > 0.3, "class-mix total-variation distance %.3f unexpectedly "
              "small (the emitted population should omit non-reproducible classes)", tv);

    TEST_SUMMARY();
}
