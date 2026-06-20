// classifier.c — class inference + replay-eligibility gate
#include "classifier.h"
#include "afterglow_config.h"
#include "ag_core/ag_eligible.h" // portable eligibility predicate (host-tested)
#include "esp_log.h"

static const char *TAG = "classifier";

// Map a pool record class to ag_core's eligibility class.
static ag_elig_class_t elig_class_of(uint8_t cls)
{
    switch (cls) {
        case AG_CLASS_STATIC_RANDOM_BLE: return AG_ELIG_STATIC_RANDOM;
        case AG_CLASS_NRPA_BLE:          return AG_ELIG_NRPA;
        case AG_CLASS_RPA_BLE:           return AG_ELIG_RPA;
        case AG_CLASS_PUBLIC_BLE:        return AG_ELIG_PUBLIC;
        case AG_CLASS_WIFI:              return AG_ELIG_WIFI_BSSID;
        default:                         return AG_ELIG_RPA; // TENTATIVE → not cloneable yet
    }
}

void classifier_observe(ag_beacon_record_t *rec)
{
    (void)rec;
    // TODO(P1): infer class from payload structure + behavior (e.g. address
    // rotation cadence distinguishes RPA from static-random) over
    // STABLE_SIGHTINGS=3; promote cls; set AG_FLAG_REPLAY_ELIGIBLE when stable
    // AND reproducible. Never branch on the address random/public byte alone.
    ESP_LOGD(TAG, "observe TODO");
}

ag_beacon_class_t classifier_class_of(const ag_beacon_record_t *rec)
{
    return rec ? (ag_beacon_class_t)rec->cls : AG_CLASS_TENTATIVE;
}

bool classifier_replay_eligible(const ag_beacon_record_t *rec)
{
    if (!rec) return false;
    // Delegate to the host-tested predicate (reproducible address + broadcast
    // only adv kind).
    ag_elig_policy_t pol = ag_elig_defaults();
    bool sightings_ok = (rec->flags & AG_FLAG_REPLAY_ELIGIBLE) != 0;
    bool is_wifi = (rec->cls == AG_CLASS_WIFI);
    // TODO(P2): carry the observed adv-kind on the record; until then a non-Wi-Fi
    // eligible record is assumed broadcast-only.
    ag_adv_kind_t kind = AG_ADV_NONCONN_NONSCAN;
    // TODO(P3): pass real own-device exclusion; false for now.
    return ag_replay_eligible(&pol, elig_class_of(rec->cls), kind,
                              is_wifi, sightings_ok, /*is_own_device=*/false);
}
