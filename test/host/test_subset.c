// test_subset.c — composite subset-selection score.
//
// Assert each term actually influences the score, no single term pathologically
// dominates, class priors are ordered as documented, and scoring is
// deterministic under a fixed seed.
#include "ag_core/ag_subset.h"
#include "test_util.h"

enum { CLS_TENTATIVE=0, CLS_STATIC_RANDOM, CLS_NRPA, CLS_RPA, CLS_PUBLIC, CLS_WIFI };

int main(void)
{
    TEST_BEGIN("subset");
    ag_subset_weights_t w = ag_subset_defaults();

    // (1) class priors ordered: static-random > wifi > public >= nrpa >
    //     tentative > rpa  ( table).
    CHECK(ag_subset_class_prior(CLS_STATIC_RANDOM) > ag_subset_class_prior(CLS_WIFI));
    CHECK(ag_subset_class_prior(CLS_WIFI) > ag_subset_class_prior(CLS_TENTATIVE));
    CHECK(ag_subset_class_prior(CLS_TENTATIVE) > ag_subset_class_prior(CLS_RPA));
    CHECK(ag_subset_class_prior(CLS_RPA) < 0.3f); // uncloneable, deprioritized

    // (2) f_rssi monotone and clamped.
    CHECK(ag_subset_f_rssi(-100) == 0.0f);
    CHECK(ag_subset_f_rssi(-40) == 1.0f);
    CHECK(ag_subset_f_rssi(-70) > ag_subset_f_rssi(-90));

    // (3) recency decays with age.
    CHECK(ag_subset_f_recency(&w, 0.0f) > ag_subset_f_recency(&w, 120.0f));
    CHECK_NEAR(ag_subset_f_recency(&w, 0.0f), 1.0f, 1e-6);

    // (4) each term moves the score. Use a zero-random probe by averaging.
    ag_prng_t rng;
    ag_prng_seed(&rng, 5);
    // Fresh static-random high-RSSI uncrowded should beat old RPA low-RSSI crowded.
    double good = 0, bad = 0;
    for (int i = 0; i < 2000; i++) {
        good += ag_subset_score(&w, &rng, 5.0f, -50, CLS_STATIC_RANDOM, 0.0f);
        bad  += ag_subset_score(&w, &rng, 600.0f, -92, CLS_RPA, 1.0f);
    }
    CHECK_MSG(good > bad, "good record (%.3f) should outscore bad (%.3f)",
              good / 2000, bad / 2000);

    // (5) w_rssi is deliberately LOW: flipping RSSI from worst to best must not
    //     swing the score as much as flipping the class prior does.
    {
        ag_prng_t r2; ag_prng_seed(&r2, 11);
        // isolate by averaging out the random term
        double rssi_lo = 0, rssi_hi = 0, cls_lo = 0, cls_hi = 0;
        for (int i = 0; i < 4000; i++) {
            rssi_lo += ag_subset_score(&w, &r2, 60.0f, -95, CLS_NRPA, 0.2f);
            rssi_hi += ag_subset_score(&w, &r2, 60.0f, -40, CLS_NRPA, 0.2f);
            cls_lo  += ag_subset_score(&w, &r2, 60.0f, -70, CLS_RPA, 0.2f);
            cls_hi  += ag_subset_score(&w, &r2, 60.0f, -70, CLS_STATIC_RANDOM, 0.2f);
        }
        double rssi_swing = (rssi_hi - rssi_lo) / 4000;
        double cls_swing  = (cls_hi - cls_lo) / 4000;
        CHECK_MSG(cls_swing > rssi_swing, "class swing (%.3f) should exceed RSSI "
                  "swing (%.3f) — RSSI weight must stay low", cls_swing, rssi_swing);
    }

    // (6) determinism under fixed seed.
    {
        ag_prng_t a, b;
        ag_prng_seed(&a, 99); ag_prng_seed(&b, 99);
        for (int i = 0; i < 50; i++) {
            float sa = ag_subset_score(&w, &a, 10.0f, -60, CLS_WIFI, 0.3f);
            float sb = ag_subset_score(&w, &b, 10.0f, -60, CLS_WIFI, 0.3f);
            CHECK(sa == sb);
        }
    }

    // (7) per-node roll stays within documented ranges.
    {
        ag_prng_t r; ag_prng_seed(&r, 3);
        for (int i = 0; i < 1000; i++) {
            ag_subset_weights_t rw = ag_subset_roll(&r);
            CHECK(rw.w_rec >= 0.20f && rw.w_rec <= 0.40f);
            CHECK(rw.w_rssi >= 0.10f && rw.w_rssi <= 0.25f);
            CHECK(rw.w_cls >= 0.25f && rw.w_cls <= 0.45f);
            CHECK(rw.tau_sel_s >= 60.0f && rw.tau_sel_s <= 180.0f);
        }
    }

    TEST_SUMMARY();
}
