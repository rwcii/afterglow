// test_meshguard.c — mesh loop/amplification guards.
//
// Core property: a record must NOT ping-pong across the mesh forever
// (unbounded amplification). Assert: TTL bounds hops, origin pinning stops
// return-to-origin, the LRU seen-set stops resurrection (and is time-bounded so
// it can't permanently blacklist a content hash), and diffusion is bounded.
#include "ag_core/ag_meshguard.h"
#include "test_util.h"

int main(void)
{
    TEST_BEGIN("meshguard");

    static uint16_t ids[64];
    static uint32_t stamps[64];

    // (1) TTL bounds the number of hops a record can travel. Simulate a record
    //     hopping node-to-node; each accept decrements TTL; at 0 it is replay-
    //     only and never re-meshed.
    {
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        uint8_t ttl = AG_TTL_INIT;
        int hops = 0;
        for (int node = 1; node <= 20; node++) {
            uint8_t out;
            // each node has a distinct self_node and empty pool for this id
            ag_mesh_verdict_t v = ag_mesh_evaluate(&seen, /*rec_id*/0x1234,
                ttl, /*origin*/0xAAAA, /*self*/(uint32_t)node,
                /*in_pool*/false, /*pool_ttl*/0, &out);
            if (v == AG_MESH_ACCEPT) { hops++; ttl = out; }
            else break;
        }
        CHECK_MSG(hops <= AG_TTL_INIT, "record hopped %d times, TTL_INIT=%d",
                  hops, AG_TTL_INIT);
        CHECK(hops >= 1);
    }

    // (2) origin pinning: a node never absorbs a record it air-captured.
    {
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        uint8_t out;
        ag_mesh_verdict_t v = ag_mesh_evaluate(&seen, 0x55, AG_TTL_INIT,
            /*origin*/0xBEEF, /*self*/0xBEEF, false, 0, &out);
        CHECK(v == AG_MESH_DROP_OWN_ORIGIN);
    }

    // (3) seen-set stops resurrection: once accepted then evicted from the
    //     pool, re-delivery within the LRU horizon is dropped (not re-admitted).
    {
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        uint8_t out;
        ag_mesh_verdict_t v1 = ag_mesh_evaluate(&seen, 0x77, AG_TTL_INIT,
            0x1, 0x2, false, 0, &out);
        CHECK(v1 == AG_MESH_ACCEPT);
        // it left the pool (expired) but is still in the seen-set:
        ag_mesh_verdict_t v2 = ag_mesh_evaluate(&seen, 0x77, AG_TTL_INIT,
            0x1, 0x2, false, 0, &out);
        CHECK_MSG(v2 == AG_MESH_DROP_SEEN, "evicted record was resurrected");
    }

    // (4) refresh-to-lower: a record live in the pool can never have its reach
    //     raised by re-delivery with a higher TTL.
    {
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        uint8_t out;
        ag_mesh_verdict_t v = ag_mesh_evaluate(&seen, 0x88, /*inbound*/3,
            0x1, 0x2, /*in_pool*/true, /*pool_ttl*/1, &out);
        CHECK(v == AG_MESH_REFRESH_LOWER);
        CHECK_MSG(out == 1, "refresh raised TTL to %u (must keep lower=1)", out);
    }

    // (5) seen-set is a TIME-BOUNDED LRU: under capacity pressure an old id ages
    //     out and becomes re-admittable (no permanent blacklist).
    {
        static uint16_t small_ids[4];
        static uint32_t small_stamps[4];
        ag_seen_t seen;
        ag_seen_init(&seen, small_ids, small_stamps, 4);
        // insert id 100, then flood with 8 distinct ids → 100 is evicted.
        CHECK(ag_seen_check_and_add(&seen, 100) == false);
        for (int i = 200; i < 208; i++) ag_seen_check_and_add(&seen, (uint16_t)i);
        CHECK_MSG(!ag_seen_contains(&seen, 100),
                  "id 100 should have aged out of the 4-entry LRU");
        // re-adding 100 is now a fresh insert (re-admittable), not a permanent hit.
        CHECK(ag_seen_check_and_add(&seen, 100) == false);
    }

    // (6) amplification bound: from a single air-capture, the total number of
    //     accepting nodes across a small mesh is bounded (deduped, not 2^n).
    {
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        int accepts = 0;
        // 30 delivery attempts of the SAME rec_id to the SAME node's seen-set:
        // exactly one accept, the rest deduped.
        uint8_t out;
        for (int i = 0; i < 30; i++) {
            ag_mesh_verdict_t v = ag_mesh_evaluate(&seen, 0xABCD, AG_TTL_INIT,
                0x1, 0x2, false, 0, &out);
            if (v == AG_MESH_ACCEPT) accepts++;
        }
        CHECK_MSG(accepts == 1, "same record accepted %d times at one node "
                  "(amplification!)", accepts);
    }

    TEST_SUMMARY();
}
