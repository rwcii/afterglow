// mesh_logic.h — portable mesh transfer math (no BLE radio / hardware deps).
//
// The hardware-coupled mesh.c owns the connectionless adv/scan bursts and the
// FreeRTOS tick; the pure transfer math lives here so it can be host-tested
// against a plain array. Pieces:
//   - carry gate: which live records this node may hand to a peer at all;
//   - subset select: a recency-weighted random pick of ~fraction of the
//     carry-eligible records, capped, excluding the peer's own-origin records;
//   - fragmentation count: how many 4-bit-indexed body fragments a payload needs;
//   - contact table: discovery bookkeeping + the per-peer cooldown gate that
//     decides whether a HELLO should trigger a transfer (mesh.c keeps the table
//     storage and the clock; the decision lives here).
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

// Body bytes carried per DATA fragment. The DATA frame header (type, ttl,
// frag_byte, rec_id:2, addr_type, origin_lo16:2) leaves room for this many
// payload bytes in a 31-byte adv. BOTH the emit side and the reassembly side
// MUST use this one constant so the fragment offset math can never drift.
#define AG_MESH_FRAG_BODY 16

// Records are at most 31 payload bytes (ag_beacon_record_t.payload[31]), so a
// record spans at most this many fragments. Derived from the one body constant
// and the payload size so the emit-side clamp and the reassembly admission gate
// share a single ceiling and cannot disagree. (<= the 4-bit wire field's 15.)
#define AG_MESH_FRAG_CEIL ((31 + AG_MESH_FRAG_BODY - 1) / AG_MESH_FRAG_BODY)

// One slot in the contact table: a peer discovered on air (low-24 of its
// NodeID) and the timestamp of the last transfer we made to it. `used` marks
// the slot occupied. mesh.c owns an array of these (sized by MESH_CONTACTS).
typedef struct {
    uint32_t peer_lo24;     // low 24 bits of the discovered peer's NodeID
    uint32_t last_xfer_ms;  // monotonic clock at the last transfer to this peer
    bool     used;          // slot occupied
} ag_contact_t;

// Record a peer contact in the table and decide whether to transfer to it now.
//
// Returns true when the caller should transfer (a freshly discovered peer, or a
// known peer whose per-peer cooldown has elapsed) and updates the slot's
// last_xfer_ms to now_ms; returns false while the peer is still cooling down
// (the slot is left untouched). When the table is full and the peer is new, the
// stalest slot (smallest last_xfer_ms) is evicted to make room.
//
//   table     : contact-table storage owned by the caller.
//   cap        : number of slots in `table`.
//   peer_lo24  : the discovered peer's NodeID low-24.
//   now_ms     : current monotonic clock (ms). A now_ms < last_xfer_ms wrap is
//                treated as "cooldown elapsed" (never stuck cooling down).
//   cooldown_ms: minimum gap between transfers to the same peer.
bool ag_contact_should_transfer(ag_contact_t *table, uint8_t cap,
                                uint32_t peer_lo24, uint32_t now_ms,
                                uint32_t cooldown_ms);

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

// Iterator-based selection: the shared core ag_mesh_select_subset is built on.
// `get(ctx, i)` returns record i (0 <= i < count) or NULL; this lets a caller
// whose records are NOT a contiguous array (e.g. the on-device pool, indexed by
// pool_record_at) route its transfer selection through the exact same recency-
// weighted sampling the slab-based wrapper is host-tested on. Same semantics and
// return value as ag_mesh_select_subset.
uint8_t ag_mesh_select_subset_fn(const ag_beacon_record_t *(*get)(void *, uint16_t),
                                 void *ctx, uint16_t count,
                                 uint16_t self_node, uint16_t peer_node_lo16,
                                 float fraction, uint8_t cap, ag_prng_t *rng,
                                 uint16_t *out_idx, uint8_t out_max);

// Fragment count for a payload: ceil(payload_len / body_bytes), clamped to the
// 4-bit wire field's AG_MESH_FRAG_MAX (15). body_bytes is the per-fragment body
// budget (AG_MESH_FRAG_BODY = 16 for a 31B adv). Returns 0 for an empty payload;
// clamps body_bytes==0 to 1. NOTE: this clamps to what the WIRE FIELD can carry;
// the production emit path passes AG_MESH_FRAG_BODY over <=31B records, yielding
// at most AG_MESH_FRAG_CEIL fragments, which is what reassembly admits.
uint8_t ag_mesh_frag_count(uint8_t payload_len, uint8_t body_bytes);

// --- fragment reassembly state machine ------------------------------------
//
// Pure assemble/have-mask/complete logic for one in-flight record, extracted
// from the BLE transport so it can be host-tested. The transport owns an array
// of these slots (one in-flight record per rec_id) and reconstructs the pool
// record once a slot reports COMPLETE; this struct owns only the staging buffer
// and the per-fragment bookkeeping. Fragments are AG_MESH_FRAG_BODY bytes each.
typedef struct {
    uint16_t rec_id;
    uint8_t  frag_total;        // 1..AG_MESH_FRAG_MAX
    uint8_t  have_mask;         // bit i set once fragment i has landed
    uint8_t  payload[31];       // staged reassembled payload
    uint8_t  payload_len;       // high-water mark of staged bytes
    bool     used;              // slot occupied by an in-flight record
} ag_reasm_t;

// Verdict of folding one fragment into a slot.
typedef enum {
    AG_REASM_PARTIAL = 0,   // accepted; record not yet complete
    AG_REASM_COMPLETE,      // accepted; all fragments now present
    AG_REASM_BADFRAG,       // malformed fragment header — dropped, no state change
} ag_reasm_verdict_t;

// Clear a slot to the empty state.
void ag_reasm_reset(ag_reasm_t *r);

// Pick a reassembly slot for rec_id from a caller-owned array: an existing slot
// for this rec_id, else a free slot, else a deterministic overwrite victim
// (rec_id % cap) under slot pressure. Returns the slot index (0..cap-1); never
// fails for cap > 0. cap == 0 returns 0.
int ag_reasm_slot_for(ag_reasm_t *slots, int cap, uint16_t rec_id);

// Fold one fragment into slot `r`. frag_byte packs (frag_index<<4 | frag_total),
// matching the wire. Guards frag_total==0 and frag_index>=frag_total (both
// BADFRAG). A slot holding a different rec_id (or an unused slot) is reset to
// this rec_id first. Stages body[0..body_len) at frag_index*AG_MESH_FRAG_BODY
// (bounded to the staging buffer). Returns COMPLETE once every fragment is in.
ag_reasm_verdict_t ag_reasm_add(ag_reasm_t *r, uint16_t rec_id, uint8_t frag_byte,
                                const uint8_t *body, uint8_t body_len);

#ifdef __cplusplus
}
#endif
