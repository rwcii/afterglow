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

    // (3b) re-sighting a mesh-absorbed (RELAYED) record reclaims provenance: a
    //      record that arrived over the mesh (RELAYED set, origin_node = a FOREIGN
    //      first-capturer, hop_ttl exhausted) and is then air-captured locally
    //      becomes genuinely own-origin. The merge must clear RELAYED AND reset
    //      origin_node to our node id AND reset hop_ttl to 0, so the provenance
    //      flag and origin_node never disagree (else carryable would treat it as
    //      own while its return-to-source guard keyed off the foreign origin).
    {
        const uint32_t SELF_NODE = 0x0ABCD123u;
        const uint32_t FOREIGN   = 0x0FEED999u;
        // Force slab[0] into the "absorbed relay" state directly.
        slab[0].flags |= AG_FLAG_RELAYED;
        slab[0].origin_node = FOREIGN;
        slab[0].hop_ttl = 0;            // exhausted relay
        ag_capture_t cr = mk_cap(AG_PROTO_BLE, -50, fa, 10, 4000);
        int m = ag_pool_admit(slab, &count, CAP, &cr, addr_a, 1, 4000,
                              SELF_NODE, &evp, &rng);
        CHECK_MSG(m == 0, "re-sight should merge into slot 0, got %d", m);
        CHECK_MSG(!(slab[0].flags & AG_FLAG_RELAYED),
                  "RELAYED must clear on local re-sighting");
        CHECK_MSG(slab[0].origin_node == SELF_NODE,
                  "origin_node must be reclaimed as our node on re-sight");
        CHECK_MSG(slab[0].hop_ttl == 0, "re-sighted own capture must be ttl0");
    }

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

    // (5b) temporal-deviation EWMA: a source held at a STEADY level converges
    //      its rssi_dev_ewma toward ~0 regardless of which level it sits at, so
    //      several steady sources at very DIFFERENT levels (big spatial spread)
    //      still each report near-zero temporal deviation. This is the
    //      headline: a quiet room of steady-but-spread devices must not look
    //      "mobile". Build it on a fresh slab to keep counts clean.
    {
        enum { CAP2 = 8 };
        ag_beacon_record_t s2[CAP2];
        uint16_t c2c = 0;
        // three sources pinned at -40, -60, -85 dBm (20+ dB spatial spread),
        // each rock-steady across many sightings.
        int8_t levels[3] = {-40, -60, -85};
        for (int s = 0; s < 3; s++) {
            uint8_t fr[6] = {0x10, 0x20, 0x30, 0x40, 0x50, (uint8_t)s};
            for (int t = 0; t < 40; t++) {
                ag_capture_t cs = mk_cap(AG_PROTO_BLE, levels[s], fr, 6,
                                         (uint64_t)(t * 1000));
                ag_pool_admit(s2, &c2c, CAP2, &cs, fr, 1,
                              (uint32_t)(1000 + t * 1000), 0, &evp, &rng);
            }
        }
        CHECK(c2c == 3);
        for (uint16_t z = 0; z < c2c; z++) {
            CHECK_MSG(s2[z].rssi_dev_ewma <= 1,
                      "steady source %u shows temporal dev %u (should be ~0)",
                      z, s2[z].rssi_dev_ewma);
        }

        // (5c) a JITTERY source (alternating ±10 dB around its mean) accumulates
        //      a clearly NON-zero temporal deviation — the case ghosts SHOULD
        //      track. Same mean as source 0, so spatial spread is unchanged.
        uint8_t fj[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0xEE};
        uint16_t before_j = c2c;
        for (int t = 0; t < 60; t++) {
            int8_t r = (t & 1) ? (int8_t)-50 : (int8_t)-70;  // mean -60, ±10
            ag_capture_t cj = mk_cap(AG_PROTO_BLE, r, fj, 6, (uint64_t)(t * 1000));
            ag_pool_admit(s2, &c2c, CAP2, &cj, fj, 1,
                          (uint32_t)(1000 + t * 1000), 0, &evp, &rng);
        }
        CHECK(c2c == before_j + 1);
        ag_beacon_record_t *jit = &s2[before_j];
        CHECK_MSG(jit->rssi_dev_ewma >= 5,
                  "jittery source temporal dev %u too low (should track motion)",
                  jit->rssi_dev_ewma);
    }

    // (6) sweep far in the future evicts (records exceed TTL), and the slab
    //     stays compacted (count shrinks, survivors in [0,count)).
    uint16_t before = count;
    uint32_t far = 9000 + (uint32_t)(evp.ttl_max_s * 1000.0f) * 4u;
    ag_pool_evict_sweep(slab, &count, CAP, far, &evp, &rng);
    CHECK_MSG(count < before, "nothing evicted after 4x max TTL (count=%u)", count);
    for (uint16_t z = 0; z < count; z++) CHECK(slab[z].rec_id != 0 || slab[z].payload_len);

    TEST_SUMMARY();
}
