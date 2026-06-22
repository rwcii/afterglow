// test_mesh.c — portable mesh transfer math (carry gate, subset, fragmentation,
// fragment reassembly state machine, and the wire/pool rec_id round-trip).
#include "mesh_logic.h"
#include "pool_logic.h"     // ag_pool_rec_id — wire/pool rec_id round-trip
#include "ag_core/ag_meshguard.h"  // AG_TTL_INIT — legacy ttl band vs version
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

    // (0) wire version gate: only the current AG_MESH_VERSION is accepted; a peer
    //     advertising any other version (an older layout, or a future bump) is
    //     rejected so HELLO discovery / DATA admission never misparse a frame
    //     whose field layout they don't understand.
    CHECK_MSG(ag_mesh_version_ok(AG_MESH_VERSION), "current version must be accepted");
    CHECK_MSG(!ag_mesh_version_ok(0x01), "an older protocol version must be rejected");
    CHECK_MSG(!ag_mesh_version_ok((uint8_t)(AG_MESH_VERSION + 1)),
              "a future version bump must be rejected by this build");
    CHECK_MSG(!ag_mesh_version_ok(0x00), "version 0 must be rejected");
    // The DATA version byte shares a wire offset with a legacy DATA frame's ttl
    // field (0..AG_TTL_INIT). The gate must reject EVERY value a legacy ttl could
    // hold there, or a versionless legacy frame whose ttl matched the version
    // would be misparsed. Assert the whole legacy ttl band is rejected.
    for (uint8_t legacy_ttl = 0; legacy_ttl <= AG_TTL_INIT; legacy_ttl++)
        CHECK_MSG(!ag_mesh_version_ok(legacy_ttl),
                  "version must differ from every legacy ttl value (%u)", legacy_ttl);
    CHECK_MSG(AG_MESH_VERSION > AG_MESH_VERSION_MIN,
              "version must exceed the legacy-ttl band floor");

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

    // --- fragment reassembly state machine ---------------------------------
    // Body bytes per fragment are the ONE shared constant both emit and reassemble
    // key off, so a payload of 2*BODY+1 needs exactly 3 fragments.
    const uint8_t BODY = AG_MESH_FRAG_BODY;

    // (16) a multi-fragment record reassembles to the exact original payload only
    //      once every fragment has landed; out-of-order delivery is fine.
    {
        uint8_t payload[24];
        for (int i = 0; i < 24; i++) payload[i] = (uint8_t)(0xA0 + i);
        uint8_t frags = ag_mesh_frag_count(sizeof(payload), BODY);
        CHECK_MSG(frags == 2, "24B/%uB body should be 2 fragments, got %u", BODY, frags);

        ag_reasm_t r; ag_reasm_reset(&r);
        // deliver fragment 1 first (out of order): PARTIAL, not yet complete.
        uint8_t off1 = (uint8_t)(1 * BODY), len1 = (uint8_t)(sizeof(payload) - off1);
        ag_reasm_verdict_t v1 = ag_reasm_add(&r, 0x1234,
            (uint8_t)((1 << 4) | frags), payload + off1, len1);
        CHECK_MSG(v1 == AG_REASM_PARTIAL, "first-of-two fragment must be PARTIAL");
        // deliver fragment 0: completes.
        ag_reasm_verdict_t v0 = ag_reasm_add(&r, 0x1234,
            (uint8_t)((0 << 4) | frags), payload, BODY);
        CHECK_MSG(v0 == AG_REASM_COMPLETE, "last missing fragment must COMPLETE");
        CHECK_MSG(r.payload_len == sizeof(payload), "reassembled len %u != %u",
                  r.payload_len, (unsigned)sizeof(payload));
        CHECK_MSG(memcmp(r.payload, payload, sizeof(payload)) == 0,
                  "reassembled payload mismatch");
    }

    // (17) a single-fragment record completes immediately.
    {
        uint8_t payload[10];
        for (int i = 0; i < 10; i++) payload[i] = (uint8_t)i;
        ag_reasm_t r; ag_reasm_reset(&r);
        ag_reasm_verdict_t v = ag_reasm_add(&r, 0x55, (uint8_t)((0 << 4) | 1),
                                            payload, sizeof(payload));
        CHECK(v == AG_REASM_COMPLETE);
        CHECK(r.payload_len == sizeof(payload));
    }

    // (18) malformed fragment headers are rejected (BADFRAG) and leave no state.
    {
        ag_reasm_t r; ag_reasm_reset(&r);
        uint8_t b[4] = {1, 2, 3, 4};
        CHECK(ag_reasm_add(&r, 0x1, 0x00, b, 4) == AG_REASM_BADFRAG); // total 0
        CHECK(!r.used);
        CHECK(ag_reasm_add(&r, 0x1, (uint8_t)((2 << 4) | 2), b, 4) == AG_REASM_BADFRAG); // idx 2 of 2
        CHECK(!r.used);
        // an over-large declared total (the 4-bit wire field allows up to 15, but
        // the 31-byte buffer + 8-bit mask support far fewer) is rejected so a
        // crafted frame cannot complete early against a truncated mask.
        CHECK_MSG(ag_reasm_add(&r, 0x1, (uint8_t)((0 << 4) | 12), b, 4)
                  == AG_REASM_BADFRAG, "frag_total=12 must be rejected");
        CHECK(!r.used);
        // a fragment whose body would land past the staging buffer is rejected
        // rather than marked present-but-unstaged. With BODY=16, a (bogus) idx=2
        // of a 3-total record sits at off=32 > 31.
        CHECK_MSG(ag_reasm_add(&r, 0x1, (uint8_t)((2 << 4) | 3), b, 4)
                  == AG_REASM_BADFRAG, "out-of-range offset must be rejected");
        CHECK(!r.used);
        // NULL body is rejected.
        CHECK(ag_reasm_add(&r, 0x1, (uint8_t)((0 << 4) | 1), NULL, 4) == AG_REASM_BADFRAG);
    }

    // (18b) a fragment whose declared total disagrees with the slot's (set by the
    //       first fragment of this rec_id) is dropped — it must not complete
    //       against the wrong mask.
    {
        ag_reasm_t r; ag_reasm_reset(&r);
        uint8_t b[BODY]; memset(b, 0x5A, sizeof(b));
        // first fragment claims a 2-fragment record.
        CHECK(ag_reasm_add(&r, 0x7, (uint8_t)((0 << 4) | 2), b, BODY) == AG_REASM_PARTIAL);
        // a second fragment for the same rec_id claims a 1-fragment total: drop it
        // (otherwise full=(1<<1)-1=1 and bit 0 already set would falsely COMPLETE).
        CHECK_MSG(ag_reasm_add(&r, 0x7, (uint8_t)((0 << 4) | 1), b, 4)
                  == AG_REASM_BADFRAG, "inconsistent frag_total must be dropped");
        // the slot stays a 2-fragment in-flight record.
        CHECK(r.used && r.frag_total == 2);
    }

    // (19) duplicate fragments don't double-count: re-delivering frag 0 of a
    //      2-frag record stays PARTIAL until frag 1 finally lands.
    {
        ag_reasm_t r; ag_reasm_reset(&r);
        uint8_t b[BODY]; memset(b, 0x77, sizeof(b));
        CHECK(ag_reasm_add(&r, 0x9, (uint8_t)((0 << 4) | 2), b, BODY) == AG_REASM_PARTIAL);
        CHECK(ag_reasm_add(&r, 0x9, (uint8_t)((0 << 4) | 2), b, BODY) == AG_REASM_PARTIAL);
        CHECK(ag_reasm_add(&r, 0x9, (uint8_t)((1 << 4) | 2), b, 4) == AG_REASM_COMPLETE);
    }

    // (20) slot_for: an existing rec_id reuses its slot; a new rec_id takes a
    //      free slot; under pressure a deterministic victim is overwritten.
    {
        ag_reasm_t slots[4];
        for (int i = 0; i < 4; i++) ag_reasm_reset(&slots[i]);
        uint8_t b[4] = {1, 2, 3, 4};
        // fill all 4 slots with distinct, still-in-flight (2-frag) records.
        for (uint16_t id = 0; id < 4; id++) {
            int s = ag_reasm_slot_for(slots, 4, id);
            CHECK(ag_reasm_add(&slots[s], id, (uint8_t)((0 << 4) | 2), b, 4)
                  == AG_REASM_PARTIAL);
        }
        // re-offering an existing id returns its same slot (no new allocation).
        int again = ag_reasm_slot_for(slots, 4, 2);
        CHECK_MSG(slots[again].rec_id == 2, "existing rec_id must reuse its slot");
        // a 5th distinct id under pressure picks the deterministic victim id%cap.
        int victim = ag_reasm_slot_for(slots, 4, 100);
        CHECK_MSG(victim == (int)(100 % 4), "overwrite victim must be rec_id%%cap");
    }

    // --- wire/pool rec_id round-trip (dedup correctness) -------------------
    // The seen-set dedups on the WIRE rec_id. addr_type is part of a record's
    // identity (rec_id = hash(addr_type || orig_addr || payload)), so it must be
    // carried on the wire — otherwise the receiver's view of the identity is
    // wrong. The reassembly path stages the payload and reconstructs orig_addr
    // from its prefix; the wire also carries addr_type and the frozen rec_id.
    {
        const uint8_t addr_type = 1;
        uint8_t payload[24];
        for (int i = 0; i < 24; i++) payload[i] = (uint8_t)(0x30 + i);
        // sender's stable rec_id, frozen at first sighting over THIS payload.
        uint16_t wire = ag_pool_rec_id(addr_type, payload /*orig_addr=payload[0..6]*/,
                                       payload, sizeof(payload));

        // receiver reassembles the payload (possibly a LATER sighting of the
        // same device, whose mutable bytes have drifted) and sets addr_type +
        // orig_addr from the wire/prefix.
        uint8_t frags = ag_mesh_frag_count(sizeof(payload), BODY);
        ag_reasm_t r; ag_reasm_reset(&r);
        ag_reasm_verdict_t v = AG_REASM_PARTIAL;
        for (uint8_t f = 0; f < frags; f++) {
            uint8_t off = (uint8_t)(f * BODY);
            uint8_t len = (uint8_t)(sizeof(payload) - off);
            if (len > BODY) len = BODY;
            v = ag_reasm_add(&r, wire, (uint8_t)((f << 4) | frags),
                             payload + off, len);
        }
        CHECK(v == AG_REASM_COMPLETE);

        // When the staged payload still matches the sender's first-sighting
        // payload, a recompute reproduces the wire id...
        uint8_t orig_addr[6];
        memcpy(orig_addr, r.payload, 6);            // identity from payload prefix
        uint16_t same = ag_pool_rec_id(addr_type, orig_addr, r.payload, r.payload_len);
        CHECK_MSG(same == wire, "matched payload should recompute the wire id");

        // ...but a real beacon's MUTABLE bytes drift between sightings: the
        // sender froze rec_id at the first sighting while transmitting a LATER
        // payload, so recomputing on the receiver would mint a DIFFERENT id than
        // the seen-set deduped on. The pool must therefore TRUST the carried
        // wire rec_id (pool_insert_record's trust_rec_id path) rather than
        // recompute. This is the load-bearing dedup fix.
        uint8_t drifted[24];
        memcpy(drifted, r.payload, sizeof(drifted));
        drifted[20] ^= 0xFF;   // a mutable counter byte advanced
        uint16_t recomputed = ag_pool_rec_id(addr_type, orig_addr, drifted,
                                             sizeof(drifted));
        CHECK_MSG(recomputed != wire,
                  "payload drift must change a recompute (so recompute-on-absorb "
                  "would break dedup; the pool must trust the wire rec_id)");

        // addr_type is genuinely part of the identity (so carrying it matters).
        uint16_t wrong = ag_pool_rec_id((uint8_t)(addr_type ^ 1), orig_addr,
                                        r.payload, r.payload_len);
        CHECK_MSG(wrong != wire, "addr_type must affect rec_id");
    }

    TEST_SUMMARY();
}
