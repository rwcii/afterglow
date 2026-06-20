// classifier_logic.h — portable beacon-class inference + sightings gate.
//
// Infers a record's class from addr_type + payload/behavior over repeated
// sightings — never from the raw random/public address byte alone. A source is
// marked AG_FLAG_REPLAY_ELIGIBLE only once its address is reproducible
// (static-random / NRPA / Wi-Fi) AND it has accumulated min_sightings.
// Pure C: no PSRAM / ESP deps, deterministic, no hidden state.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pool.h"               // ag_beacon_record_t, ag_beacon_class_t, flags
#include "radio_backend.h"      // ag_proto_t
#include "ag_core/ag_eligible.h" // ag_elig_class_t, ag_adv_kind_t

#ifdef __cplusplus
extern "C" {
#endif

// BLE random-address subtype, read from the top two bits of the MSB of a random
// address (orig_addr[5]). Used only as a rotation hint — promotion still
// requires payload/behavior evidence over sightings.
typedef enum {
    AG_RANDSUB_NRPA   = 0,  // 0b00 — non-resolvable private (cloneable)
    AG_RANDSUB_RPA    = 1,  // 0b01 — resolvable private, self-rotating
    AG_RANDSUB_RESVD  = 2,  // 0b10 — reserved
    AG_RANDSUB_STATIC = 3,  // 0b11 — static random (cloneable)
} ag_rand_subtype_t;

// Update classification state for a record on a new sighting. Reads proto,
// addr_type, orig_addr, payload, obs_count and adv_kind; may promote rec->cls
// and set/clear AG_FLAG_REPLAY_ELIGIBLE. Idempotent for a given record state.
void ag_classify_observe(ag_beacon_record_t *rec, uint8_t min_sightings);

// Map a pool class (ag_beacon_class_t) to the eligibility-gate class used by
// ag_replay_eligible().
ag_elig_class_t ag_classify_elig_class(uint8_t cls);

// Random-subtype hint from a random BLE address MSB (orig_addr[5]).
ag_rand_subtype_t ag_classify_rand_subtype(const uint8_t orig_addr[6]);

// True if a payload has the shape of a recognized broadcast-beacon structure
// (e.g. iBeacon / Eddystone-style manufacturer/service-data frame). Heuristic
// over the AdvData length/type bytes only — never inspects the address.
bool ag_classify_beacon_payload(const uint8_t *payload, uint8_t payload_len);

// True if rec->cls is a reproducible (cloneable) address class.
bool ag_classify_class_reproducible(uint8_t cls);

#ifdef __cplusplus
}
#endif
