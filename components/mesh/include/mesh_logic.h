// mesh_logic.h — portable mesh transfer math (no BLE radio / hardware deps).
//
// The hardware-coupled mesh.c owns the connectionless adv/scan bursts, the
// contact table and the FreeRTOS tick; the pure transfer math lives here so it
// can be host-tested against a plain array. Three pieces:
//   - carry gate: which live records this node may hand to a peer at all;
//   - subset select: a recency-weighted random pick of ~fraction of the
//     carry-eligible records, capped, excluding the peer's own-origin records;
//   - fragmentation count: how many 4-bit-indexed body fragments a payload needs.
// Inbound gating stays in ag_mesh_evaluate (ag_meshguard.h) — not reimplemented.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>             // NULL
#include "pool.h"               // ag_beacon_record_t, flags, classes
#include "ag_core/ag_prng.h"

#ifdef __cplusplus
extern "C" {
#endif

// frag_index / frag_total are 4-bit fields on the wire, so a record can span at
// most this many body fragments.
#define AG_MESH_FRAG_MAX 15

// May this record be handed to a peer? Stricter than replay eligibility: the
// record must be replay-eligible AND have been seen on >= 2 sweeps (a multi-
// sweep persistence proxy via obs_count) AND not be marked departing.
bool ag_mesh_carry_eligible(const ag_beacon_record_t *rec);

// Pick a recency-weighted random subset of carry-eligible records to hand a peer.
//   slab/count    : compacted live records [0,count).
//   self_node     : this node's 32-bit NodeID (unused by selection, accepted for
//                   symmetry with the caller's transfer context).
//   peer_node_lo16: low 16 bits of the peer's NodeID; records whose origin_node
//                   low-16 matches are never selected (would return to source).
//   fraction      : target share of the carry-eligible set (clamped to [0,1]).
//   cap           : hard upper bound (config max_records_per_contact).
//   rng           : injected randomness (deterministic under test).
//   out_idx/out_max: caller buffer; slot indices into `slab` are written here.
// Returns the number of indices written (<= min(cap, out_max)). Records explicitly
// exhausted (hop_ttl carried but driven to 0 by relay) are skipped; a fresh
// air-captured record (hop_ttl==0 by construction) is still carryable.
uint8_t ag_mesh_select_subset(const ag_beacon_record_t *slab, uint16_t count,
                              uint16_t self_node, uint16_t peer_node_lo16,
                              float fraction, uint8_t cap, ag_prng_t *rng,
                              uint16_t *out_idx, uint8_t out_max);

// Fragment count for a payload: ceil(payload_len / body_bytes), clamped to
// AG_MESH_FRAG_MAX. body_bytes is the per-fragment body budget (~20 for a 31B
// adv). Returns 0 for an empty payload; clamps body_bytes==0 to 1.
uint8_t ag_mesh_frag_count(uint8_t payload_len, uint8_t body_bytes);

#ifdef __cplusplus
}
#endif
