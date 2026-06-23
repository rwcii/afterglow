// pool_logic.c — portable pool record helpers (host-tested in test_pool.c).
#include "pool_logic.h"
#include "ag_core/ag_eligible.h" // ag_adv_kind_merge — stickiest-unsafe-wins
#include <string.h>

uint16_t ag_pool_rec_id(uint8_t addr_type, const uint8_t orig_addr[6],
                        const uint8_t *payload, uint8_t payload_len)
{
    // FNV-1a 32-bit over (addr_type || orig_addr || payload), folded to 16 bits.
    uint32_t h = 2166136261u;
    h = (h ^ addr_type) * 16777619u;
    for (int i = 0; i < 6; i++) h = (h ^ orig_addr[i]) * 16777619u;
    for (uint8_t i = 0; i < payload_len; i++) h = (h ^ payload[i]) * 16777619u;
    return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

int8_t ag_pool_rssi_ewma(int8_t prev_ewma, int8_t sample)
{
    // alpha = 1/8: ewma += (sample - ewma) / 8. Integer math, rounds toward 0.
    int delta = (int)sample - (int)prev_ewma;
    return (int8_t)((int)prev_ewma + (delta >> 3));
}

uint8_t ag_pool_rssi_dev_ewma(uint8_t prev_dev, int8_t prev_ewma, int8_t sample)
{
    // dev += (|sample - prev_ewma| - dev) / 8. Integer math, rounds toward 0.
    int d = (int)sample - (int)prev_ewma;
    if (d < 0) d = -d;
    int delta = d - (int)prev_dev;
    return (uint8_t)((int)prev_dev + (delta >> 3));
}

int ag_pool_find(const ag_beacon_record_t *slab, uint16_t count,
                 uint8_t addr_type, const uint8_t orig_addr[6],
                 const uint8_t *payload, uint8_t payload_len)
{
    // Identity is the device: addr_type + advertising address. The payload is
    // NOT part of the match — real beacons rotate mutable bytes (counters,
    // timestamps) every transmission, so keying identity on the payload would
    // mint a fresh record per sighting and obs_count could never accumulate.
    // (payload/payload_len are unused now but kept in the signature so callers
    // and the host tests stay stable.)
    (void)payload; (void)payload_len;
    for (uint16_t i = 0; i < count; i++) {
        if (slab[i].addr_type == addr_type &&
            memcmp(slab[i].orig_addr, orig_addr, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int ag_pool_admit(ag_beacon_record_t *slab, uint16_t *count, uint16_t capacity,
                  const ag_capture_t *cap, const uint8_t orig_addr[6],
                  uint8_t addr_type, uint32_t now_ms, uint32_t node_id,
                  const ag_evict_params_t *evp, ag_prng_t *rng)
{
    uint8_t plen = (uint8_t)(cap->frame_len > 31 ? 31 : cap->frame_len);
    int idx = ag_pool_find(slab, *count, addr_type, orig_addr, cap->frame, plen);

    if (idx >= 0) {
        ag_beacon_record_t *r = &slab[idx];
        r->rssi_last = cap->rssi;
        // Temporal deviation first: it measures the new sample against the PRIOR
        // mean, so it must run before rssi_ewma absorbs the sample.
        r->rssi_dev_ewma = ag_pool_rssi_dev_ewma(r->rssi_dev_ewma, r->rssi_ewma,
                                                 cap->rssi);
        r->rssi_ewma = ag_pool_rssi_ewma(r->rssi_ewma, cap->rssi);
        if (r->obs_count < 255) r->obs_count++;
        // Refresh the stored payload to the latest sighting so replay emits the
        // current AdvData. rec_id stays fixed at its first-sighting value: it is
        // the stable per-record identifier the mesh seen-set/dedup keys on.
        memcpy(r->payload, cap->frame, plen);
        r->payload_len = plen;
        // Estimate cadence from the inter-arrival gap (ms → 0.625ms TU units).
        if (now_ms > r->last_seen_ms) {
            uint32_t gap = now_ms - r->last_seen_ms;
            uint32_t tu = (gap * 8u) / 5u;             // ms / 0.625
            r->interval_q = (uint16_t)(tu > 0xFFFFu ? 0xFFFFu : tu);
        }
        r->last_seen_ms = now_ms;
        r->channel = cap->channel;
        // Stickiest-unsafe-wins: a source that EVER advertised connectably or
        // scannably under this identity stays that kind forever, so a later
        // broadcast-only sighting can't relax it back into eligibility.
        r->adv_kind = (uint8_t)ag_adv_kind_merge((ag_adv_kind_t)r->adv_kind,
                                                 cap->adv_kind);
        // It's back — clear DEPARTING. A local re-sighting also makes this record
        // genuinely own-origin: we air-captured it ourselves now. If it had been
        // absorbed over the mesh (RELAYED set, origin_node = the foreign first-
        // capturer's id), take over BOTH provenance signals together so they
        // never disagree — clear RELAYED and reclaim origin_node as our own node.
        // (Leaving origin_node foreign while clearing RELAYED would let carryable
        // treat the record as own yet key its return-to-source guard off a
        // foreign origin.)
        if (r->flags & AG_FLAG_RELAYED) {
            r->origin_node = node_id;
            r->hop_ttl = 0;        // own fresh capture: ttl0 is carryable, not exhausted
        }
        r->flags = (uint8_t)(r->flags & ~(AG_FLAG_DEPARTING | AG_FLAG_RELAYED));
        return idx;
    }

    // First sighting → insert if room.
    if (*count >= capacity) return -1;
    ag_beacon_record_t *r = &slab[*count];
    memset(r, 0, sizeof(*r));
    r->proto = (uint8_t)cap->proto;
    r->cls = AG_CLASS_TENTATIVE;
    // Record the observed PDU behavior as-is. memset above zeroed it to
    // NONCONN_NONSCAN, which would be a fail-OPEN default, so set it explicitly
    // from the capture (AG_ADV_UNKNOWN when the backend couldn't parse it).
    r->adv_kind = (uint8_t)cap->adv_kind;
    memcpy(r->orig_addr, orig_addr, 6);
    r->addr_type = addr_type;
    r->payload_len = plen;
    memcpy(r->payload, cap->frame, plen);
    r->rec_id = ag_pool_rec_id(addr_type, orig_addr, cap->frame, plen);
    r->origin_node = node_id;
    r->hop_ttl = 0;            // air-captured local origin
    // RELAYED stays clear (memset above): a local air-capture is own-origin, so
    // this record is carryable at ttl0 (a fresh capture seeds its first hop).
    r->rssi_last = cap->rssi;
    r->rssi_ewma = cap->rssi;
    r->obs_count = 1;
    r->channel = cap->channel;
    r->first_seen_ms = now_ms;
    r->last_seen_ms = now_ms;
    r->base_ttl_s = ag_evict_draw_base_ttl(evp, rng);
    r->p_virt = 0.0f;
    r->p_center = 0.0f;
    int new_idx = (int)(*count);
    (*count)++;
    return new_idx;
}

// Cheap score percentile: rank by rssi_ewma (stronger = higher percentile).
static float score_percentile_of(const ag_beacon_record_t *slab, uint16_t count,
                                 uint16_t idx)
{
    if (count <= 1) return 1.0f;
    int weaker = 0;
    int8_t mine = slab[idx].rssi_ewma;
    for (uint16_t i = 0; i < count; i++) {
        if (i == idx) continue;
        if (slab[i].rssi_ewma < mine) weaker++;
    }
    return (float)weaker / (float)(count - 1);
}

uint16_t ag_pool_evict_sweep(ag_beacon_record_t *slab, uint16_t *count,
                             uint16_t carry_cap, uint32_t now_ms,
                             const ag_evict_params_t *evp, ag_prng_t *rng)
{
    uint16_t n = *count;
    if (n == 0) return 0;
    float fill_frac = carry_cap ? (float)n / (float)carry_cap : 1.0f;

    uint16_t write = 0;
    for (uint16_t i = 0; i < n; i++) {
        float age_s = (now_ms >= slab[i].first_seen_ms)
                          ? (float)(now_ms - slab[i].first_seen_ms) / 1000.0f
                          : 0.0f;
        float pct = score_percentile_of(slab, n, i);
        float base_ttl = slab[i].base_ttl_s > 1.0f ? slab[i].base_ttl_s : 1.0f;
        bool evict = ag_evict_decide(evp, rng, age_s, base_ttl, fill_frac, pct);
        if (!evict) {
            if (write != i) slab[write] = slab[i];
            write++;
        }
    }
    uint16_t evicted = (uint16_t)(n - write);
    *count = write;
    return evicted;
}
