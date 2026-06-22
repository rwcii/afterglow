// test_mesh.c — portable mesh transfer math (carry gate, subset, fragmentation).
#include "mesh_logic.h"
#include "test_util.h"
#include <string.h>

// build a record with the knobs the carry/subset logic reads.
static ag_beacon_record_t mk_rec(uint8_t flags, uint8_t obs_count,
                                 uint32_t origin_node, uint8_t hop_ttl,
                                 uint32_t last_seen_ms)
{
    ag_beacon_record_t r;
    memset(&r, 0, sizeof(r));
    r.cls = AG_CLASS_STATIC_RANDOM_BLE;
    r.flags = flags;
    r.obs_count = obs_count;
    r.origin_node = origin_node;
    r.hop_ttl = hop_ttl;
    r.last_seen_ms = last_seen_ms;
    r.payload_len = 20;
    return r;
}

int main(void)
{
    TEST_BEGIN("mesh");
    ag_prng_t rng; ag_prng_seed(&rng, 0xABCDEFu);

    const uint8_t REPLAY = AG_FLAG_REPLAY_ELIGIBLE;
    const uint16_t SELF = 0x1111;
    const uint16_t PEER = 0x2222;

    // (1) carry gate is STRICTER than replay: replay-eligible but obs_count==1
    //     is NOT carry-eligible (needs multi-sweep persistence).
    {
        ag_beacon_record_t one = mk_rec(REPLAY, 1, 0x9000, 0, 1000);
        ag_beacon_record_t two = mk_rec(REPLAY, 2, 0x9000, 0, 1000);
        CHECK_MSG(!ag_mesh_carry_eligible(&one),
                  "obs_count==1 must not be carry-eligible");
        CHECK(ag_mesh_carry_eligible(&two));
    }

    // (2) not replay-eligible -> never carry-eligible, regardless of obs_count.
    {
        ag_beacon_record_t r = mk_rec(0, 9, 0x9000, 0, 1000);
        CHECK(!ag_mesh_carry_eligible(&r));
    }

    // (3) departing records are excluded even when otherwise carryable.
    {
        ag_beacon_record_t r = mk_rec(REPLAY | AG_FLAG_DEPARTING, 5, 0x9000, 0, 1000);
        CHECK(!ag_mesh_carry_eligible(&r));
    }

    // (4) subset: peer-origin records are never selected; fraction targets the
    //     carry-eligible set; cap bounds the result.
    {
        enum { N = 10 };
        ag_beacon_record_t slab[N];
        for (int i = 0; i < N; i++) {
            // half of the records originate at the PEER (low-16 == PEER), which
            // must be excluded; the rest at unrelated nodes.
            uint32_t origin = (i % 2 == 0) ? (0xDEAD0000u | PEER)
                                           : (0x00010000u | (uint32_t)(0x3000 + i));
            slab[i] = mk_rec(REPLAY, 4, origin, 1, (uint32_t)(1000 + i * 10));
        }
        // 5 carry-eligible (the odd indices). fraction 1.0 -> all 5, then cap 3.
        uint16_t out[N];
        uint8_t n = ag_mesh_select_subset(slab, N, SELF, PEER, 1.0f, 3, &rng,
                                          out, N);
        CHECK_MSG(n == 3, "cap 3 not honored, got %u", n);
        for (uint8_t s = 0; s < n; s++) {
            uint16_t idx = out[s];
            CHECK_MSG(idx % 2 == 1, "selected peer-origin record at idx %u", idx);
            uint16_t lo = (uint16_t)(slab[idx].origin_node & 0xFFFFu);
            CHECK(lo != PEER);
            // no duplicate index.
            for (uint8_t t = s + 1; t < n; t++) CHECK(out[t] != idx);
        }
    }

    // (5) subset: fraction ~0.5 over 8 carry-eligible -> ~4 selected (rounded).
    {
        enum { N = 8 };
        ag_beacon_record_t slab[N];
        for (int i = 0; i < N; i++)
            slab[i] = mk_rec(REPLAY, 3, 0x00010000u | (uint32_t)(0x4000 + i), 1,
                             (uint32_t)(5000 + i * 7));
        uint16_t out[N];
        uint8_t n = ag_mesh_select_subset(slab, N, SELF, PEER, 0.5f, 16, &rng,
                                          out, N);
        CHECK_MSG(n == 4, "0.5*8 should round to 4, got %u", n);
    }

    // (6) subset: out_max bounds the result below cap/fraction.
    {
        enum { N = 6 };
        ag_beacon_record_t slab[N];
        for (int i = 0; i < N; i++)
            slab[i] = mk_rec(REPLAY, 3, 0x00010000u | (uint32_t)(0x5000 + i), 1,
                             (uint32_t)(6000 + i));
        uint16_t out[2];
        uint8_t n = ag_mesh_select_subset(slab, N, SELF, PEER, 1.0f, 16, &rng,
                                          out, 2);
        CHECK_MSG(n == 2, "out_max 2 not honored, got %u", n);
    }

    // (7) subset: a relayed record (foreign origin) at hop_ttl==0 is exhausted
    //     and skipped; a fresh air capture (self origin) at hop_ttl==0 is still
    //     carryable.
    {
        enum { N = 2 };
        ag_beacon_record_t slab[N];
        slab[0] = mk_rec(REPLAY, 4, 0x00010000u | 0x7777u, 0, 8000); // relayed, exhausted
        slab[1] = mk_rec(REPLAY, 4, 0x00010000u | SELF, 0, 8000);    // own air capture
        uint16_t out[N];
        uint8_t n = ag_mesh_select_subset(slab, N, SELF, PEER, 1.0f, 16, &rng,
                                          out, N);
        CHECK_MSG(n == 1, "only the own-origin ttl0 record is carryable, got %u", n);
        CHECK(out[0] == 1);
    }

    // (8) no carry-eligible records -> empty selection.
    {
        ag_beacon_record_t slab[3];
        for (int i = 0; i < 3; i++) slab[i] = mk_rec(REPLAY, 1, 0x9000, 1, 1000);
        uint16_t out[3];
        uint8_t n = ag_mesh_select_subset(slab, 3, SELF, PEER, 1.0f, 16, &rng,
                                          out, 3);
        CHECK(n == 0);
    }

    // (9) fragmentation math: ceil(len/body), 0 -> 0, clamp to 15.
    CHECK_MSG(ag_mesh_frag_count(31, 20) == 2, "31/20 should ceil to 2");
    CHECK(ag_mesh_frag_count(20, 20) == 1);
    CHECK(ag_mesh_frag_count(21, 20) == 2);
    CHECK(ag_mesh_frag_count(40, 20) == 2);
    CHECK(ag_mesh_frag_count(0, 20) == 0);   // empty payload
    CHECK(ag_mesh_frag_count(1, 20) == 1);
    // >15 fragments clamped: 200 bytes / 1 -> 200, clamped to 15.
    CHECK_MSG(ag_mesh_frag_count(200, 1) == 15, "frag total must clamp to 15");
    // body_bytes==0 guarded to 1 (no div-by-zero), then clamped.
    CHECK(ag_mesh_frag_count(10, 0) == 10);

    // --- contact table + per-peer cooldown ---------------------------------
    // The cooldown/eviction decision behind mesh.c's contact_seen(): a HELLO
    // from a peer triggers a transfer on first contact, then is gated until the
    // per-peer cooldown elapses; the table evicts the stalest peer when full.
    const uint32_t COOLDOWN = 120000;   // ms (config default, clamped [60s,180s])

    // (10) first contact -> transfer; re-contact within cooldown -> gated;
    //      after the cooldown elapses -> transfer again.
    {
        ag_contact_t tbl[8];
        memset(tbl, 0, sizeof(tbl));
        const uint32_t P = 0xABCDEF;
        CHECK_MSG(ag_contact_should_transfer(tbl, 8, P, 1000, COOLDOWN),
                  "first contact must transfer");
        // re-contact partway through the cooldown -> gated (no transfer).
        CHECK_MSG(!ag_contact_should_transfer(tbl, 8, P, 1000 + COOLDOWN / 2, COOLDOWN),
                  "re-contact inside cooldown must be gated");
        // exactly at the cooldown boundary -> elapsed, transfer.
        CHECK_MSG(ag_contact_should_transfer(tbl, 8, P, 1000 + COOLDOWN, COOLDOWN),
                  "contact at cooldown boundary must transfer");
    }

    // (11) a gated re-contact must NOT slide the cooldown window forward (the
    //      slot's last-transfer time is left untouched while cooling down).
    {
        ag_contact_t tbl[8];
        memset(tbl, 0, sizeof(tbl));
        const uint32_t P = 0x010203;
        CHECK(ag_contact_should_transfer(tbl, 8, P, 0, COOLDOWN));
        // two gated re-contacts inside the window...
        CHECK(!ag_contact_should_transfer(tbl, 8, P, 40000, COOLDOWN));
        CHECK(!ag_contact_should_transfer(tbl, 8, P, 80000, COOLDOWN));
        // ...still measured from the ORIGINAL transfer: at t=COOLDOWN it fires.
        CHECK_MSG(ag_contact_should_transfer(tbl, 8, P, COOLDOWN, COOLDOWN),
                  "gated re-contacts must not push the cooldown window forward");
    }

    // (12) own-HELLO never reaches the contact table: mesh_try_consume drops a
    //      HELLO whose peer-id equals our own NodeID low-24 before calling the
    //      contact logic, so the table is only ever asked about foreign peers.
    //      Asserting the gate's contract here: a distinct peer is always a fresh
    //      first contact (the own-HELLO is filtered upstream, never inserted).
    {
        ag_contact_t tbl[8];
        memset(tbl, 0, sizeof(tbl));
        const uint32_t SELF24 = 0x0F0F0F;
        const uint32_t PEER24 = 0x0E0E0E;
        // The peer's HELLO is a first contact -> transfer.
        CHECK(ag_contact_should_transfer(tbl, 8, PEER24, 5000, COOLDOWN));
        // Our own id is never passed to the table; only PEER24 occupies a slot.
        int used = 0, has_self = 0;
        for (int i = 0; i < 8; i++) {
            if (tbl[i].used) used++;
            if (tbl[i].used && tbl[i].peer_lo24 == SELF24) has_self = 1;
        }
        CHECK_MSG(used == 1, "exactly one slot used, got %d", used);
        CHECK_MSG(!has_self, "own NodeID must never occupy a contact slot");
    }

    // (13) a 9th distinct peer evicts the stalest slot (LRU by last-transfer).
    {
        ag_contact_t tbl[8];
        memset(tbl, 0, sizeof(tbl));
        // Fill all 8 slots with distinct peers at increasing timestamps; peer
        // 0x100 is the stalest (oldest last-transfer).
        for (int i = 0; i < 8; i++) {
            uint32_t peer = 0x100u + (uint32_t)i;
            CHECK(ag_contact_should_transfer(tbl, 8, peer, 1000u + (uint32_t)i * 1000u, COOLDOWN));
        }
        // A 9th distinct peer: table full -> stalest (0x100) evicted.
        const uint32_t NEW = 0x999;
        CHECK_MSG(ag_contact_should_transfer(tbl, 8, NEW, 50000, COOLDOWN),
                  "9th distinct peer must transfer (fresh insert)");
        int has_new = 0, has_stalest = 0;
        for (int i = 0; i < 8; i++) {
            if (tbl[i].used && tbl[i].peer_lo24 == NEW) has_new = 1;
            if (tbl[i].used && tbl[i].peer_lo24 == 0x100u) has_stalest = 1;
        }
        CHECK_MSG(has_new, "the new peer must occupy a slot");
        CHECK_MSG(!has_stalest, "the stalest peer (0x100) must have been evicted");
    }

    // (14) time wrap-around / now < last guard: a clock that appears to run
    //      backwards (monotonic ms wrap or reset) is treated as cooldown-elapsed
    //      rather than wrapping the unsigned subtraction into a huge gap that
    //      would wedge the peer "cooling down" forever.
    {
        ag_contact_t tbl[8];
        memset(tbl, 0, sizeof(tbl));
        const uint32_t P = 0x55AA55;
        // First contact near the top of the 32-bit ms range.
        CHECK(ag_contact_should_transfer(tbl, 8, P, 0xFFFFF000u, COOLDOWN));
        // Clock wraps past zero (now < last): must NOT be stuck cooling down.
        CHECK_MSG(ag_contact_should_transfer(tbl, 8, P, 0x00000100u, COOLDOWN),
                  "post-wrap contact must transfer, not wedge on a huge gap");
    }

    // (15) defensive arg guards: NULL table / zero capacity never transfer.
    {
        ag_contact_t tbl[1];
        memset(tbl, 0, sizeof(tbl));
        CHECK(!ag_contact_should_transfer(NULL, 8, 0x1, 0, COOLDOWN));
        CHECK(!ag_contact_should_transfer(tbl, 0, 0x1, 0, COOLDOWN));
    }

    TEST_SUMMARY();
}
