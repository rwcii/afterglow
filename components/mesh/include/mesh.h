// mesh.h — connectionless BLE gossip absorption
//
// Leaderless device-to-device pool diffusion. Ships DISABLED (config
// mesh_enabled=false). On contact, two nodes exchange a recency-weighted random
// subset (~15% of live pool, capped at max_records_per_contact=16) over
// connectionless BLE adv/scan bursts. Loops/amplification are bounded by:
//  - hop TTL (ttl_init=3), decremented per hop;
//  - 32-bit per-boot NodeID + origin pinning (never absorb own-origin);
//  - an LRU seen-set (4096) so a record is never resurrected within its horizon.
// Net: one air-captured beacon -> at most one ghost per node within TTL hops.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mesh_init(void);

// Periodic tick: emit HELLO heartbeats, service the contact table (cooldown)
// and run fragmented DATA transfer/reassembly with dedup/TTL/origin guards.
// No-op while mesh_enabled=false.
void mesh_tick(void);

#ifdef __cplusplus
}
#endif
