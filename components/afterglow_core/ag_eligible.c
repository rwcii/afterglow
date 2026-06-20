// ag_eligible.c — replay-eligibility predicate.
#include "ag_core/ag_eligible.h"

ag_elig_policy_t ag_elig_defaults(void)
{
    ag_elig_policy_t p;
    // Conservative defaults: only relay broadcast-only advertisements, and keep
    // Wi-Fi beacon replay off until validated (matches the ship-disabled defaults).
    p.require_broadcast_only = true;
    p.wifi_beacons_enabled = false;
    return p;
}

bool ag_replay_class_cloneable(ag_elig_class_t cls)
{
    switch (cls) {
        case AG_ELIG_STATIC_RANDOM:
        case AG_ELIG_NRPA:
        case AG_ELIG_WIFI_BSSID:
            return true;
        case AG_ELIG_RPA:    // self-rotating address; not reproducible
        case AG_ELIG_PUBLIC: // address type would not match on re-emission
        default:
            return false;
    }
}

bool ag_replay_adv_safe(const ag_elig_policy_t *pol, ag_adv_kind_t kind)
{
    if (!pol->require_broadcast_only) {
        // Policy permits scannable/connectable replay (explicit opt-in).
        return true;
    }
    // Only broadcast-only (non-connectable, non-scannable) advertisements.
    return kind == AG_ADV_NONCONN_NONSCAN;
}

bool ag_replay_eligible(const ag_elig_policy_t *pol,
                        ag_elig_class_t cls,
                        ag_adv_kind_t kind,
                        bool is_wifi,
                        bool sightings_ok,
                        bool is_own_device)
{
    if (is_own_device) return false;            // exclude co-located operator gear
    if (!sightings_ok) return false;            // stable-sightings gate
    if (!ag_replay_class_cloneable(cls)) return false; // reproducible-address gate

    if (is_wifi) {
        if (!pol->wifi_beacons_enabled) return false;  // master switch (default off)
        return true;  // a beacon is inherently broadcast; adv-kind gate is BLE-only
    }

    // BLE: broadcast-only advertisements only.
    return ag_replay_adv_safe(pol, kind);
}
