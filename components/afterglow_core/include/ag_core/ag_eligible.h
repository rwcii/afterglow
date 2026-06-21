// ag_eligible.h — replay-eligibility predicate (portable).
//
// A captured beacon is eligible for replay only when both hold:
//
//   (A) Its address is reproducible: only static-random (0b11) and NRPA (0b00)
//       BLE addresses, and Wi-Fi BSSIDs, can be cloned. RPA (0b01) and PUBLIC
//       addresses are never emitted as mismatched-address clones.
//
//   (B) It is broadcast-only: non-connectable, non-scannable advertisement
//       types (e.g. iBeacon / Eddystone-style). Connectable/scannable adv and
//       live-AP Wi-Fi behaviors are excluded.
//
// Pure: no ESP deps.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// BLE advertising PDU type, by the connectable/scannable bits in the PDU header.
// Ordered safe→unsafe so the merge rule below can keep the most-unsafe kind ever
// observed under one identity (see ag_adv_kind_merge). AG_ADV_UNKNOWN is the
// fail-closed default carried until the capture path parses the real PDU header:
// it is NOT broadcast-only, so the predicate refuses it, yet it yields to the
// first genuine observation so a real broadcast-only source can still become
// eligible.
typedef enum {
    AG_ADV_NONCONN_NONSCAN = 0, // ADV_NONCONN_IND — broadcast only (eligible)
    AG_ADV_SCANNABLE,           // ADV_SCAN_IND
    AG_ADV_CONNECTABLE,         // ADV_IND / ADV_DIRECT_IND
    AG_ADV_UNKNOWN,             // not yet parsed — fail closed (ineligible)
} ag_adv_kind_t;

// Merge two observations of the same identity's PDU behavior: "stickiest-unsafe
// wins". A genuine unsafe observation (scannable/connectable) can never be
// downgraded by a later broadcast-only sighting; AG_ADV_UNKNOWN is the only kind
// a real observation replaces. Used on resighting/merge so a source that ever
// advertised connectably stays ineligible forever.
ag_adv_kind_t ag_adv_kind_merge(ag_adv_kind_t a, ag_adv_kind_t b);

// Address-type class for the reproducibility gate (mirrors pool.h classes).
typedef enum {
    AG_ELIG_STATIC_RANDOM = 0,  // 0b11 — cloneable
    AG_ELIG_NRPA,               // 0b00 — cloneable
    AG_ELIG_RPA,                // 0b01 — NOT cloneable
    AG_ELIG_PUBLIC,             // NOT cloneable as mismatched clone
    AG_ELIG_WIFI_BSSID,         // cloneable (arbitrary src MAC)
} ag_elig_class_t;

// Config knobs that gate eligibility (subset of afterglow_config_t).
typedef struct {
    bool require_broadcast_only; // refuse connectable/scannable adv
    bool wifi_beacons_enabled;   // Wi-Fi beacon replay master switch (default false)
} ag_elig_policy_t;

ag_elig_policy_t ag_elig_defaults(void);

// Address reproducible (cloneable)?
bool ag_replay_class_cloneable(ag_elig_class_t cls);

// Is this adv kind allowed to be replayed under the policy? True if allowed.
bool ag_replay_adv_safe(const ag_elig_policy_t *pol, ag_adv_kind_t kind);

// Combined replay-eligibility decision. `sightings_ok` is the stable-sightings
// gate result (>= min sightings); `is_own_device` excludes co-located operator
// gear; `source_present` is true while the original source is still being
// observed — a record is replayed only once its source is absent, so this
// returns false whenever source_present is true. Returns true only if ALL gates
// pass.
bool ag_replay_eligible(const ag_elig_policy_t *pol,
                        ag_elig_class_t cls,
                        ag_adv_kind_t kind,
                        bool is_wifi,
                        bool sightings_ok,
                        bool is_own_device,
                        bool source_present);

#ifdef __cplusplus
}
#endif
