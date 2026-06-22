// ag_meshguard.c — TTL / origin / LRU-seen-set diffusion guards.
#include "ag_core/ag_meshguard.h"

void ag_seen_init(ag_seen_t *s, uint16_t *ids_backing, uint32_t *stamps_backing, int cap)
{
    s->ids = ids_backing;
    s->stamps = stamps_backing;
    s->cap = cap;
    s->count = 0;
    s->clock = 0;
}

static int seen_find(const ag_seen_t *s, uint16_t rec_id)
{
    for (int i = 0; i < s->count; i++) {
        if (s->ids[i] == rec_id) return i;
    }
    return -1;
}

bool ag_seen_contains(const ag_seen_t *s, uint16_t rec_id)
{
    return seen_find(s, rec_id) >= 0;
}

bool ag_seen_check_and_add(ag_seen_t *s, uint16_t rec_id)
{
    if (s->cap <= 0) return false;
    s->clock++;

    int idx = seen_find(s, rec_id);
    if (idx >= 0) {
        s->stamps[idx] = s->clock;   // touch (LRU move-to-front, logical)
        return true;
    }

    if (s->count < s->cap) {
        s->ids[s->count] = rec_id;
        s->stamps[s->count] = s->clock;
        s->count++;
    } else {
        // Evict the least-recently-used slot. Its rec_id becomes re-admittable
        // later — suppression is time-bounded, not permanent.
        int lru = 0;
        uint32_t oldest = s->stamps[0];
        for (int i = 1; i < s->count; i++) {
            if (s->stamps[i] < oldest) {
                oldest = s->stamps[i];
                lru = i;
            }
        }
        s->ids[lru] = rec_id;
        s->stamps[lru] = s->clock;
    }
    return false;
}

ag_mesh_verdict_t ag_mesh_evaluate(ag_seen_t *s,
                                   uint16_t rec_id,
                                   uint8_t inbound_ttl,
                                   uint32_t origin_node,
                                   uint32_t self_node,
                                   bool already_in_pool,
                                   uint8_t pool_ttl,
                                   uint8_t *out_ttl)
{
    if (out_ttl) *out_ttl = 0;

    // Clamp the wire TTL to the design ceiling FIRST. The hop-TTL field is a
    // single attacker-controllable byte (0..255), but diffusion is bounded to
    // AG_TTL_INIT hops by design. A crafted frame declaring ttl=255 must not be
    // able to store a hop_ttl above the ceiling and amplify across the mesh, so
    // cap it here independently of the carry-gate invariants downstream — this
    // is the hard bound, not a soft hint.
    if (inbound_ttl > AG_TTL_INIT) inbound_ttl = AG_TTL_INIT;

    // Guard: TTL exhausted on the wire. ttl==0 records are replay-only and are
    // never re-meshed.
    if (inbound_ttl == 0) {
        return AG_MESH_DROP_TTL;
    }

    // Guard: origin pinning — never absorb a record we ourselves air-captured
    // (soft hint, but cheap and prevents the obvious return-to-origin loop).
    if (origin_node != 0 && origin_node == self_node) {
        return AG_MESH_DROP_OWN_ORIGIN;
    }

    // Already live in our pool: refresh to the LOWER ttl (never raise), so a
    // record cannot have its reach extended by re-delivery.
    if (already_in_pool) {
        uint8_t lower = (inbound_ttl < pool_ttl) ? inbound_ttl : pool_ttl;
        if (out_ttl) *out_ttl = lower;
        // Touch the seen-set so dedup state stays warm.
        ag_seen_check_and_add(s, rec_id);
        return AG_MESH_REFRESH_LOWER;
    }

    // Not in pool: consult the seen-set. A hit here means "seen but already
    // evicted" → do not resurrect (within the LRU's time-bounded horizon).
    bool seen = ag_seen_check_and_add(s, rec_id);
    if (seen) {
        return AG_MESH_DROP_SEEN;
    }

    // Accept: decrement TTL for storage (decrement-only, never raised).
    if (out_ttl) *out_ttl = (uint8_t)(inbound_ttl - 1);
    return AG_MESH_ACCEPT;
}
