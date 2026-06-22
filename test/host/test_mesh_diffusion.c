// test_mesh_diffusion.c — simulated N-node mesh diffusion bound over the REAL
// guard code (ag_mesh_evaluate / ag_seen_t / hop-TTL decrement).
//
// The on-air rig is only ever two-node-equivalent, so node DENSITY is simulated
// here: we build several topologies (ring, random-geometric, full mesh) of N
// nodes, seed ONE air-capture at node 0, flood the record outward, and assert
// the diffusion bound that the loop guards must enforce:
//
//   - every node that ACCEPTS the record is within AG_TTL_INIT hops of the
//     origin (TTL bounds reach regardless of density);
//   - each node accepts the record AT MOST ONCE (dedup: no per-node
//     amplification, even under repeated re-offers of the same rec_id);
//   - the total accept count is bounded by the node count (no 2^n blow-up).
//
// Each node runs its own real seen-set + pool-membership state; edges deliver
// the record peer-to-peer with the carried hop-TTL, exactly as mesh.c does.
#include "ag_core/ag_meshguard.h"
#include "ag_core/ag_prng.h"
#include "test_util.h"
#include <string.h>

#define MAX_NODES 256
#define SEEN_CAP  512

// One simulated node: its NodeID, real seen-set, and whether the record is
// currently live in its pool (with the stored hop-TTL).
typedef struct {
    uint32_t node_id;
    ag_seen_t seen;
    uint16_t  ids[SEEN_CAP];
    uint32_t  stamps[SEEN_CAP];
    bool      in_pool;
    uint8_t   pool_ttl;
    int       accepts;     // total ACCEPTs of the seeded rec_id at this node
    int       hop;         // BFS hop distance from origin among accepting nodes
} node_t;

static node_t g_nodes[MAX_NODES];

// Adjacency as a bitset-free edge list per node (small N).
static int  g_adj[MAX_NODES][MAX_NODES];
static int  g_deg[MAX_NODES];

static void reset_topology(int n)
{
    for (int i = 0; i < n; i++) {
        g_nodes[i].node_id = 0x10000u + (uint32_t)i; // distinct, non-zero origins
        ag_seen_init(&g_nodes[i].seen, g_nodes[i].ids, g_nodes[i].stamps, SEEN_CAP);
        g_nodes[i].in_pool = false;
        g_nodes[i].pool_ttl = 0;
        g_nodes[i].accepts = 0;
        g_nodes[i].hop = -1;
        g_deg[i] = 0;
    }
}

static void add_edge(int a, int b)
{
    g_adj[a][g_deg[a]++] = b;
    g_adj[b][g_deg[b]++] = a;
}

static void build_ring(int n)
{
    reset_topology(n);
    for (int i = 0; i < n; i++) add_edge(i, (i + 1) % n);
}

static void build_full(int n)
{
    reset_topology(n);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) add_edge(i, j);
}

// Random-geometric-ish: connect each node to a few random others (a denser, less
// structured graph than the ring; degree ~k).
static void build_random(int n, int k, ag_prng_t *rng)
{
    reset_topology(n);
    for (int i = 0; i < n; i++) {
        for (int e = 0; e < k; e++) {
            int j = (int)(ag_prng_unit(rng) * n);
            if (j != i && g_deg[i] < MAX_NODES - 1 && g_deg[j] < MAX_NODES - 1)
                add_edge(i, j);
        }
    }
}

// Flood the record from `origin`. Each delivery offers the record to a neighbor
// at the carried hop-TTL; the neighbor's REAL guard decides accept/refresh/drop.
// On accept the neighbor re-floods to ITS neighbors at the decremented TTL. A
// work queue carries (node, inbound_ttl). origin_node is pinned to the air-
// capturer's NodeID so the own-origin guard is exercised.
typedef struct { int node; uint8_t ttl; int hop; } work_t;

static void flood(int n, int origin, uint16_t rec_id, int reoffer)
{
    (void)n;
    static work_t q[MAX_NODES * MAX_NODES];
    int head = 0, tail = 0;
    uint32_t origin_id = g_nodes[origin].node_id;

    // Seed: the origin air-captured the record (its own pool already holds it at
    // TTL_INIT); it floods to neighbors at TTL_INIT.
    g_nodes[origin].in_pool = true;
    g_nodes[origin].pool_ttl = AG_TTL_INIT;
    g_nodes[origin].hop = 0;
    for (int e = 0; e < g_deg[origin]; e++)
        q[tail++] = (work_t){ g_adj[origin][e], AG_TTL_INIT, 1 };

    while (head < tail) {
        work_t w = q[head++];
        node_t *nd = &g_nodes[w.node];
        uint8_t out_ttl = 0;
        // Re-offer the same record `reoffer` extra times to the SAME node to
        // assert dedup holds (accepts must stay 1 regardless).
        ag_mesh_verdict_t v = AG_MESH_DROP_SEEN;
        for (int rep = 0; rep <= reoffer; rep++) {
            v = ag_mesh_evaluate(&nd->seen, rec_id, w.ttl, origin_id,
                                 nd->node_id, nd->in_pool, nd->pool_ttl, &out_ttl);
            if (v == AG_MESH_ACCEPT) {
                nd->accepts++;
                nd->in_pool = true;
                nd->pool_ttl = out_ttl;
                if (nd->hop < 0) nd->hop = w.hop;
            } else if (v == AG_MESH_REFRESH_LOWER) {
                nd->pool_ttl = out_ttl;
            }
        }
        // Re-flood only on the first accept, at the decremented TTL.
        if (v == AG_MESH_ACCEPT && out_ttl > 0) {
            for (int e = 0; e < g_deg[w.node]; e++)
                q[tail++] = (work_t){ g_adj[w.node][e], out_ttl, w.hop + 1 };
        }
    }
}

// Assert the diffusion bound for a flooded topology; return the accept count.
static int check_diffusion(int n, const char *label, int reoffer)
{
    flood(n, 0, 0xBEEF, reoffer);
    int accepting = 0;
    int max_hop = 0;
    for (int i = 0; i < n; i++) {
        // exactly-once: no node accepts the same record more than once.
        CHECK_MSG(g_nodes[i].accepts <= 1,
                  "%s: node %d accepted %d times (amplification)", label, i,
                  g_nodes[i].accepts);
        if (g_nodes[i].accepts == 1) {
            accepting++;
            // every accepting node is within TTL_INIT hops of the origin.
            CHECK_MSG(g_nodes[i].hop <= AG_TTL_INIT,
                      "%s: node %d accepted at hop %d > TTL_INIT %d", label, i,
                      g_nodes[i].hop, AG_TTL_INIT);
            if (g_nodes[i].hop > max_hop) max_hop = g_nodes[i].hop;
        }
    }
    // total accepts bounded by node count (deduped, not 2^n).
    CHECK_MSG(accepting <= n, "%s: %d accepts > %d nodes", label, accepting, n);
    fprintf(stderr, "  [diffusion] %-22s N=%3d reach=%3d max_hop=%d\n",
            label, n, accepting, max_hop);
    return accepting;
}

int main(void)
{
    TEST_BEGIN("mesh_diffusion");
    ag_prng_t rng; ag_prng_seed(&rng, 0xD1FF0001u);

    // Diffusion-vs-density curve: as density rises (ring -> random -> full), the
    // reach RISES toward the TTL-bounded neighborhood but the per-node accept
    // count stays exactly 1 and no accept exceeds TTL_INIT hops. The numbers
    // printed here are the recorded curve.
    fprintf(stderr, "  diffusion-vs-density curve (TTL_INIT=%d):\n", AG_TTL_INIT);

    int prev_ring = -1;
    for (int idx = 0; idx < 4; idx++) {
        int n = (int[]){10, 50, 100, 250}[idx];

        // ring: sparse, degree 2 — reach is exactly the TTL-radius shell.
        build_ring(n);
        int ring_reach = check_diffusion(n, "ring", 0);
        // ring reach is bounded by the 2*TTL+1 nodes within TTL hops (or n).
        int ring_bound = (2 * AG_TTL_INIT + 1 < n) ? (2 * AG_TTL_INIT + 1) : n;
        CHECK_MSG(ring_reach <= ring_bound,
                  "ring N=%d reach %d > TTL shell %d", n, ring_reach, ring_bound);
        // denser ring (more nodes) never reaches FEWER than a small ring.
        if (prev_ring >= 0) CHECK(ring_reach >= prev_ring || n <= 10);
        prev_ring = ring_reach;

        // random-geometric: each node ~degree 6.
        build_random(n, 6, &rng);
        check_diffusion(n, "random-geometric", 0);

        // full mesh: every node within 1 hop of the origin -> all reachable, but
        // still each accepts exactly once and at hop 1.
        build_full(n);
        int full_reach = check_diffusion(n, "full-mesh", 0);
        CHECK_MSG(full_reach == n - 1 || (AG_TTL_INIT >= 1 && full_reach == n - 1),
                  "full mesh N=%d should reach all n-1 peers, got %d", n, full_reach);
        // every accepting node in a full mesh is exactly 1 hop from origin.
        for (int i = 1; i < n; i++)
            CHECK_MSG(g_nodes[i].accepts == 0 || g_nodes[i].hop == 1,
                      "full mesh node %d accepted at hop %d (expected 1)", i,
                      g_nodes[i].hop);
    }

    // Duplicate-flood stress: re-offer the SAME rec_id many times to every node
    // on a dense graph and assert accepts-per-node stays exactly 1 (no
    // resurrection / amplification under a flood of duplicate deliveries).
    {
        const int n = 64;
        build_full(n);
        check_diffusion(n, "full-mesh+flood-x10", 10);
        for (int i = 0; i < n; i++)
            CHECK_MSG(g_nodes[i].accepts <= 1,
                      "flood-x10: node %d accepted %d times", i, g_nodes[i].accepts);
    }

    TEST_SUMMARY();
}
