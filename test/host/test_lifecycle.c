// test_lifecycle.c — portable rotation/departure decisions.
#include "lifecycle_logic.h"
#include "test_util.h"
#include <string.h>

// a record with a 1000 ms cadence (interval_q = 1600 TU * 0.625 = 1000 ms).
static ag_beacon_record_t mk_rec(void)
{
    ag_beacon_record_t r;
    memset(&r, 0, sizeof(r));
    r.cls = AG_CLASS_STATIC_RANDOM_BLE;
    r.interval_q = 1600;          // -> 1000 ms
    r.rssi_ewma = -70;
    r.rssi_last = -70;
    r.first_seen_ms = 0;
    r.last_seen_ms = 0;
    r.obs_count = 4;
    r.p_virt = 4.0f;
    r.p_center = 0.0f;
    uint8_t addr[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    memcpy(r.orig_addr, addr, 6);
    uint8_t pl[5] = {0x02, 0x01, 0x06, 0xAB, 0xCD};
    memcpy(r.payload, pl, 5);
    r.payload_len = 5;
    return r;
}

int main(void)
{
    TEST_BEGIN("lifecycle");
    ag_prng_t rng; ag_prng_seed(&rng, 0xA17E1Fu);

    // --- interval conversion (0.625 ms TU) -------------------------------
    CHECK(ag_life_interval_ms(1600) == 1000);
    CHECK(ag_life_interval_ms(0) == 0);

    const uint32_t WINDOW = 600000u; // 10 min own-device window
    const uint8_t GAP_MULT = 5;      // gap = 5 * 1000 ms = 5000 ms

    // --- own-device detection --------------------------------------------
    // (a) strong + continuous over the window + recent -> true.
    ag_beacon_record_t own = mk_rec();
    own.rssi_ewma = -30;
    own.first_seen_ms = 1000;
    own.last_seen_ms = 1000 + WINDOW;     // spanned the whole window
    CHECK_MSG(ag_life_is_own_device(&own, 1000 + WINDOW, WINDOW),
              "strong continuous co-located gear not detected");

    // (b) weak signal -> false even when continuously present.
    ag_beacon_record_t weak = own;
    weak.rssi_ewma = -60;
    CHECK(!ag_life_is_own_device(&weak, 1000 + WINDOW, WINDOW));

    // (c) strong but short presence span -> false.
    ag_beacon_record_t brief = mk_rec();
    brief.rssi_ewma = -20;
    brief.first_seen_ms = 0;
    brief.last_seen_ms = WINDOW / 2;      // half the window
    CHECK(!ag_life_is_own_device(&brief, WINDOW / 2, WINDOW));

    // (d) strong + long span but gone stale (not recent) -> false.
    ag_beacon_record_t stale = own;
    CHECK(!ag_life_is_own_device(&stale, 1000 + WINDOW + WINDOW + 1, WINDOW));

    // --- departure detection ---------------------------------------------
    ag_beacon_record_t r = mk_rec();
    r.last_seen_ms = 10000;
    // within the 5000 ms gap -> present.
    CHECK(!ag_life_departed(&r, 10000 + 4000, GAP_MULT));
    // just past the gap -> departed.
    CHECK(ag_life_departed(&r, 10000 + 5001, GAP_MULT));

    // --- tick transitions: NONE -> DEPARTING -> (grace) -> EXPIRE ----------
    ag_beacon_record_t t = mk_rec();
    t.last_seen_ms = 10000;
    float p_before = t.p_virt;

    // present: no change.
    CHECK(ag_life_tick_record(&t, 10000 + 1000, GAP_MULT, &rng) == AG_LIFE_NONE);
    CHECK(!(t.flags & AG_FLAG_DEPARTING));
    CHECK(t.p_virt == p_before);

    // crossed the gap: flagged + faded toward center.
    ag_life_action_t a = ag_life_tick_record(&t, 10000 + 6000, GAP_MULT, &rng);
    CHECK_MSG(a == AG_LIFE_DEPARTING, "expected DEPARTING, got %d", (int)a);
    CHECK(t.flags & AG_FLAG_DEPARTING);
    CHECK_MSG(t.p_virt < p_before, "p_virt did not fade: %g", (double)t.p_virt);

    // still inside the post-departure grace window (gap+1s < gap+2min): NONE.
    CHECK(ag_life_tick_record(&t, 10000 + 5000 + 1000, GAP_MULT, &rng)
          == AG_LIFE_NONE);

    // well past gap + max grace (12 min = 720000 ms): EXPIRE.
    uint32_t far = 10000 + 5000 + 720000u + 1000;
    CHECK_MSG(ag_life_tick_record(&t, far, GAP_MULT, &rng) == AG_LIFE_EXPIRE,
              "expected EXPIRE past grace window");

    // --- rotation successor ----------------------------------------------
    ag_beacon_record_t parent = mk_rec();
    parent.rssi_ewma = -52;
    parent.p_virt = -3.5f;
    parent.p_center = -2.0f;
    parent.first_seen_ms = 30000u;   // original device first seen at t=30s
    parent.base_ttl_s = 600.0f;      // lineage TTL basis
    parent.ttl_cap_s = 900.0f;
    parent.replay_deadline_ms = 123456u;
    ag_beacon_record_t child;
    ag_life_make_successor(&parent, &child, 0x12345678u, 99000u);

    // continuous RSSI: walk state + observed levels carry over.
    CHECK(child.p_virt == parent.p_virt);
    CHECK(child.p_center == parent.p_center);
    CHECK(child.rssi_ewma == parent.rssi_ewma);

    // payload copied verbatim.
    CHECK(child.payload_len == parent.payload_len);
    CHECK(memcmp(child.payload, parent.payload, parent.payload_len) == 0);

    // address differs (new identity), presence reset, not departing, id cleared.
    CHECK_MSG(memcmp(child.orig_addr, parent.orig_addr, 6) != 0,
              "successor reused the parent address");
    CHECK(child.obs_count == 1);
    CHECK(!(child.flags & AG_FLAG_DEPARTING));
    CHECK(child.rec_id == 0);

    // last_seen tracks the new sighting; first_seen does NOT — the lineage ages
    // from the original device's first sighting so the TTL deadline is shared.
    CHECK(child.last_seen_ms == 99000u);
    CHECK_MSG(child.first_seen_ms == parent.first_seen_ms,
              "successor reset the lineage age clock (first_seen_ms=%u, want %u)",
              child.first_seen_ms, parent.first_seen_ms);
    CHECK_MSG(child.base_ttl_s == parent.base_ttl_s &&
              child.ttl_cap_s == parent.ttl_cap_s &&
              child.replay_deadline_ms == parent.replay_deadline_ms,
              "successor did not inherit the lineage TTL basis");

    // successor address is a Non-Resolvable Private Address: subtype bits (top two
    // of the MSB) are 0b00 — never 0b01 (RPA) or 0b10 (reserved).
    CHECK_MSG((child.orig_addr[0] & 0xC0) == 0x00,
              "successor address is not NRPA-class (msb=0x%02x)", child.orig_addr[0]);

    // distinct seeds yield distinct, still-NRPA addresses (deterministic).
    ag_beacon_record_t c2;
    ag_life_make_successor(&parent, &c2, 0x12345679u, 99000u);
    CHECK(memcmp(c2.orig_addr, child.orig_addr, 6) != 0);
    CHECK((c2.orig_addr[0] & 0xC0) == 0x00);

    // every seed yields an NRPA: sweep a range of seeds and check the subtype bits.
    for (uint32_t s = 1; s <= 256; s++) {
        ag_beacon_record_t cs;
        ag_life_make_successor(&parent, &cs, s * 2654435761u, 99000u);
        CHECK_MSG((cs.orig_addr[0] & 0xC0) == 0x00,
                  "seed %u produced a non-NRPA successor (msb=0x%02x)",
                  s, cs.orig_addr[0]);
    }

    // --- rotation mode (§A6.3) -------------------------------------------
    // Only the NRPA class rotates; everything cloneable else holds.
    CHECK(ag_life_rotation_mode(AG_CLASS_NRPA_BLE) == AG_LIFE_ROTATING);
    CHECK(ag_life_rotation_mode(AG_CLASS_STATIC_RANDOM_BLE) == AG_LIFE_STATIONARY_HOLD);
    CHECK(ag_life_rotation_mode(AG_CLASS_PUBLIC_BLE) == AG_LIFE_STATIONARY_HOLD);
    CHECK(ag_life_rotation_mode(AG_CLASS_WIFI) == AG_LIFE_STATIONARY_HOLD);
    CHECK(ag_life_rotation_mode(AG_CLASS_TENTATIVE) == AG_LIFE_STATIONARY_HOLD);
    CHECK(ag_life_rotation_mode(AG_CLASS_RPA_BLE) == AG_LIFE_STATIONARY_HOLD);

    // --- rotation period draw: log-normal clamped to [1 min, 1 h] ---------
    {
        uint32_t lo = 1u * 60u * 1000u, hi = 60u * 60u * 1000u;
        uint64_t sum = 0; int N = 4000;
        for (int k = 0; k < N; k++) {
            uint32_t ms = ag_life_draw_rotate_ms(&rng);
            CHECK_MSG(ms >= lo && ms <= hi,
                      "rotate period %u out of [%u,%u]", ms, lo, hi);
            sum += ms;
        }
        uint32_t mean = (uint32_t)(sum / (uint64_t)N);
        // log-normal located at 15 min — mean sits modestly above the median;
        // just assert it lands in a sane minutes-scale band (not pinned to a clamp).
        CHECK_MSG(mean > 10u * 60u * 1000u && mean < 30u * 60u * 1000u,
                  "rotate-period mean %u ms outside the expected band", mean);
    }

    // --- rotation-due predicate ------------------------------------------
    {
        ag_beacon_record_t due = mk_rec();
        due.next_rotate_ms = 0;               // unscheduled -> never due
        CHECK(!ag_life_rotation_due(&due, 1000000u));
        due.next_rotate_ms = 50000u;
        CHECK(!ag_life_rotation_due(&due, 49999u));
        CHECK(ag_life_rotation_due(&due, 50000u));
        CHECK(ag_life_rotation_due(&due, 60000u));
    }

    // --- rotate-in-place sequence: lineage ages, address churns to NRPA,
    //     never triggered by departure -------------------------------------
    {
        ag_beacon_record_t g = mk_rec();
        g.cls = AG_CLASS_NRPA_BLE;
        g.orig_addr[0] = 0x00;               // NRPA parent
        g.first_seen_ms = 10000u;
        g.base_ttl_s = 600.0f;
        g.last_seen_ms = 200000u;            // present (not departed)
        uint32_t parent_first = g.first_seen_ms;
        float parent_ttl = g.base_ttl_s;
        uint8_t prev_addr[6];
        memcpy(prev_addr, g.orig_addr, 6);

        // simulate three rotations
        for (int k = 0; k < 3; k++) {
            ag_beacon_record_t s;
            ag_life_make_successor(&g, &s, ag_prng_u32(&rng), 200000u + k * 1000u);
            s.next_rotate_ms = (200000u + k * 1000u) + ag_life_draw_rotate_ms(&rng);
            g = s;
            CHECK_MSG((g.orig_addr[0] & 0xC0) == 0x00, "rotated addr not NRPA");
            CHECK_MSG(memcmp(g.orig_addr, prev_addr, 6) != 0, "rotation reused address");
            CHECK_MSG(g.first_seen_ms == parent_first, "rotation reset lineage age");
            CHECK_MSG(g.base_ttl_s == parent_ttl, "rotation changed lineage TTL");
            memcpy(prev_addr, g.orig_addr, 6);
        }
    }

    TEST_SUMMARY();
}
