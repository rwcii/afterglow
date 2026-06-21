// test_adv_kind.c — observed advertising-kind determination, end to end.
//
// The other tests pin individual stages: test_eligibility drives the predicate
// with an adv-kind handed in directly, and test_classifier constructs records
// with adv_kind preset. Neither exercises the DETERMINATION of adv_kind from a
// captured frame — the path that decides, from raw capture data, whether a
// source advertises broadcast-only. That determination gates replay: only
// non-connectable, non-scannable advertisements may be re-emitted with a
// matching address.
//
// Here we feed synthetic ag_capture_t observations — as the radio backend
// surfaces them, with adv_kind read from the adv-report event type — through the
// real ag_pool_admit + ag_classify_observe, then apply the production
// eligibility composition (classifier.c::classifier_replay_eligible). We assert:
//   - a connectable source is never eligible, however many stable sightings;
//   - a source seen connectable even once stays ineligible despite later
//     broadcast-only sightings (stickiest-unsafe-wins merge);
//   - an unparsed (AG_ADV_UNKNOWN) source fails closed;
//   - a genuinely broadcast-only source still becomes eligible (no over-block);
//   - AG_ADV_UNKNOWN yields to a real broadcast-only observation.
#include "pool_logic.h"
#include "classifier_logic.h"
#include "ag_core/ag_eligible.h"
#include "test_util.h"
#include <string.h>

#define MIN_SIGHT 3
#define CAP 8

// A recognized broadcast-beacon AdvData: one manufacturer-specific (0xFF) field.
// Placed after the 6-byte AdvA so the pool's identity extraction (frame[0..5])
// and the classifier's payload framing (offset 6) both see what they expect.
static const uint8_t BEACON_AD[] = { 0x05, 0xFF, 0x4C, 0x00, 0x02, 0x15 };

// Build a BLE capture: AdvA (msb in byte 5 sets the random subtype) || BEACON_AD,
// with the observed PDU behavior the backend would have read off the wire.
static ag_capture_t mk_ble_cap(uint8_t msb, ag_adv_kind_t kind, int8_t rssi,
                               uint64_t ts_us, uint8_t *frame_out)
{
    frame_out[0] = 0x11; frame_out[1] = 0x22; frame_out[2] = 0x33;
    frame_out[3] = 0x44; frame_out[4] = 0x55; frame_out[5] = msb;
    memcpy(frame_out + 6, BEACON_AD, sizeof BEACON_AD);

    ag_capture_t c = {0};
    c.proto = AG_PROTO_BLE;
    c.rssi = rssi;
    c.channel = 37;
    c.ts_us = ts_us;
    c.frame = frame_out;
    c.frame_len = (uint16_t)(6 + sizeof BEACON_AD);
    c.adv_kind = kind;
    return c;
}

// The production eligibility composition (classifier.c::classifier_replay_eligible):
// the per-record flag set by ag_classify_observe, AND the policy gate, against
// the record's observed class + adv_kind.
static bool eligible(const ag_beacon_record_t *rec)
{
    ag_elig_policy_t pol = ag_elig_defaults();
    bool sightings_ok = (rec->flags & AG_FLAG_REPLAY_ELIGIBLE) != 0;
    bool is_wifi = (rec->cls == AG_CLASS_WIFI);
    return ag_replay_eligible(&pol, ag_classify_elig_class(rec->cls),
                              (ag_adv_kind_t)rec->adv_kind, is_wifi,
                              sightings_ok, /*is_own_device=*/false);
}

// Drive one capture all the way through admit + classify, returning the touched
// record. addr_type=1 (random) so subtype bits in the AdvA MSB are honored.
static ag_beacon_record_t *feed(ag_beacon_record_t *slab, uint16_t *count,
                                ag_prng_t *rng, const ag_evict_params_t *evp,
                                const ag_capture_t *cap, uint32_t now_ms)
{
    uint8_t addr[6];
    memcpy(addr, cap->frame, 6);
    int idx = ag_pool_admit(slab, count, CAP, cap, addr, /*addr_type=*/1,
                            now_ms, /*node_id=*/0, evp, rng);
    if (idx < 0) return NULL;
    ag_classify_observe(&slab[idx], MIN_SIGHT, /*require_beacon_payload=*/true);
    return &slab[idx];
}

int main(void)
{
    TEST_BEGIN("adv_kind");
    ag_prng_t rng; ag_prng_seed(&rng, 0xA1A1A1A1u);
    ag_evict_params_t evp = ag_evict_defaults();
    uint8_t frame[6 + sizeof BEACON_AD];

    // --- (1) connectable source: never eligible, however many stable sightings.
    //     A connectable ADV_IND (the common phone/wearable case) must not be
    //     treated as broadcast-only by the predicate.
    {
        ag_beacon_record_t slab[CAP]; uint16_t count = 0;
        ag_beacon_record_t *r = NULL;
        for (int n = 0; n < 6; n++) {
            ag_capture_t c = mk_ble_cap(0xC0 /*static-random*/, AG_ADV_CONNECTABLE,
                                        -50, 1000u * (n + 1), frame);
            r = feed(slab, &count, &rng, &evp, &c, 1000u * (n + 1));
        }
        CHECK(r != NULL);
        CHECK_MSG(r->adv_kind == AG_ADV_CONNECTABLE,
                  "observed connectable kind was lost (adv_kind=%u)", r->adv_kind);
        CHECK_MSG(!eligible(r),
                  "a connectable source was marked replay-eligible");
    }

    // --- (2) stickiest-unsafe-wins: one connectable sighting fixes the identity
    //     as connectable, even if every later sighting is broadcast-only.
    {
        ag_beacon_record_t slab[CAP]; uint16_t count = 0;
        // First sighting: connectable.
        ag_capture_t c0 = mk_ble_cap(0xC0, AG_ADV_CONNECTABLE, -50, 1000, frame);
        feed(slab, &count, &rng, &evp, &c0, 1000);
        // Many later sightings: broadcast-only.
        ag_beacon_record_t *r = NULL;
        for (int n = 1; n < 8; n++) {
            ag_capture_t c = mk_ble_cap(0xC0, AG_ADV_NONCONN_NONSCAN, -50,
                                        1000u * (n + 1), frame);
            r = feed(slab, &count, &rng, &evp, &c, 1000u * (n + 1));
        }
        CHECK_MSG(r->adv_kind == AG_ADV_CONNECTABLE,
                  "a broadcast-only resighting relaxed a connectable source "
                  "(adv_kind=%u)", r->adv_kind);
        CHECK_MSG(!eligible(r),
                  "a once-connectable source became eligible after the fact");
    }

    // --- (3) unparsed PDU behavior fails closed: AG_ADV_UNKNOWN is not
    //     broadcast-only, so the source is never eligible.
    {
        ag_beacon_record_t slab[CAP]; uint16_t count = 0;
        ag_beacon_record_t *r = NULL;
        for (int n = 0; n < 6; n++) {
            ag_capture_t c = mk_ble_cap(0xC0, AG_ADV_UNKNOWN, -50,
                                        1000u * (n + 1), frame);
            r = feed(slab, &count, &rng, &evp, &c, 1000u * (n + 1));
        }
        CHECK(r->adv_kind == AG_ADV_UNKNOWN);
        CHECK_MSG(!eligible(r), "an unparsed-kind source was eligible (fail-open)");
    }

    // --- (4) no over-block: a genuinely broadcast-only static-random beacon
    //     still becomes eligible once stable (the gate must not break the
    //     legitimate replay path).
    {
        ag_beacon_record_t slab[CAP]; uint16_t count = 0;
        ag_beacon_record_t *r = NULL;
        for (int n = 0; n < 6; n++) {
            ag_capture_t c = mk_ble_cap(0xC0, AG_ADV_NONCONN_NONSCAN, -50,
                                        1000u * (n + 1), frame);
            r = feed(slab, &count, &rng, &evp, &c, 1000u * (n + 1));
        }
        CHECK(r->adv_kind == AG_ADV_NONCONN_NONSCAN);
        CHECK(r->cls == AG_CLASS_STATIC_RANDOM_BLE);
        CHECK_MSG(eligible(r),
                  "a broadcast-only beacon was wrongly blocked (over-block)");
    }

    // --- (5) UNKNOWN yields to a real broadcast-only observation: a first
    //     unparsed sighting must not permanently block an otherwise-clean source.
    {
        ag_beacon_record_t slab[CAP]; uint16_t count = 0;
        ag_capture_t c0 = mk_ble_cap(0xC0, AG_ADV_UNKNOWN, -50, 1000, frame);
        feed(slab, &count, &rng, &evp, &c0, 1000);
        ag_beacon_record_t *r = NULL;
        for (int n = 1; n < 8; n++) {
            ag_capture_t c = mk_ble_cap(0xC0, AG_ADV_NONCONN_NONSCAN, -50,
                                        1000u * (n + 1), frame);
            r = feed(slab, &count, &rng, &evp, &c, 1000u * (n + 1));
        }
        CHECK_MSG(r->adv_kind == AG_ADV_NONCONN_NONSCAN,
                  "UNKNOWN did not yield to a real broadcast-only sighting "
                  "(adv_kind=%u)", r->adv_kind);
        CHECK(eligible(r));
    }

    // --- (6) merge helper, the kernel of stickiest-unsafe-wins, directly.
    {
        CHECK(ag_adv_kind_merge(AG_ADV_UNKNOWN, AG_ADV_NONCONN_NONSCAN)
              == AG_ADV_NONCONN_NONSCAN);
        CHECK(ag_adv_kind_merge(AG_ADV_NONCONN_NONSCAN, AG_ADV_UNKNOWN)
              == AG_ADV_NONCONN_NONSCAN);
        CHECK(ag_adv_kind_merge(AG_ADV_NONCONN_NONSCAN, AG_ADV_CONNECTABLE)
              == AG_ADV_CONNECTABLE);
        CHECK(ag_adv_kind_merge(AG_ADV_CONNECTABLE, AG_ADV_NONCONN_NONSCAN)
              == AG_ADV_CONNECTABLE);
        CHECK(ag_adv_kind_merge(AG_ADV_SCANNABLE, AG_ADV_CONNECTABLE)
              == AG_ADV_CONNECTABLE);
        CHECK(ag_adv_kind_merge(AG_ADV_UNKNOWN, AG_ADV_UNKNOWN) == AG_ADV_UNKNOWN);
    }

    TEST_SUMMARY();
}
