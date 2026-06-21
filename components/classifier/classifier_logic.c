// classifier_logic.c — portable beacon-class inference (host-tested in test_classifier.c).
#include "classifier_logic.h"
#include <string.h>

// BLE address types as observed on the wire: 0 = public, 1 = random.
enum { AG_ADDR_PUBLIC = 0, AG_ADDR_RANDOM = 1 };

// Captured BLE frames (and the stored payload) are AdvA(6) || AdvData; the AD
// list starts after the address. Matches the marshalling in radio_single.c and
// the identity extraction in pool.c.
enum { AG_ADVA_LEN = 6 };

ag_rand_subtype_t ag_classify_rand_subtype(const uint8_t orig_addr[6])
{
    // Top two bits of the most-significant address byte carry the random subtype.
    // orig_addr holds the AdvA as the backend surfaces it (esp_bd_addr_t order,
    // most-significant octet first), so the subtype byte is orig_addr[0] — the
    // same octet handed to esp_ble_gap_set_rand_addr on the emit/clone path.
    return (ag_rand_subtype_t)((orig_addr[0] >> 6) & 0x3u);
}

bool ag_classify_beacon_payload(const uint8_t *payload, uint8_t payload_len)
{
    // Walk the AdvData length/type structures. A broadcast beacon carries either
    // a manufacturer-specific (0xFF) or a service-data (0x16 / 0x21) AD field.
    // We only read the AD framing bytes here — never the address.
    if (payload == NULL || payload_len < 3) return false;
    uint8_t i = 0;
    while (i + 1 < payload_len) {
        uint8_t len = payload[i];
        if (len == 0) break;                       // padding / end of AD list
        if (i + 1 + len > payload_len) break;      // truncated field
        uint8_t type = payload[i + 1];
        if (type == 0xFF || type == 0x16 || type == 0x21) return true;
        i = (uint8_t)(i + 1 + len);
    }
    return false;
}

bool ag_classify_class_reproducible(uint8_t cls)
{
    // Only addresses we can re-emit byte-for-byte: static-random, NRPA, Wi-Fi.
    return cls == AG_CLASS_STATIC_RANDOM_BLE ||
           cls == AG_CLASS_NRPA_BLE ||
           cls == AG_CLASS_WIFI;
}

ag_elig_class_t ag_classify_elig_class(uint8_t cls)
{
    switch (cls) {
        case AG_CLASS_STATIC_RANDOM_BLE: return AG_ELIG_STATIC_RANDOM;
        case AG_CLASS_NRPA_BLE:          return AG_ELIG_NRPA;
        case AG_CLASS_RPA_BLE:           return AG_ELIG_RPA;
        case AG_CLASS_PUBLIC_BLE:        return AG_ELIG_PUBLIC;
        case AG_CLASS_WIFI:              return AG_ELIG_WIFI_BSSID;
        case AG_CLASS_TENTATIVE:
        default:
            // Unpromoted: treat as RPA for the gate so it never clones early.
            return AG_ELIG_RPA;
    }
}

void ag_classify_observe(ag_beacon_record_t *rec, uint8_t min_sightings,
                         bool require_beacon_payload)
{
    if (rec == NULL) return;

    bool stable = rec->obs_count >= min_sightings;

    if (rec->proto == AG_PROTO_WIFI) {
        // Wi-Fi: classified by protocol; BSSID is an arbitrary cloneable MAC.
        rec->cls = AG_CLASS_WIFI;
    } else if (rec->addr_type == AG_ADDR_PUBLIC) {
        // Public address: not cloneable as a mismatched clone, but the class is
        // known immediately from addr_type (no behaviour inference needed).
        rec->cls = AG_CLASS_PUBLIC_BLE;
    } else {
        // Random BLE. The subtype bits are a hint, not proof; promotion out of
        // TENTATIVE additionally requires a recognized beacon payload and a
        // persistent address over >= min_sightings.
        ag_rand_subtype_t sub = ag_classify_rand_subtype(rec->orig_addr);
        // The stored payload is the captured frame: AdvA(6) || AdvData. The AD
        // list begins after the 6-byte address, so the beacon-framing walk must
        // start there — walking from byte 0 reads the address bytes as bogus AD
        // length/type fields and almost never recognizes a real beacon.
        bool beacon_like = false;
        if (rec->payload_len > AG_ADVA_LEN) {
            beacon_like = ag_classify_beacon_payload(
                rec->payload + AG_ADVA_LEN,
                (uint8_t)(rec->payload_len - AG_ADVA_LEN));
        }
        // Payload-class requirement: a recognized broadcast-beacon payload is
        // required for promotion unless the policy relaxes it. When not required,
        // the requirement is satisfied so subtype + persistence alone promote.
        // Default-true preserves the strict behavior.
        bool payload_ok = beacon_like || !require_beacon_payload;

        if (sub == AG_RANDSUB_RPA) {
            // Resolvable-private addresses rotate faster than sightings can
            // accumulate: each rotation mints a new identity, so obs_count for
            // any one address stays low. Keep RPA regardless of obs_count.
            rec->cls = AG_CLASS_RPA_BLE;
        } else if (sub == AG_RANDSUB_NRPA) {
            // Non-resolvable (0b00): promote to NRPA only once persistent +
            // payload_ok. Re-emittable: the controller accepts a 0b00 address.
            if (stable && payload_ok) rec->cls = AG_CLASS_NRPA_BLE;
            else if (rec->cls != AG_CLASS_NRPA_BLE) rec->cls = AG_CLASS_TENTATIVE;
        } else if (sub == AG_RANDSUB_STATIC) {
            // Static-random (0b11): promote once a persistent address with a
            // satisfied payload requirement is confirmed. Re-emittable: the
            // controller accepts a 0b11 address.
            if (stable && payload_ok) rec->cls = AG_CLASS_STATIC_RANDOM_BLE;
            else if (rec->cls != AG_CLASS_STATIC_RANDOM_BLE) rec->cls = AG_CLASS_TENTATIVE;
        } else {
            // Reserved (0b10): not a valid settable random address type, so it
            // can never be re-emitted as a matching address. Leave it
            // unpromoted (TENTATIVE) so the reproducibility gate excludes it.
            rec->cls = AG_CLASS_TENTATIVE;
        }
    }

    // adv_kind is the OBSERVED PDU behavior, owned by the capture/merge path
    // (radio_single reads it from the adv-report event type; pool_admit keeps
    // the stickiest-unsafe value across resightings). The classifier refines the
    // address class, never the kind — overwriting it here would re-open the
    // fail-closed gate, so it is left untouched. A connectable/scannable source,
    // or one whose kind is still AG_ADV_UNKNOWN, is not broadcast-only and is
    // therefore ineligible below.

    // Eligibility flag: only when persistent AND the class is reproducible AND
    // the advertisement is broadcast-only. Otherwise clear it.
    bool kind_ok = (rec->proto == AG_PROTO_WIFI) ||
                   (rec->adv_kind == AG_ADV_NONCONN_NONSCAN);
    if (stable && ag_classify_class_reproducible(rec->cls) && kind_ok) {
        rec->flags |= AG_FLAG_REPLAY_ELIGIBLE;
    } else {
        rec->flags &= (uint8_t)~AG_FLAG_REPLAY_ELIGIBLE;
    }
}
