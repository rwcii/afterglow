// ag_meshguard.h — mesh loop / amplification guards (portable)
//
// One air-captured beacon must diffuse to AT MOST one ghost per node, within
// TTL_INIT hops, deduped, never resurrected, never returned to origin. Without
// these guards a beacon could ping-pong across the mesh indefinitely (unbounded
// amplification). The three hard guards:
//
//   1. Hop TTL (TTL_INIT=3): decrement-only on absorption; ttl==0 is
//      replay-only, never re-meshed. Bounds diffusion to N hops.
//   2. rec_id dedup + LRU seen-set: already-in-pool refreshes to the LOWER ttl
//      (never raise); seen-but-evicted is dropped (no resurrection) — but the
//      seen-set is an LRU, so suppression is TIME-BOUNDED (bounded re-admission
//      when an entry ages out, validated by experiment E8).
//   3. Origin pinning: 32-bit origin_node of the first air-capturer; never
//      absorb own-origin (soft hint; TTL + seen-set are the hard guards).
//
// Pure data structure + logic; no ESP deps.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_TTL_INIT 3   //  (range 2-4); Config value 2 rejected.

// LRU seen-set over rec_id. Fixed-capacity, no allocation; the caller provides
// backing storage sized at init. Simple move-to-front LRU (small N, default
// 4096 entries; the production slab budgets this).
typedef struct {
    uint16_t *ids;      // rec_id ring (caller-provided)
    uint32_t *stamps;   // last-touch logical clock per slot
    int       cap;
    int       count;
    uint32_t  clock;
} ag_seen_t;

// Initialize over caller-provided backing arrays of `cap` entries each.
void ag_seen_init(ag_seen_t *s, uint16_t *ids_backing, uint32_t *stamps_backing, int cap);

// Returns true if rec_id was ALREADY present (a hit). Always records/touches it
// (move-to-front). On insert when full, evicts the LRU entry — so an evicted
// id can later be re-admitted (time-bounded suppression, / E8).
bool ag_seen_check_and_add(ag_seen_t *s, uint16_t rec_id);

// True if rec_id is currently in the seen-set (no mutation).
bool ag_seen_contains(const ag_seen_t *s, uint16_t rec_id);

// Result of evaluating an inbound mesh record against the guards.
typedef enum {
    AG_MESH_ACCEPT = 0,     // new, admit with decremented ttl
    AG_MESH_REFRESH_LOWER,  // already in pool; refresh to the lower ttl
    AG_MESH_DROP_TTL,       // ttl exhausted (replay-only, never re-meshed)
    AG_MESH_DROP_SEEN,      // seen-but-evicted: do not resurrect
    AG_MESH_DROP_OWN_ORIGIN,// would return to its air-source node
} ag_mesh_verdict_t;

// Evaluate one inbound record. `inbound_ttl` is the hop TTL carried on the wire
// `origin_node`/`self_node` are 32-bit NodeIDs; `already_in_pool` and
// `pool_ttl` describe a matching live record if present. Updates the seen-set.
// On AG_MESH_ACCEPT, *out_ttl receives the decremented TTL to store.
ag_mesh_verdict_t ag_mesh_evaluate(ag_seen_t *s,
                                   uint16_t rec_id,
                                   uint8_t inbound_ttl,
                                   uint32_t origin_node,
                                   uint32_t self_node,
                                   bool already_in_pool,
                                   uint8_t pool_ttl,
                                   uint8_t *out_ttl);

#ifdef __cplusplus
}
#endif
