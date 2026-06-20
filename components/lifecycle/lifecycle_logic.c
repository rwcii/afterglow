// lifecycle_logic.c — portable rotation/departure decisions (host-tested in test_lifecycle.c).
#include "lifecycle_logic.h"
#include <string.h>

// nominal cadence when interval_q has not been estimated yet (ms).
#define AG_LIFE_NOMINAL_INTERVAL_MS 1000u
// post-departure grace window bounds (ms): [2,12] minutes.
#define AG_LIFE_GRACE_MIN_MS (2u * 60u * 1000u)
#define AG_LIFE_GRACE_MAX_MS (12u * 60u * 1000u)
// own-device strong-signal floor (dBm).
#define AG_LIFE_OWN_RSSI_MIN_DBM (-45)
// fade factor applied to p_virt on the departing transition.
#define AG_LIFE_FADE 0.5f

uint32_t ag_life_interval_ms(uint16_t interval_q)
{
    // 0.625 ms TU units → ms: q * 5 / 8.
    return ((uint32_t)interval_q * 5u) / 8u;
}

static uint32_t depart_gap_ms(const ag_beacon_record_t *r, uint8_t depart_gap_mult)
{
    uint32_t ivl = ag_life_interval_ms(r->interval_q);
    if (ivl == 0) ivl = AG_LIFE_NOMINAL_INTERVAL_MS;
    uint32_t mult = depart_gap_mult ? depart_gap_mult : 1u;
    return ivl * mult;
}

bool ag_life_is_own_device(const ag_beacon_record_t *r, uint32_t now_ms,
                           uint32_t window_ms)
{
    if (r->rssi_ewma <= AG_LIFE_OWN_RSSI_MIN_DBM) return false;
    // continuous presence: spanned at least the whole window...
    uint32_t span = (r->last_seen_ms >= r->first_seen_ms)
                        ? (r->last_seen_ms - r->first_seen_ms)
                        : 0u;
    if (span < window_ms) return false;
    // ...and still present recently (not a stale span from long ago).
    uint32_t silent = (now_ms >= r->last_seen_ms) ? (now_ms - r->last_seen_ms) : 0u;
    if (silent > window_ms) return false;
    return true;
}

bool ag_life_departed(const ag_beacon_record_t *r, uint32_t now_ms,
                      uint8_t depart_gap_mult)
{
    uint32_t silent = (now_ms >= r->last_seen_ms) ? (now_ms - r->last_seen_ms) : 0u;
    return silent > depart_gap_ms(r, depart_gap_mult);
}

ag_life_action_t ag_life_tick_record(ag_beacon_record_t *r, uint32_t now_ms,
                                     uint8_t depart_gap_mult, ag_prng_t *rng)
{
    if (!ag_life_departed(r, now_ms, depart_gap_mult)) {
        return AG_LIFE_NONE;
    }

    uint32_t silent = (now_ms >= r->last_seen_ms) ? (now_ms - r->last_seen_ms) : 0u;

    if (!(r->flags & AG_FLAG_DEPARTING)) {
        // first tick past the gap: mark departing and fade toward center.
        r->flags |= AG_FLAG_DEPARTING;
        r->p_virt = r->p_center + (r->p_virt - r->p_center) * AG_LIFE_FADE;
        return AG_LIFE_DEPARTING;
    }

    // already departing: retire once silence exceeds gap + a drawn grace window.
    float grace = ag_prng_uniform(rng, (float)AG_LIFE_GRACE_MIN_MS,
                                  (float)AG_LIFE_GRACE_MAX_MS);
    uint32_t expire_at = depart_gap_ms(r, depart_gap_mult) + (uint32_t)grace;
    if (silent > expire_at) {
        return AG_LIFE_EXPIRE;
    }
    return AG_LIFE_NONE;
}

void ag_life_make_successor(const ag_beacon_record_t *parent,
                            ag_beacon_record_t *child, uint32_t new_addr_seed,
                            uint32_t now_ms)
{
    // carry identity context and tx state, then override what rotates.
    *child = *parent;

    // fresh pseudo-random address from the seed (splitmix64-style avalanche).
    uint64_t x = (uint64_t)new_addr_seed + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= (x >> 31);
    for (int i = 0; i < 6; i++) child->orig_addr[i] = (uint8_t)(x >> (8 * i));

    // payload copied verbatim (template continuity across the rotation).
    memcpy(child->payload, parent->payload, sizeof(child->payload));
    child->payload_len = parent->payload_len;

    // CONTINUOUS RSSI trajectory: the walk state and observed levels carry over.
    child->p_virt = parent->p_virt;
    child->p_center = parent->p_center;
    child->rssi_ewma = parent->rssi_ewma;
    child->rssi_last = parent->rssi_last;

    // fresh presence: a newborn record, not yet flagged, id recomputed upstream.
    child->first_seen_ms = now_ms;
    child->last_seen_ms = now_ms;
    child->obs_count = 1;
    child->flags = (uint8_t)(parent->flags & ~AG_FLAG_DEPARTING);
    child->rec_id = 0;  // wrapper recomputes from the new addr + payload
}
