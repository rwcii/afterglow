// lifecycle_logic.h — portable rotation/departure decisions (host-tested).
//
// The hardware-coupled lifecycle.c walks the pool, owns the clock and RNG and
// writes successors back into PSRAM slots; the per-record decision math lives
// here so it can be host-tested against a plain stack record. Every entry point
// is pure and deterministic — the clock arrives as now_ms and randomness is
// threaded through an injected ag_prng_t.
//
// Two end-of-life models per ghost:
//   - DEPARTURE (default): the source stops appearing; fade p_virt and retire.
//   - ROTATION: the source behaved phone-like (rotated its address while
//     present); pair the retirement with a successor that carries a fresh
//     address but a CONTINUOUS RSSI trajectory (p_virt / p_center copied).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pool.h"               // ag_beacon_record_t, ag_beacon_class_t, flags
#include "ag_core/ag_prng.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tick outcome for one record.
typedef enum {
    AG_LIFE_NONE = 0,   // present (or already-departing within grace): no change
    AG_LIFE_DEPARTING,  // crossed the departure gap this tick: flagged + faded
    AG_LIFE_EXPIRE,     // past the post-departure grace window: retire the record
} ag_life_action_t;

// Estimated advertising cadence in ms from interval_q (0.625 ms TU units:
// ms = q * 5 / 8). Returns 0 when interval_q is 0 (cadence not yet estimated).
uint32_t ag_life_interval_ms(uint16_t interval_q);

// True when the record looks like co-located operator gear that must never be
// replayed: strong signal (rssi_ewma > -45 dBm) AND continuously present across
// window_ms (first_seen..last_seen span >= window) AND still recent
// (now_ms - last_seen_ms <= window_ms).
bool ag_life_is_own_device(const ag_beacon_record_t *r, uint32_t now_ms,
                           uint32_t window_ms);

// True when the source has gone silent past its tolerated gap:
// (now_ms - last_seen_ms) > depart_gap_mult * interval_ms(interval_q).
// With no cadence estimate (interval_q == 0) a nominal 1000 ms cadence is used.
bool ag_life_departed(const ag_beacon_record_t *r, uint32_t now_ms,
                      uint8_t depart_gap_mult);

// Advance one record's lifecycle by one tick.
//  - present (not departed): no change, returns AG_LIFE_NONE.
//  - departed, not yet flagged: set AG_FLAG_DEPARTING and fade p_virt down
//    (bias toward p_center), returns AG_LIFE_DEPARTING.
//  - departed and flagged: returns AG_LIFE_EXPIRE once the silence exceeds the
//    departure gap PLUS a post-departure grace window drawn in [2,12] minutes
//    (rng); otherwise AG_LIFE_NONE while still inside the grace window.
ag_life_action_t ag_life_tick_record(ag_beacon_record_t *r, uint32_t now_ms,
                                     uint8_t depart_gap_mult, ag_prng_t *rng);

// Build a rotation successor from parent into child (rotation model). Copies the
// payload and identity context, assigns a fresh pseudo-random orig_addr derived
// from new_addr_seed, and carries a CONTINUOUS RSSI trajectory
// (child p_virt/p_center == parent's, rssi_ewma/rssi_last copied). first_seen
// and last_seen are reset to now_ms, obs_count = 1, the DEPARTING flag is
// cleared and rec_id is left 0 for the wrapper to recompute from the new addr.
void ag_life_make_successor(const ag_beacon_record_t *parent,
                            ag_beacon_record_t *child, uint32_t new_addr_seed,
                            uint32_t now_ms);

#ifdef __cplusplus
}
#endif
