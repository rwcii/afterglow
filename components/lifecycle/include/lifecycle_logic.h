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
    AG_LIFE_NONE = 0,   // present, or already-departing: no change this tick
    AG_LIFE_DEPARTING,  // crossed the departure gap this tick: flagged + faded
} ag_life_action_t;

// Per-ghost address-rotation behavior. STATIONARY_HOLD ghosts keep one
// cloned address for life; ROTATING ghosts swap to a fresh NRPA on a per-ghost
// independent ~15 min timer and keep doing so until the lineage's TTL completes.
typedef enum {
    AG_LIFE_STATIONARY_HOLD = 0, // static-random / public / Wi-Fi: never rotate
    AG_LIFE_ROTATING,            // NRPA class: rotate to fresh NRPA on the timer
} ag_life_rotation_mode_t;

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

// Advance one record's PRESENCE state by one tick (presence gate only — not
// end-of-life; lineage lifetime is owned solely by the eviction TTL):
//  - present (not departed): no change, returns AG_LIFE_NONE.
//  - departed, not yet flagged: set AG_FLAG_DEPARTING and fade p_virt down
//    (bias toward p_center), returns AG_LIFE_DEPARTING.
//  - departed and already flagged: no change, returns AG_LIFE_NONE (the record
//    is held — replayable while absent — until the eviction sweep retires it at
//    its TTL). Re-observation clears AG_FLAG_DEPARTING in pool admission.
ag_life_action_t ag_life_tick_record(ag_beacon_record_t *r, uint32_t now_ms,
                                     uint8_t depart_gap_mult);

// Build a rotation successor from parent into child (rotation model). Copies the
// payload and identity context, assigns a fresh pseudo-random orig_addr derived
// from new_addr_seed, and carries a CONTINUOUS RSSI trajectory
// (child p_virt/p_center == parent's, rssi_ewma/rssi_last copied). first_seen
// and last_seen are reset to now_ms, obs_count = 1, the DEPARTING flag is
// cleared and rec_id is left 0 for the wrapper to recompute from the new addr.
void ag_life_make_successor(const ag_beacon_record_t *parent,
                            ag_beacon_record_t *child, uint32_t new_addr_seed,
                            uint32_t now_ms);

// Rotation mode for a record's beacon class: AG_LIFE_ROTATING for the
// NRPA class, AG_LIFE_STATIONARY_HOLD for everything else.
ag_life_rotation_mode_t ag_life_rotation_mode(uint8_t cls);

// Draw a per-ghost address-rotation period (ms): log-normal located at ~15 min,
// clamped to [1 min, 1 h]. Independent per call (no cohort correlation).
uint32_t ag_life_draw_rotate_ms(ag_prng_t *rng);

// True when a ROTATING ghost's scheduled rotation deadline (next_rotate_ms) has
// passed. next_rotate_ms == 0 (unscheduled / stationary) is never due.
bool ag_life_rotation_due(const ag_beacon_record_t *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
