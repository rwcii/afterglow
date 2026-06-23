// test_classifier.c — portable class-inference + sightings gate.
//
// Assert: TENTATIVE is not eligible before min_sightings and promotes after;
// RPA / PUBLIC never become eligible; Wi-Fi is gated by being WIFI class;
// connectable adv-kind is never eligible; the elig-class mapping is total.
#include "classifier_logic.h"
#include "test_util.h"
#include <string.h>

#define MIN_SIGHT 3

// A minimal recognized broadcast-beacon payload: one manufacturer-specific
// (0xFF) AD field. len=0x05, type=0xFF, then 4 company/data bytes.
static const uint8_t BEACON_AD[] = { 0x05, 0xFF, 0x4C, 0x00, 0x02, 0x15 };
// A payload with no manufacturer/service-data AD field (just a flags field).
static const uint8_t PLAIN_AD[]  = { 0x02, 0x01, 0x06 };

// Build a fresh random-BLE record. msb sets orig_addr[0], the most-significant
// AdvA octet whose top two bits carry the random subtype — orig_addr is stored
// MSB-first (esp_bd_addr_t order), matching the on-device capture path.
// The stored payload mirrors the captured frame layout used everywhere on-device:
// AdvA(6) || AdvData. The classifier reads beacon framing from after the AdvA,
// so the AD list must sit at offset 6 (not byte 0).
static ag_beacon_record_t mk_ble(uint8_t msb, ag_adv_kind_t kind,
                                 const uint8_t *ad, uint8_t adlen)
{
    ag_beacon_record_t r;
    memset(&r, 0, sizeof r);
    r.proto = AG_PROTO_BLE;
    r.addr_type = 1;                 // random
    r.cls = AG_CLASS_TENTATIVE;
    r.adv_kind = (uint8_t)kind;
    r.orig_addr[0] = msb;
    memcpy(r.payload, r.orig_addr, 6);       // AdvA prefix (as captured)
    memcpy(r.payload + 6, ad, adlen);        // AdvData follows
    r.payload_len = (uint8_t)(6 + adlen);
    return r;
}

static bool eligible(const ag_beacon_record_t *r)
{
    return (r->flags & AG_FLAG_REPLAY_ELIGIBLE) != 0;
}

// Simulate n sightings; obs_count saturates like the pool's merge path.
static void observe_n(ag_beacon_record_t *r, int n)
{
    for (int i = 0; i < n; i++) {
        if (r->obs_count < 255) r->obs_count++;
        ag_classify_observe(r, MIN_SIGHT, /*require_beacon_payload=*/true);
    }
}

int main(void)
{
    TEST_BEGIN("classifier");

    // --- (1) static-random beacon: TENTATIVE before threshold, promotes after.
    {
        ag_beacon_record_t r = mk_ble(0xC0, AG_ADV_NONCONN_NONSCAN, // 0b11 static
                                      BEACON_AD, sizeof BEACON_AD);
        observe_n(&r, MIN_SIGHT - 1);            // 2 sightings
        CHECK(r.cls == AG_CLASS_TENTATIVE);
        CHECK(!eligible(&r));
        observe_n(&r, 1);                        // 3rd sighting -> threshold
        CHECK_MSG(r.cls == AG_CLASS_STATIC_RANDOM_BLE,
                  "static-random promotes at min_sightings (cls=%u)", r.cls);
        CHECK(eligible(&r));
    }

    // --- (2) NRPA beacon promotes and is eligible after threshold.
    {
        ag_beacon_record_t r = mk_ble(0x00, AG_ADV_NONCONN_NONSCAN, // 0b00 NRPA
                                      BEACON_AD, sizeof BEACON_AD);
        observe_n(&r, MIN_SIGHT - 1);
        CHECK(r.cls == AG_CLASS_TENTATIVE && !eligible(&r));
        observe_n(&r, 1);
        CHECK(r.cls == AG_CLASS_NRPA_BLE);
        CHECK(eligible(&r));
    }

    // --- (3) RPA: never promotes to a reproducible class, never eligible,
    //         no matter how many sightings (0b01 subtype bits).
    {
        ag_beacon_record_t r = mk_ble(0x40, AG_ADV_NONCONN_NONSCAN, // 0b01 RPA
                                      BEACON_AD, sizeof BEACON_AD);
        observe_n(&r, 10);
        CHECK_MSG(r.cls == AG_CLASS_RPA_BLE, "RPA stays RPA (cls=%u)", r.cls);
        CHECK_MSG(!eligible(&r), "RPA must never be replay-eligible");
        CHECK(ag_classify_elig_class(r.cls) == AG_ELIG_RPA);
        CHECK(!ag_classify_class_reproducible(r.cls));
    }

    // --- (3b) reserved subtype (0b10): not a settable random address type, so
    //          it must stay TENTATIVE and never become reproducible/eligible.
    {
        ag_beacon_record_t r = mk_ble(0x80, AG_ADV_NONCONN_NONSCAN, // 0b10 reserved
                                      BEACON_AD, sizeof BEACON_AD);
        observe_n(&r, 10);
        CHECK_MSG(r.cls == AG_CLASS_TENTATIVE,
                  "reserved subtype must not promote (cls=%u)", r.cls);
        CHECK_MSG(!eligible(&r), "reserved subtype must never be replay-eligible");
        CHECK(!ag_classify_class_reproducible(r.cls));
    }

    // --- (4) PUBLIC: classed immediately, never eligible.
    {
        ag_beacon_record_t r = mk_ble(0x00, AG_ADV_NONCONN_NONSCAN,
                                      BEACON_AD, sizeof BEACON_AD);
        r.addr_type = 0;                         // public
        observe_n(&r, 10);
        CHECK(r.cls == AG_CLASS_PUBLIC_BLE);
        CHECK_MSG(!eligible(&r), "PUBLIC must never be replay-eligible");
        CHECK(ag_classify_elig_class(r.cls) == AG_ELIG_PUBLIC);
    }

    // --- (5) Wi-Fi: gated purely by being WIFI class; eligible after threshold.
    {
        ag_beacon_record_t r;
        memset(&r, 0, sizeof r);
        r.proto = AG_PROTO_WIFI;
        r.cls = AG_CLASS_TENTATIVE;
        observe_n(&r, MIN_SIGHT - 1);
        CHECK(r.cls == AG_CLASS_WIFI);
        CHECK_MSG(!eligible(&r), "wifi not eligible before min_sightings");
        observe_n(&r, 1);
        CHECK(r.cls == AG_CLASS_WIFI);
        CHECK(eligible(&r));
        CHECK(ag_classify_elig_class(r.cls) == AG_ELIG_WIFI_BSSID);
    }

    // --- (6) connectable adv-kind: never eligible even when class is reproducible.
    {
        ag_beacon_record_t r = mk_ble(0xC0, AG_ADV_CONNECTABLE, // 0b11 static
                                      BEACON_AD, sizeof BEACON_AD);
        observe_n(&r, 10);
        // class may still be reproducible, but broadcast-only gate blocks it.
        CHECK(r.cls == AG_CLASS_STATIC_RANDOM_BLE);
        CHECK_MSG(!eligible(&r), "connectable adv must never be replay-eligible");
    }

    // --- (7) scannable adv-kind also blocked.
    {
        ag_beacon_record_t r = mk_ble(0xC0, AG_ADV_SCANNABLE,
                                      BEACON_AD, sizeof BEACON_AD);
        observe_n(&r, 10);
        CHECK(!eligible(&r));
    }

    // --- (8) non-beacon payload does not promote a static-random address.
    {
        ag_beacon_record_t r = mk_ble(0xC0, AG_ADV_NONCONN_NONSCAN,
                                      PLAIN_AD, sizeof PLAIN_AD);
        observe_n(&r, 10);
        CHECK_MSG(r.cls == AG_CLASS_TENTATIVE,
                  "no beacon payload -> stays tentative (cls=%u)", r.cls);
        CHECK(!eligible(&r));
    }

    // --- (9) payload classifier: framing-only, recognizes mfr/service-data.
    {
        CHECK(ag_classify_beacon_payload(BEACON_AD, sizeof BEACON_AD));
        CHECK(!ag_classify_beacon_payload(PLAIN_AD, sizeof PLAIN_AD));
        CHECK(!ag_classify_beacon_payload(NULL, 0));
        const uint8_t svc[] = { 0x03, 0x16, 0xAA, 0xBB };  // service-data 0x16
        CHECK(ag_classify_beacon_payload(svc, sizeof svc));
    }

    // --- (10) subtype hint reads only the address MSB (orig_addr[0]) top two
    // bits — the byte stored first under esp_bd_addr_t / MSB-first capture order.
    {
        uint8_t a[6] = {0xC0,0,0,0,0,0};
        CHECK(ag_classify_rand_subtype(a) == AG_RANDSUB_STATIC);
        a[0] = 0x00; CHECK(ag_classify_rand_subtype(a) == AG_RANDSUB_NRPA);
        a[0] = 0x40; CHECK(ag_classify_rand_subtype(a) == AG_RANDSUB_RPA);
        a[0] = 0x80; CHECK(ag_classify_rand_subtype(a) == AG_RANDSUB_RESVD);
    }

    // --- (11) elig-class mapping is total and never crashes on unknowns.
    {
        CHECK(ag_classify_elig_class(AG_CLASS_TENTATIVE) == AG_ELIG_RPA);
        CHECK(ag_classify_elig_class(99) == AG_ELIG_RPA);
    }

    // --- (12) determinism: re-observing a settled record does not flip state.
    {
        ag_beacon_record_t r = mk_ble(0x00, AG_ADV_NONCONN_NONSCAN,
                                      BEACON_AD, sizeof BEACON_AD);
        observe_n(&r, MIN_SIGHT);
        ag_beacon_record_t snap = r;
        ag_classify_observe(&r, MIN_SIGHT, true); // same obs_count, no new sighting
        CHECK(r.cls == snap.cls && r.flags == snap.flags);
    }

    TEST_SUMMARY();
}
