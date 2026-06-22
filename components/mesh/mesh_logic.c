// mesh_logic.c — portable mesh transfer math (host-tested in test_mesh.c).
#include "mesh_logic.h"

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
    if (!ag_mesh_carry_eligible(r)) return false;
    uint16_t origin_lo16 = (uint16_t)(r->origin_node & 0xFFFFu);
    // never hand a record back toward its source node.
    if (origin_lo16 == peer_node_lo16) return false;
    // a relayed record (foreign origin) at ttl 0 is exhausted; a local-origin
    // record at ttl 0 is a fresh air capture and still carryable.
    if (r->hop_ttl == 0 && origin_lo16 != self_node) return false;
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

uint8_t ag_mesh_select_subset(const ag_beacon_record_t *slab, uint16_t count,
                              uint16_t self_node, uint16_t peer_node_lo16,
                              float fraction, uint8_t cap, ag_prng_t *rng,
                              uint16_t *out_idx, uint8_t out_max)
{
    if (slab == NULL || out_idx == NULL || out_max == 0 || count == 0) return 0;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    // pass 1: collect carryable indices and the newest last_seen among them.
    uint16_t n_elig = 0;
    uint32_t newest_ms = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (carryable(&slab[i], self_node, peer_node_lo16)) {
            if (slab[i].last_seen_ms > newest_ms) newest_ms = slab[i].last_seen_ms;
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
            if (!carryable(&slab[i], self_node, peer_node_lo16)) continue;
            bool taken = false;
            for (uint8_t s = 0; s < n_sel; s++)
                if (out_idx[s] == i) { taken = true; break; }
            if (taken) continue;
            total += recency_weight(slab[i].last_seen_ms, newest_ms);
        }
        if (total <= 0.0f) break;

        float r = (float)ag_prng_unit(rng) * total;
        uint16_t chosen = 0xFFFFu;
        for (uint16_t i = 0; i < count; i++) {
            if (!carryable(&slab[i], self_node, peer_node_lo16)) continue;
            bool taken = false;
            for (uint8_t s = 0; s < n_sel; s++)
                if (out_idx[s] == i) { taken = true; break; }
            if (taken) continue;
            float w = recency_weight(slab[i].last_seen_ms, newest_ms);
            if (r < w) { chosen = i; break; }
            r -= w;
        }
        if (chosen == 0xFFFFu) break; // float rounding guard; nothing left
        out_idx[n_sel++] = chosen;
    }
    return n_sel;
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
