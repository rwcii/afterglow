// pool.h — fixed-slot PSRAM beacon pool 
//
// Single preallocated fixed-slot slab in PSRAM (heap_caps_malloc with
// MALLOC_CAP_SPIRAM). The capture path copies frames to INTERNAL RAM first
// (DMA/ISR cannot touch PSRAM) and the pool task admits them here.
// Eviction combines capacity (score-ranked sigmoid), per-record log-normal TTL
// and a Weibull-shaped stochastic dropout — one canonical model.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "radio_backend.h" // ag_capture_t, ag_proto_t

#ifdef __cplusplus
extern "C" {
#endif

// Record class. Address byte is NEVER used to classify.
typedef enum {
    AG_CLASS_TENTATIVE = 0,     // first-seen random addr (prior 0.30)
    AG_CLASS_STATIC_RANDOM_BLE, // 0b11 — cloneable
    AG_CLASS_NRPA_BLE,          // 0b00 — cloneable
    AG_CLASS_RPA_BLE,           // 0b01 — uncloneable, self-rotating
    AG_CLASS_PUBLIC_BLE,        // uncloneable
    AG_CLASS_WIFI,              // beacon / BSSID
} ag_beacon_class_t;

// Record flags ( `flags`).
enum {
    AG_FLAG_REPLAY_ELIGIBLE = 1 << 0,
    AG_FLAG_REPLAY_LOSSY    = 1 << 1,
    AG_FLAG_DEPARTING       = 1 << 2,
};

// Pool record — mirrors `afterglow_record_t` common header.
typedef struct {
    uint8_t  proto;             // ag_proto_t
    uint8_t  cls;               // ag_beacon_class_t
    uint8_t  flags;
    uint8_t  orig_addr[6];      // original observed AdvA/BSSID (feeds rec_id, NOT replay addr)
    uint8_t  addr_type;         // observed (informational)
    uint16_t rec_id;            // stable per-record id (hash of addr_type||orig_addr||first payload); fixed across merges/relays
    uint32_t origin_node;       // full 32-bit NodeID of first air-capturer
    uint16_t interval_q;        // estimated cadence (0.625 ms / TU units)
    int8_t   rssi_last, rssi_ewma;
    uint8_t  rssi_dev_ewma;     // EWMA of |sample - prior rssi_ewma|, dB magnitude:
                                // this source's TEMPORAL RSSI variability,
                                // distinct from cross-source spatial spread.
    uint8_t  channel, obs_count, hop_ttl;
    uint8_t  adv_kind;          // ag_adv_kind_t — observed PDU behavior (broadcast/scannable/connectable)
    uint16_t wifi_seq;          // per-ghost synthetic 802.11 seq state
    uint32_t first_seen_ms, last_seen_ms;
    float    base_ttl_s, ttl_cap_s;
    uint32_t replay_deadline_ms;
    uint32_t next_rotate_ms;    // absolute ms of the next address rotation for a
                                // ROTATING (NRPA-class) ghost; 0 = unscheduled /
                                // STATIONARY_HOLD. Lifecycle-owned.
    float    p_virt, p_center;  // RSSI walk state
    uint8_t  payload_len;
    uint8_t  payload[31];       // BLE <=31 B; Wi-Fi beacon templates are truncated to fit (Wi-Fi replay ships off)
} ag_beacon_record_t;

// Allocate the PSRAM slab sized to cfg->store_cap. Call once at boot.
esp_err_t pool_init(void);

// Admit/merge a captured beacon (admit on FIRST sighting). Copies
// what it keeps; cap->frame is only valid for the call.
esp_err_t pool_admit(const ag_capture_t *cap);

// Run one eviction sweep (capacity + TTL + Weibull dropout).
void pool_evict_sweep(void);

// Index of the record touched by the most recent successful pool_admit (-1 if
// none / slab was full). Valid until the next admit or sweep.
int pool_last_admitted(void);

// This node's 32-bit per-boot NodeID (origin pinning / mesh self-id).
uint32_t pool_node_id(void);

// Insert a fully-prepared record (e.g. a rotation successor built by lifecycle,
// or a record absorbed over the mesh). Recomputes rec_id from the record's
// identity+payload. Returns the slot index, or -1 if the slab is full.
int pool_insert_record(const ag_beacon_record_t *rec);

// Current number of live records.
uint16_t pool_count(void);

// Borrow a record by index for replay/lifecycle/mesh iteration (NULL if empty
// slot). Caller must not retain past the next sweep.
const ag_beacon_record_t *pool_record_at(uint16_t idx);

// Mutable borrow (classifier/lifecycle update record state in place). NULL if
// out of range. Caller must not retain past the next sweep.
ag_beacon_record_t *pool_record_mut(uint16_t idx);
uint16_t pool_capacity(void);

#ifdef __cplusplus
}
#endif
