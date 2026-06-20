// pool_logic.h — portable pool record helpers (no PSRAM / hardware deps).
//
// The hardware-coupled pool.c owns the PSRAM slab and the FreeRTOS task; the
// pure record logic (rec_id hashing, EWMA merge, find-or-insert, the eviction
// sweep over an array) lives here so it can be host-tested against a plain
// stack array. pool.c is a thin wrapper that supplies the slab + RNG + clock.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pool.h"               // ag_beacon_record_t, ag_beacon_class_t, flags
#include "radio_backend.h"      // ag_capture_t, ag_proto_t
#include "ag_core/ag_prng.h"
#include "ag_core/ag_eviction.h"

#ifdef __cplusplus
extern "C" {
#endif

// Stable 16-bit record id over (addr_type, orig_addr[6], payload). Identical
// across relays — the dedup/seen-set key. Pure FNV-1a fold to 16 bits.
uint16_t ag_pool_rec_id(uint8_t addr_type, const uint8_t orig_addr[6],
                        const uint8_t *payload, uint8_t payload_len);

// EWMA update for int8 RSSI (alpha ~ 1/8). Returns the new ewma.
int8_t ag_pool_rssi_ewma(int8_t prev_ewma, int8_t sample);

// Find the slot index of an existing record matching cap's identity, or -1.
// `slab` holds `count` live records (compacted: indices [0,count)).
int ag_pool_find(const ag_beacon_record_t *slab, uint16_t count,
                 uint8_t addr_type, const uint8_t orig_addr[6],
                 const uint8_t *payload, uint8_t payload_len);

// Admit or merge a captured beacon into the (compacted) slab.
//  - on merge: updates rssi_last/ewma, obs_count (saturating), last_seen, interval_q.
//  - on insert (first sighting): fills a fresh record at index *count, draws
//    base_ttl via ag_evict_draw_base_ttl, sets origin_node, CLASS_TENTATIVE.
// Returns the slot index touched, or -1 if the slab is full (caller evicts).
// `now_ms` is the capture timestamp; `node_id` pins origin on first sighting.
int ag_pool_admit(ag_beacon_record_t *slab, uint16_t *count, uint16_t capacity,
                  const ag_capture_t *cap, const uint8_t orig_addr[6],
                  uint8_t addr_type, uint32_t now_ms, uint32_t node_id,
                  const ag_evict_params_t *evp, ag_prng_t *rng);

// One eviction sweep over the compacted slab. Computes each record's age,
// score-percentile (by rssi_ewma rank as a cheap proxy) and the current
// fill fraction, calls ag_evict_decide, and compacts survivors in place.
// Returns the number of records evicted. now_ms drives age.
uint16_t ag_pool_evict_sweep(ag_beacon_record_t *slab, uint16_t *count,
                             uint16_t carry_cap, uint32_t now_ms,
                             const ag_evict_params_t *evp, ag_prng_t *rng);

#ifdef __cplusplus
}
#endif
