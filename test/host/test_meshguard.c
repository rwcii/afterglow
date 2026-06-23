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

    // (1b) wire-TTL clamp: the hop-TTL is a single attacker-controllable byte
    //      (0..255), but diffusion must stay bounded to AG_TTL_INIT hops. A frame
    //      declaring an inflated ttl (e.g. 255) must be clamped to the ceiling on
    //      acceptance, never stored above it — otherwise one crafted frame could
    //      amplify across far more than AG_TTL_INIT hops.
    {
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        uint8_t out = 0xFF;
        ag_mesh_verdict_t v = ag_mesh_evaluate(&seen, /*rec_id*/0x2345,
            /*inbound_ttl*/255, /*origin*/0xAAAA, /*self*/0x1,
            /*in_pool*/false, /*pool_ttl*/0, &out);
        CHECK(v == AG_MESH_ACCEPT);
        // stored ttl = clamp(255, TTL_INIT) - 1 = TTL_INIT - 1.
        CHECK_MSG(out == (uint8_t)(AG_TTL_INIT - 1),
                  "inflated wire ttl=255 stored as %u (must clamp to TTL_INIT-1=%d)",
                  out, AG_TTL_INIT - 1);
        // And the clamp must bound the in-pool refresh path too. Use a pool_ttl
        // ABOVE the ceiling so the clamp is the ONLY thing that bounds the result:
        // without the clamp, refresh-lower would store min(255, pool_ttl) =
        // pool_ttl (> AG_TTL_INIT); with the clamp, inbound is capped to
        // AG_TTL_INIT first, so the stored ttl is min(AG_TTL_INIT, pool_ttl) =
        // AG_TTL_INIT. (pool_ttl can only exceed the ceiling if a prior bug stored
        // it there; the clamp is the backstop that re-bounds it on refresh.) This
        // assertion FAILS if the clamp is removed — the weaker pool_ttl < ceiling
        // case would pass either way and does not exercise the clamp.
        ag_seen_t seen2;
        ag_seen_init(&seen2, ids, stamps, 64);
        uint8_t out2 = 0xFF;
        ag_mesh_verdict_t v2 = ag_mesh_evaluate(&seen2, 0x2346, /*inbound*/255,
            0x1, 0x2, /*in_pool*/true, /*pool_ttl*/200, &out2);
        CHECK(v2 == AG_MESH_REFRESH_LOWER);
        CHECK_MSG(out2 == AG_TTL_INIT,
                  "inflated ttl refresh stored %u (clamp must bound it to "
                  "TTL_INIT=%d, not min(255,pool_ttl))", out2, AG_TTL_INIT);
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

    // (2b) origin keying width (the #54 lo16-collision fix): the absorb wrapper
    //      keys the own-origin guard on the wire origin's LOW 24 bits (biased
    //      above the lo24 space). Two distinct nodes that collide in their low 16
    //      bits but differ in bit[16..23] must NOT be treated as own-origin — a
    //      record from such a peer must still be ACCEPTed (it would have FALSE
    //      DROP_OWN_ORIGIN'd under the old lo16 keying). Model the exact keying
    //      mesh_absorb_inbound applies.
    {
        // self and a foreign origin that share lo16 (0x4321) but differ in lo24.
        const uint32_t self_full   = 0x00114321u;  // lo24 = 0x114321
        const uint32_t origin_full = 0x00224321u;  // lo24 = 0x224321, same lo16
        uint32_t origin_keyed = (origin_full & 0xFFFFFFu) | 0x1000000u;
        uint32_t self_keyed   = (self_full   & 0xFFFFFFu) | 0x1000000u;
        // Sanity: the old lo16 keying WOULD have collided (false own-origin).
        CHECK_MSG(((origin_full & 0xFFFFu) | 0x10000u) ==
                  ((self_full & 0xFFFFu) | 0x10000u),
                  "test premise: the two nodes collide under lo16 keying");
        // The new lo24 keying must distinguish them.
        CHECK_MSG(origin_keyed != self_keyed,
                  "lo24 keying must distinguish nodes that share only lo16");
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        uint8_t out;
        ag_mesh_verdict_t v = ag_mesh_evaluate(&seen, 0x56, AG_TTL_INIT,
            origin_keyed, self_keyed, false, 0, &out);
        CHECK_MSG(v == AG_MESH_ACCEPT,
                  "a peer sharing only lo16 must not false-trip own-origin");

        // And a genuine own-origin record (full lo24 match) still DROPs.
        uint32_t own_keyed = (self_full & 0xFFFFFFu) | 0x1000000u;
        ag_seen_t seen2;
        ag_seen_init(&seen2, ids, stamps, 64);
        ag_mesh_verdict_t v2 = ag_mesh_evaluate(&seen2, 0x57, AG_TTL_INIT,
            own_keyed, self_keyed, false, 0, &out);
        CHECK_MSG(v2 == AG_MESH_DROP_OWN_ORIGIN,
                  "a true own-origin record (lo24 match) must still drop");
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
    //     raised by re-delivery with a higher TTL. The verdict yields the LOWER
    //     ttl in *out, and the caller (mesh_absorb_inbound) MUST write that back
    //     to the live record's hop_ttl on REFRESH_LOWER — otherwise the lowering
    //     is computed but never applied. test_mesh_diffusion models that write-back
    //     (nd->pool_ttl = out_ttl on REFRESH_LOWER); this asserts the value.
    {
        ag_seen_t seen;
        ag_seen_init(&seen, ids, stamps, 64);
        uint8_t out;
        // higher inbound than the live record: must NOT raise (keep the lower).
        ag_mesh_verdict_t v = ag_mesh_evaluate(&seen, 0x88, /*inbound*/3,
            0x1, 0x2, /*in_pool*/true, /*pool_ttl*/1, &out);
        CHECK(v == AG_MESH_REFRESH_LOWER);
        CHECK_MSG(out == 1, "refresh raised TTL to %u (must keep lower=1)", out);

        // lower inbound than the live record: the verdict yields the lower
        // inbound value, which the caller applies to actually reduce reach.
        ag_seen_t seen2;
        ag_seen_init(&seen2, ids, stamps, 64);
        uint8_t out2;
        ag_mesh_verdict_t v2 = ag_mesh_evaluate(&seen2, 0x89, /*inbound*/1,
            0x1, 0x2, /*in_pool*/true, /*pool_ttl*/3, &out2);
        CHECK(v2 == AG_MESH_REFRESH_LOWER);
        CHECK_MSG(out2 == 1, "refresh must yield the lower ttl=1, got %u", out2);
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
