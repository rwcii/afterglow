// test_pool.c — portable pool record logic (rec_id, merge/insert, sweep).
#include "pool_logic.h"
#include "test_util.h"
#include <string.h>

static ag_capture_t mk_cap(ag_proto_t proto, int8_t rssi, const uint8_t *frame,
                           uint16_t len, uint64_t ts_us)
{
    ag_capture_t c = {0};
    c.proto = proto; c.rssi = rssi; c.channel = 37; c.ts_us = ts_us;
    c.frame = frame; c.frame_len = len;
    return c;
}

int main(void)
{
    TEST_BEGIN("pool");
    ag_prng_t rng; ag_prng_seed(&rng, 0xC0FFEEu);
    ag_evict_params_t evp = ag_evict_defaults();

    // identity = first 6 bytes (BLE AdvA) for these frames.
    uint8_t fa[10] = {1, 2, 3, 4, 5, 6, 0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t fb[10] = {9, 8, 7, 6, 5, 4, 0x11, 0x22, 0x33, 0x44};
    uint8_t addr_a[6] = {1, 2, 3, 4, 5, 6};
    uint8_t addr_b[6] = {9, 8, 7, 6, 5, 4};

    // (1) rec_id is stable for identical identity+payload, differs otherwise.
    uint16_t id_a1 = ag_pool_rec_id(1, addr_a, fa, 10);
    uint16_t id_a2 = ag_pool_rec_id(1, addr_a, fa, 10);
    uint16_t id_b  = ag_pool_rec_id(1, addr_b, fb, 10);
    CHECK(id_a1 == id_a2);
    CHECK_MSG(id_a1 != id_b, "distinct identities collided");

    enum { CAP = 8 };
    ag_beacon_record_t slab[CAP];
    uint16_t count = 0;

    // (2) first sighting inserts; class TENTATIVE; obs_count 1.
    ag_capture_t c1 = mk_cap(AG_PROTO_BLE, -60, fa, 10, 1000);
    int i = ag_pool_admit(slab, &count, CAP, &c1, addr_a, 1, 1000, 0, &evp, &rng);
    CHECK(i == 0 && count == 1);
    CHECK(slab[0].cls == AG_CLASS_TENTATIVE);
    CHECK(slab[0].obs_count == 1);
    CHECK(slab[0].base_ttl_s >= evp.ttl_min_s && slab[0].base_ttl_s <= evp.ttl_max_s);

    // (3) second sighting of same identity MERGES (no new slot), bumps obs,
    //     updates ewma toward the new sample.
    ag_capture_t c2 = mk_cap(AG_PROTO_BLE, -40, fa, 10, 2000);
    int j = ag_pool_admit(slab, &count, CAP, &c2, addr_a, 1, 2000, 0, &evp, &rng);
    CHECK_MSG(j == 0 && count == 1, "merge created a new slot (count=%u)", count);
    CHECK(slab[0].obs_count == 2);
    CHECK_MSG(slab[0].rssi_ewma > -60 && slab[0].rssi_ewma <= -40,
              "ewma did not move toward sample: %d", slab[0].rssi_ewma);
    CHECK(slab[0].interval_q > 0); // cadence estimated from the 1000ms gap

    // (4) distinct identity inserts a second record.
    ag_capture_t c3 = mk_cap(AG_PROTO_BLE, -55, fb, 10, 2500);
    int k = ag_pool_admit(slab, &count, CAP, &c3, addr_b, 1, 2500, 0, &evp, &rng);
    CHECK(k == 1 && count == 2);

    // (5) full slab returns -1 for a brand-new identity.
    for (uint16_t f = count; f < CAP; f++) {
        uint8_t fr[6] = {0x80, 0x80, 0x80, 0x80, 0x80, (uint8_t)f};
        ag_capture_t cf = mk_cap(AG_PROTO_BLE, -70, fr, 6, 3000 + f);
        ag_pool_admit(slab, &count, CAP, &cf, fr, 1, 3000 + f, 0, &evp, &rng);
    }
    CHECK(count == CAP);
    uint8_t frnew[6] = {0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
    ag_capture_t cfull = mk_cap(AG_PROTO_BLE, -70, frnew, 6, 9000);
    int full = ag_pool_admit(slab, &count, CAP, &cfull, frnew, 1, 9000, 0, &evp, &rng);
    CHECK_MSG(full < 0, "admit into full slab should fail, got %d", full);

    // (6) sweep far in the future evicts (records exceed TTL), and the slab
    //     stays compacted (count shrinks, survivors in [0,count)).
    uint16_t before = count;
    uint32_t far = 9000 + (uint32_t)(evp.ttl_max_s * 1000.0f) * 4u;
    ag_pool_evict_sweep(slab, &count, CAP, far, &evp, &rng);
    CHECK_MSG(count < before, "nothing evicted after 4x max TTL (count=%u)", count);
    for (uint16_t z = 0; z < count; z++) CHECK(slab[z].rec_id != 0 || slab[z].payload_len);

    TEST_SUMMARY();
}
