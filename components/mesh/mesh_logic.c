// mesh_logic.c — portable mesh transfer math (host-tested in test_mesh.c).
#include "mesh_logic.h"
#include "ag_core/ag_meshguard.h"  // AG_TTL_INIT — the legacy-ttl value range the
                                   // DATA version byte must stay clear of.
#include <string.h>     // memset / memcpy (reassembly staging)

// The DATA version byte occupies the wire offset a legacy DATA frame used for
// `ttl` (0..AG_TTL_INIT). If AG_MESH_VERSION fell in that range, a legacy frame
// whose ttl equalled it would slip through ag_mesh_version_ok and be misparsed.
// AG_MESH_VERSION_MIN encodes that lower bound; pin both invariants at compile
// time so a future version bump back into the ttl band fails the build, not the
// field.
_Static_assert(AG_MESH_VERSION_MIN >= AG_TTL_INIT,
               "version floor must cover the full legacy ttl range");
_Static_assert(AG_MESH_VERSION > AG_MESH_VERSION_MIN,
               "AG_MESH_VERSION must exceed the legacy-ttl band to stay "
               "distinguishable from a versionless legacy DATA frame");

bool ag_mesh_version_ok(uint8_t wire_version)
{
    return wire_version == AG_MESH_VERSION;
}

bool ag_mesh_carry_eligible(const ag_beacon_record_t *rec)
{
    if (rec == NULL) return false;
    // carry is stricter than replay: replay-eligible, multi-sweep persistent
    // (obs_count >= 2), and not on its way out.
    if (!(rec->flags & AG_FLAG_REPLAY_ELIGIBLE)) return false;
    if (rec->obs_count < 2) return false;
    if (rec->flags & AG_FLAG_DEPARTING) return false;
    return true;
}

// Carryable on the wire: carry-eligible AND not a meshed record whose hop TTL
// has been driven to 0. A fresh air-captured record carries hop_ttl==0 but is
// its own origin, so it is still allowed to seed its first hop.
static bool carryable(const ag_beacon_record_t *r, uint16_t self_node,
                      uint16_t peer_node_lo16)
{
    (void)self_node;   // provenance is now a record flag, not an origin compare
    if (!ag_mesh_carry_eligible(r)) return false;
    uint16_t origin_lo16 = (uint16_t)(r->origin_node & 0xFFFFu);
    // Never hand a record back toward its source node. This return-to-source
    // guard stays at lo16 (unlike the own-origin guard, widened to lo24): the
    // peer id is carried only as lo16 on the transfer path (transfer_to_peer
    // takes peer_lo16), so widening here would require propagating a wider peer
    // id through HELLO discovery -> contact table -> selection. A lo16 peer
    // collision (~1/65536) at worst suppresses one carry or fails to suppress one
    // return-to-source hop; the TTL + seen-set hard guards still bound diffusion,
    // so this is a deliberately bounded soft hint, not a correctness guard.
    if (origin_lo16 == peer_node_lo16) return false;
    // a relayed record (foreign origin) at ttl 0 is exhausted (replay-only); a
    // local air-captured record at ttl 0 is a fresh own capture and still
    // carryable. Distinguish the two by the RELAYED provenance flag rather than
    // by an origin-lo16 coincidence: two nodes can share a NodeID's low 16 bits,
    // which would otherwise misclassify an exhausted foreign relay as own and
    // keep it (wrongly) carryable.
    // INVARIANT: any foreign-origin record admitted to the pool MUST carry
    // AG_FLAG_RELAYED. The only ingest path for one is mesh_absorb_inbound, which
    // sets it; local air-capture (ag_pool_admit) is genuinely own-origin and
    // leaves it clear. The default here is "unflagged => own => carryable", so a
    // NEW foreign-ingest path that forgets the flag would wrongly keep a ttl0
    // relay carryable — set RELAYED at any such path.
    if (r->hop_ttl == 0 && (r->flags & AG_FLAG_RELAYED)) return false;
    return true;
}

// Recency weight: later last_seen_ms ranks higher. Normalized against the most
// recent carryable record so the spread stays in a stable [eps,1] band.
static float recency_weight(uint32_t last_seen_ms, uint32_t newest_ms)
{
    // older records get a smaller (but nonzero) weight; +1 avoids /0 and a
    // hard-zero weight for the oldest record.
    uint32_t age = (newest_ms >= last_seen_ms) ? (newest_ms - last_seen_ms) : 0u;
    return 1.0f / (1.0f + (float)age);
}

uint8_t ag_mesh_select_subset_fn(const ag_beacon_record_t *(*get)(void *, uint16_t),
                                 void *ctx, uint16_t count,
                                 uint16_t self_node, uint16_t peer_node_lo16,
                                 float fraction, uint8_t cap, ag_prng_t *rng,
                                 uint16_t *out_idx, uint8_t out_max)
{
    if (get == NULL || out_idx == NULL || out_max == 0 || count == 0) return 0;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    // pass 1: collect carryable indices and the newest last_seen among them.
    uint16_t n_elig = 0;
    uint32_t newest_ms = 0;
    for (uint16_t i = 0; i < count; i++) {
        const ag_beacon_record_t *r = get(ctx, i);
        if (r && carryable(r, self_node, peer_node_lo16)) {
            if (r->last_seen_ms > newest_ms) newest_ms = r->last_seen_ms;
            n_elig++;
        }
    }
    if (n_elig == 0) return 0;

    // target count = round(fraction * eligible), then bound by cap and out_max.
    uint16_t want = (uint16_t)((float)n_elig * fraction + 0.5f);
    if (want > n_elig) want = n_elig;
    if (cap && want > cap) want = cap;
    if (want > out_max) want = out_max;
    if (want == 0) return 0;

    // pass 2: weighted sampling without replacement. For each remaining target
    // slot, draw a record from the carryable set with probability proportional
    // to its recency weight (A-Res style: one weighted pick per slot, taken
    // records masked out). n_elig is small (<= max_records_per_contact-ish), so
    // the per-slot linear scan is cheap and fully deterministic given rng.
    uint8_t n_sel = 0;
    for (uint16_t pick = 0; pick < want; pick++) {
        // total weight of still-available carryable records.
        float total = 0.0f;
        for (uint16_t i = 0; i < count; i++) {
            const ag_beacon_record_t *rr = get(ctx, i);
            if (!rr || !carryable(rr, self_node, peer_node_lo16)) continue;
            bool taken = false;
            for (uint8_t s = 0; s < n_sel; s++)
                if (out_idx[s] == i) { taken = true; break; }
            if (taken) continue;
            total += recency_weight(rr->last_seen_ms, newest_ms);
        }
        if (total <= 0.0f) break;

        float r = (float)ag_prng_unit(rng) * total;
        uint16_t chosen = 0xFFFFu;
        for (uint16_t i = 0; i < count; i++) {
            const ag_beacon_record_t *rr = get(ctx, i);
            if (!rr || !carryable(rr, self_node, peer_node_lo16)) continue;
            bool taken = false;
            for (uint8_t s = 0; s < n_sel; s++)
                if (out_idx[s] == i) { taken = true; break; }
            if (taken) continue;
            float w = recency_weight(rr->last_seen_ms, newest_ms);
            if (r < w) { chosen = i; break; }
            r -= w;
        }
        if (chosen == 0xFFFFu) break; // float rounding guard; nothing left
        out_idx[n_sel++] = chosen;
    }
    return n_sel;
}

// Slab getter: a thin adaptor so the contiguous-array entry point shares the
// exact iterator-based core above.
static const ag_beacon_record_t *slab_get(void *ctx, uint16_t i)
{
    return &((const ag_beacon_record_t *)ctx)[i];
}

uint8_t ag_mesh_select_subset(const ag_beacon_record_t *slab, uint16_t count,
                              uint16_t self_node, uint16_t peer_node_lo16,
                              float fraction, uint8_t cap, ag_prng_t *rng,
                              uint16_t *out_idx, uint8_t out_max)
{
    if (slab == NULL) return 0;
    return ag_mesh_select_subset_fn(slab_get, (void *)slab, count, self_node,
                                    peer_node_lo16, fraction, cap, rng,
                                    out_idx, out_max);
}

uint8_t ag_mesh_frag_count(uint8_t payload_len, uint8_t body_bytes)
{
    if (payload_len == 0) return 0;
    if (body_bytes == 0) body_bytes = 1;
    uint16_t n = (uint16_t)((payload_len + body_bytes - 1) / body_bytes); // ceil
    if (n > AG_MESH_FRAG_MAX) n = AG_MESH_FRAG_MAX;
    return (uint8_t)n;
}

bool ag_contact_should_transfer(ag_contact_t *table, uint8_t cap,
                                uint32_t peer_lo24, uint32_t now_ms,
                                uint32_t cooldown_ms)
{
    if (table == NULL || cap == 0) return false;

    // Find this peer's slot; else the first free slot; else remember the stalest
    // (smallest last_xfer_ms) to evict. Mirrors the array walk mesh.c used.
    int slot = -1, oldest = 0;
    for (uint8_t i = 0; i < cap; i++) {
        if (table[i].used && table[i].peer_lo24 == peer_lo24) { slot = i; break; }
        if (!table[i].used) { slot = (int)i; break; }
        if (table[i].last_xfer_ms < table[oldest].last_xfer_ms) oldest = (int)i;
    }
    if (slot < 0) slot = oldest;   // table full of distinct peers: evict stalest

    ag_contact_t *c = &table[slot];
    bool known = c->used && c->peer_lo24 == peer_lo24;
    if (known) {
        // Per-peer cooldown. Compute elapsed without unsigned wrap: a clock that
        // appears to run backwards (now < last) means a wrap/reset, which we
        // treat as cooldown-elapsed rather than letting the subtraction wrap to
        // a huge "still cooling down" value.
        uint32_t elapsed = (now_ms >= c->last_xfer_ms)
                               ? (now_ms - c->last_xfer_ms) : cooldown_ms;
        if (elapsed < cooldown_ms) return false;   // still cooling down
    }

    c->used = true;
    c->peer_lo24 = peer_lo24;
    c->last_xfer_ms = now_ms;
    return true;
}

// --- fragment reassembly state machine ------------------------------------

void ag_reasm_reset(ag_reasm_t *r)
{
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
}

int ag_reasm_slot_for(ag_reasm_t *slots, int cap, uint16_t rec_id)
{
    if (slots == NULL || cap <= 0) return 0;
    // existing slot for this rec_id wins.
    for (int i = 0; i < cap; i++)
        if (slots[i].used && slots[i].rec_id == rec_id) return i;
    // else first free slot.
    for (int i = 0; i < cap; i++)
        if (!slots[i].used) return i;
    // else a deterministic overwrite victim under slot pressure.
    return (int)(rec_id % (uint16_t)cap);
}

// Largest fragment count the staging buffer + have_mask can actually hold:
// have_mask is a uint8_t (8 bits) and a fragment's body must land within the
// The reassembly admission ceiling IS the shared emit/reassembly ceiling
// (AG_MESH_FRAG_CEIL, derived once in mesh_logic.h from the 31-byte payload and
// AG_MESH_FRAG_BODY). A frame declaring more fragments than a <=31B record can
// span is malformed and is rejected rather than silently truncated. The value
// (2 at BODY=16) is well within the 8-bit have_mask, so no separate mask cap is
// needed; if AG_MESH_FRAG_BODY ever shrinks past 4B this must stay <= 8.
#define AG_REASM_MAX_FRAGS AG_MESH_FRAG_CEIL

ag_reasm_verdict_t ag_reasm_add(ag_reasm_t *r, uint16_t rec_id, uint8_t frag_byte,
                                const uint8_t *body, uint8_t body_len)
{
    if (r == NULL) return AG_REASM_BADFRAG;
    uint8_t frag_idx = (uint8_t)(frag_byte >> 4);
    uint8_t frag_tot = (uint8_t)(frag_byte & 0x0F);
    // Malformed header: zero fragments, an index outside the declared total, or a
    // total larger than the staging buffer / have_mask can represent (the wire
    // field allows up to 15, but a 31-byte body over AG_MESH_FRAG_BODY chunks
    // and an 8-bit mask support far fewer — reject rather than truncate).
    if (frag_tot == 0 || frag_tot > AG_REASM_MAX_FRAGS || frag_idx >= frag_tot)
        return AG_REASM_BADFRAG;
    // The fragment body must fit at its offset; an out-of-range fragment would be
    // marked present without being staged (a corrupt early-COMPLETE), so drop it.
    uint8_t off = (uint8_t)(frag_idx * AG_MESH_FRAG_BODY);
    if (body == NULL || (uint16_t)off + body_len > sizeof(r->payload))
        return AG_REASM_BADFRAG;

    // (Re)claim the slot for this rec_id on first fragment or on reuse.
    if (!r->used || r->rec_id != rec_id) {
        ag_reasm_reset(r);
        r->used = true;
        r->rec_id = rec_id;
        r->frag_total = frag_tot;
    }
    // A fragment whose declared total disagrees with the slot's (set by the first
    // fragment of this rec_id) is inconsistent — drop it rather than completing
    // against the wrong mask.
    if (r->frag_total != frag_tot) return AG_REASM_BADFRAG;

    // Stage the body at the fragment's (bounds-checked) offset.
    memcpy(r->payload + off, body, body_len);
    if ((uint16_t)off + body_len > r->payload_len)
        r->payload_len = (uint8_t)(off + body_len);
    r->have_mask |= (uint8_t)(1u << frag_idx);

    uint8_t full = (uint8_t)((1u << frag_tot) - 1);
    return ((r->have_mask & full) == full) ? AG_REASM_COMPLETE : AG_REASM_PARTIAL;
}
