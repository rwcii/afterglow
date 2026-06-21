// test_eligibility.c — replay-eligibility predicate tests.
//
// With the broadcast-only policy enabled, asserts that the replay-eligibility
// predicate admits only broadcast-only (non-connectable, non-scannable)
// advertisements, refuses non-reproducible address classes (RPA / public), and
// that Wi-Fi eligibility obeys the wifi_beacons_enabled master switch.
#include "ag_core/ag_eligible.h"
#include "../test_util.h"

int main(void)
{
    TEST_BEGIN("eligibility");
    ag_elig_policy_t pol = ag_elig_defaults();

    // default policy is the conservative one.
    CHECK(pol.require_broadcast_only == true);
    CHECK(pol.wifi_beacons_enabled == false);

    // (1) with the filter on, only broadcast-only advertisements pass; a
    //     connectable or scannable adv never does, regardless of address class.
    CHECK(ag_replay_adv_safe(&pol, AG_ADV_NONCONN_NONSCAN) == true);
    CHECK(ag_replay_adv_safe(&pol, AG_ADV_SCANNABLE) == false);
    CHECK(ag_replay_adv_safe(&pol, AG_ADV_CONNECTABLE) == false);

    // (2) end-to-end: a reproducible static-random identity that passed the
    //     sightings gate is still refused when it is connectable or scannable.
    // source_present=false throughout (the absent-source condition) except where
    // the presence gate is the dimension under test in (8).
    CHECK_MSG(ag_replay_eligible(&pol, AG_ELIG_STATIC_RANDOM, AG_ADV_CONNECTABLE,
              /*is_wifi*/false, /*sightings_ok*/true, /*own*/false,
              /*present*/false) == false,
              "a connectable advertisement was marked eligible");
    CHECK_MSG(ag_replay_eligible(&pol, AG_ELIG_STATIC_RANDOM, AG_ADV_SCANNABLE,
              false, true, false, false) == false,
              "a scannable advertisement was marked eligible");

    // (3) the accepted path: broadcast-only, reproducible, sighted, not own,
    //     source absent.
    CHECK(ag_replay_eligible(&pol, AG_ELIG_STATIC_RANDOM, AG_ADV_NONCONN_NONSCAN,
          false, true, false, false) == true);
    CHECK(ag_replay_eligible(&pol, AG_ELIG_NRPA, AG_ADV_NONCONN_NONSCAN,
          false, true, false, false) == true);

    // (4) reproducibility gate: RPA/PUBLIC never eligible
    //     even when broadcast-only.
    CHECK(ag_replay_eligible(&pol, AG_ELIG_RPA, AG_ADV_NONCONN_NONSCAN,
          false, true, false, false) == false);
    CHECK(ag_replay_eligible(&pol, AG_ELIG_PUBLIC, AG_ADV_NONCONN_NONSCAN,
          false, true, false, false) == false);

    // (5) gates compose: own-device and un-sighted are always refused.
    CHECK(ag_replay_eligible(&pol, AG_ELIG_STATIC_RANDOM, AG_ADV_NONCONN_NONSCAN,
          false, /*sightings_ok*/false, false, false) == false);
    CHECK(ag_replay_eligible(&pol, AG_ELIG_STATIC_RANDOM, AG_ADV_NONCONN_NONSCAN,
          false, true, /*own*/true, false) == false);

    // (6) Wi-Fi obeys its master switch (ships disabled).
    CHECK(ag_replay_eligible(&pol, AG_ELIG_WIFI_BSSID, AG_ADV_NONCONN_NONSCAN,
          /*is_wifi*/true, true, false, false) == false);    // disabled by default
    pol.wifi_beacons_enabled = true;
    CHECK(ag_replay_eligible(&pol, AG_ELIG_WIFI_BSSID, AG_ADV_NONCONN_NONSCAN,
          true, true, false, false) == true);

    // (7) explicit opt-out: disabling the broadcast-only filter widens the
    //     predicate to admit scannable/connectable advertisements.
    pol = ag_elig_defaults();
    pol.require_broadcast_only = false;
    CHECK(ag_replay_adv_safe(&pol, AG_ADV_CONNECTABLE) == true);

    // (8) presence gate: an otherwise-accepted record is refused while its
    //     source is still present, and admitted once the source is absent. This
    //     holds independently of the other gates.
    pol = ag_elig_defaults();
    CHECK_MSG(ag_replay_eligible(&pol, AG_ELIG_STATIC_RANDOM, AG_ADV_NONCONN_NONSCAN,
              false, /*sightings_ok*/true, /*own*/false,
              /*present*/true) == false,
              "a present source was marked eligible");
    CHECK_MSG(ag_replay_eligible(&pol, AG_ELIG_STATIC_RANDOM, AG_ADV_NONCONN_NONSCAN,
              false, true, false, /*present*/false) == true,
              "an absent reproducible source was wrongly refused");

    TEST_SUMMARY();
}
