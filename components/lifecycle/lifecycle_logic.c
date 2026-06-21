// lifecycle_logic.c — portable rotation/departure decisions (host-tested in test_lifecycle.c).
#include "lifecycle_logic.h"
#include <math.h>
#include <string.h>

// nominal cadence when interval_q has not been estimated yet (ms).
#define AG_LIFE_NOMINAL_INTERVAL_MS 1000u
// Address-rotation period for ROTATING (NRPA-class) ghosts: log-normal located
// at ~15 min (RPA-like churn), clamped to the BLE Core advertising-interval-style
// band [1 min, 1 h] (cf. 0x0001..0x0E10). Per-ghost independent — drawn once per
// scheduling, never shared across the cohort (avoids correlated rotation).
#ifdef AG_ONAIR_TEST
// On-air rig only: compress the period to seconds so the two-board test can watch
// a ghost actually swap address within a run. Production keeps the 15 min median.
#define AG_LIFE_ROTATE_MEDIAN_MS (8u * 1000u)
#define AG_LIFE_ROTATE_MIN_MS    (4u * 1000u)
#define AG_LIFE_ROTATE_MAX_MS    (20u * 1000u)
#else
#define AG_LIFE_ROTATE_MEDIAN_MS (15u * 60u * 1000u)
#define AG_LIFE_ROTATE_MIN_MS    (1u * 60u * 1000u)
#define AG_LIFE_ROTATE_MAX_MS    (60u * 60u * 1000u)
#endif
#define AG_LIFE_ROTATE_SIGMA     0.55f
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
                                     uint8_t depart_gap_mult)
{
    // Presence gate ONLY: departure marks "the source is currently absent" so the
    // ghost may stand in for it; it is NOT end-of-life. Lineage lifetime is owned
    // solely by the eviction TTL (age from first_seen). Re-observation clears
    // AG_FLAG_DEPARTING in pool admission, so the gate is edge-correct both ways.
    if (!ag_life_departed(r, now_ms, depart_gap_mult)) {
        return AG_LIFE_NONE;
    }
    if (!(r->flags & AG_FLAG_DEPARTING)) {
        // first tick past the gap: mark departing and fade toward center.
        r->flags |= AG_FLAG_DEPARTING;
        r->p_virt = r->p_center + (r->p_virt - r->p_center) * AG_LIFE_FADE;
        return AG_LIFE_DEPARTING;
    }
    return AG_LIFE_NONE;  // already departing: hold until the TTL eviction sweep
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
    // A rotation successor is always a Non-Resolvable Private Address: force the
    // subtype bits (top two of the MSB, orig_addr[0]) to 0b00. Without this the
    // avalanche can land on 0b01 (RPA — uncloneable, the eligibility gate forbids
    // it) or 0b10 (reserved — the controller rejects set_rand_addr). Only NRPAs
    // legitimately rotate, so a rotating ghost of any cloneable source ages out
    // into the NRPA class. (See classifier_logic.c: subtype is read from [0].)
    child->orig_addr[0] = (uint8_t)(child->orig_addr[0] & 0x3F);

    // payload copied verbatim (template continuity across the rotation).
    memcpy(child->payload, parent->payload, sizeof(child->payload));
    child->payload_len = parent->payload_len;

    // CONTINUOUS RSSI trajectory: the walk state and observed levels carry over.
    child->p_virt = parent->p_virt;
    child->p_center = parent->p_center;
    child->rssi_ewma = parent->rssi_ewma;
    child->rssi_last = parent->rssi_last;

    // INHERIT the lineage TTL basis: first_seen_ms / base_ttl_s / ttl_cap_s /
    // replay_deadline_ms carry over from the parent (already copied by the
    // struct assignment above; first_seen_ms is deliberately NOT reset). Eviction
    // measures age from first_seen against base_ttl_s, so a lineage must age from
    // the ORIGINAL device's first sighting — otherwise each rotation restarts the
    // age clock and the lineage lives forever. The whole rotating lineage now dies
    // when the original's TTL completes, regardless of how many times it rotated.
    (void)now_ms;  // successor age is the parent's, not "now"
    // fresh presence for the new address only: just-observed, not yet flagged.
    child->last_seen_ms = now_ms;
    child->obs_count = 1;
    child->flags = (uint8_t)(parent->flags & ~AG_FLAG_DEPARTING);
    child->rec_id = 0;  // wrapper recomputes from the new addr + payload
}

ag_life_rotation_mode_t ag_life_rotation_mode(uint8_t cls)
{
    // Only the NRPA class rotates: real non-resolvable-private sources mint a
    // fresh address periodically, so a rotating ghost of one is realistic.
    // Static-random / public / Wi-Fi hold a single address for life (correct for
    // iBeacon/Eddystone/APs, which do not rotate; making them rotate would be
    // unrealistic for those device classes).
    // Captured RPA (0b01) is never cloned at all (refused by the eligibility gate),
    // so it never reaches here as a replayable ghost.
    return (cls == AG_CLASS_NRPA_BLE) ? AG_LIFE_ROTATING : AG_LIFE_STATIONARY_HOLD;
}

uint32_t ag_life_draw_rotate_ms(ag_prng_t *rng)
{
    // Log-normal located at the ~15 min median; clamp into the [1 min, 1 h] band.
    // Mirrors ag_evict_draw_base_ttl's shape so rotation churn and lineage death
    // share one statistical idiom.
    float mu = logf((float)AG_LIFE_ROTATE_MEDIAN_MS);
    float z = ag_prng_gauss(rng, 0.0f, 1.0f);
    float ms = expf(mu + AG_LIFE_ROTATE_SIGMA * z);
    if (ms < (float)AG_LIFE_ROTATE_MIN_MS) ms = (float)AG_LIFE_ROTATE_MIN_MS;
    if (ms > (float)AG_LIFE_ROTATE_MAX_MS) ms = (float)AG_LIFE_ROTATE_MAX_MS;
    return (uint32_t)ms;
}

bool ag_life_rotation_due(const ag_beacon_record_t *r, uint32_t now_ms)
{
    // A rotation is due once the scheduled deadline passes. next_rotate_ms == 0
    // means unscheduled (stationary, or not yet armed) — never due.
    return r->next_rotate_ms != 0u && now_ms >= r->next_rotate_ms;
}
