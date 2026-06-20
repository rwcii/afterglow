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
    uint16_t rec_id;            // hash(addr_type||orig_addr||payload) — stable across relays
    uint32_t origin_node;       // full 32-bit NodeID of first air-capturer
    uint16_t interval_q;        // estimated cadence (0.625 ms / TU units)
    int8_t   rssi_last, rssi_ewma;
    uint8_t  channel, obs_count, hop_ttl;
    uint16_t wifi_seq;          // per-ghost synthetic 802.11 seq state
    uint32_t first_seen_ms, last_seen_ms;
    float    base_ttl_s, ttl_cap_s;
    uint32_t replay_deadline_ms;
    float    p_virt, p_center;  // RSSI walk state
    uint8_t  payload_len;
    uint8_t  payload[31];       // BLE <=31 B; Wi-Fi templates use a separate tier (TODO P2)
} ag_beacon_record_t;

// Allocate the PSRAM slab sized to cfg->store_cap. Call once at boot.
esp_err_t pool_init(void);

// Admit/merge a captured beacon (admit on FIRST sighting). Copies
// what it keeps; cap->frame is only valid for the call.
esp_err_t pool_admit(const ag_capture_t *cap);

// Run one eviction sweep (capacity + TTL + Weibull dropout).
void pool_evict_sweep(void);

// Current number of live records.
uint16_t pool_count(void);

// Borrow a record by index for replay/lifecycle/mesh iteration (NULL if empty
// slot). Caller must not retain past the next sweep.
const ag_beacon_record_t *pool_record_at(uint16_t idx);
uint16_t pool_capacity(void);

#ifdef __cplusplus
}
#endif
