// classifier.h — beacon-class inference.
//
// Infers a record's class (static-random / NRPA / RPA / public / wifi) from
// PAYLOAD and BEHAVIOR over observations — NEVER from the raw address byte
// (that would just echo the air, not prove cloneability). A source is promoted
// to REPLAY_ELIGIBLE only after replay_min_sightings (default 3) and only if
// its address is reproducible (static-random/NRPA); RPA/PUBLIC are gated out.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "pool.h" // ag_beacon_record_t, ag_beacon_class_t

#ifdef __cplusplus
extern "C" {
#endif

// Load config (min_sightings, eligibility policy). Call once at boot.
esp_err_t classifier_init(void);

// Update classification state for a record on a new sighting; may promote
// cls and set AG_FLAG_REPLAY_ELIGIBLE once stable.
void classifier_observe(ag_beacon_record_t *rec);

// Current inferred class for a record.
ag_beacon_class_t classifier_class_of(const ag_beacon_record_t *rec);

// True if the record's address is reproducible and eligible for replay
// (static-random/NRPA, >= min_sightings, not own-device).
bool classifier_replay_eligible(const ag_beacon_record_t *rec);

#ifdef __cplusplus
}
#endif
