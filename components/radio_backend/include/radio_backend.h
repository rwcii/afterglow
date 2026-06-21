// radio_backend.h — Afterglow radio HAL
//
// Abstracts capture / replay / tx-power operations from the underlying radio
// hardware. The default implementation (radio_single) drives the ESP32-S3's
// single shared 2.4 GHz radio with strict serial time-division.
// A future radio_dual backend (companion nRF52840/ESP32) can be
// dropped in without touching pool / replay / mesh logic.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "ag_core/ag_eligible.h" // ag_adv_kind_t — observed PDU behavior

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AG_PROTO_BLE = 0,
    AG_PROTO_WIFI = 1,
} ag_proto_t;

// One observed/captured beacon as surfaced by a backend's capture callback.
// Raw frame bytes are owned by the backend for the duration of the callback
// only; the pool must copy what it keeps (copy-to-internal-RAM).
typedef struct {
    ag_proto_t proto;
    int8_t     rssi;
    uint8_t    channel;       // Wi-Fi channel; 37/38/39 mapped for BLE adv
    uint64_t   ts_us;         // local capture timestamp (esp_timer)
    const uint8_t *frame;     // raw frame / adv payload
    uint16_t   frame_len;
    // Observed PDU behavior, read from the adv-report event type (BLE) or the
    // mgmt subtype (Wi-Fi). AG_ADV_UNKNOWN if the backend could not determine it;
    // the eligibility gate fails closed on anything that is not broadcast-only.
    ag_adv_kind_t adv_kind;
} ag_capture_t;

typedef void (*ag_capture_cb_t)(const ag_capture_t *cap, void *user);

// A single ghost emission request, issued one-at-a-time on the single-radio
// backend (the per-ghost pre-TX hook sets tx power immediately before this
// call). tx_power_idx is a backend-normalized ladder index; the backend
// maps it to esp_ble_tx_power_set / esp_wifi_set_max_tx_power.
typedef struct {
    ag_proto_t proto;
    const uint8_t *frame;     // BLE: raw AdvData (<=31B); Wi-Fi: beacon template
    uint16_t   frame_len;
    uint8_t    channel;
    int8_t     tx_power_idx;  // normalized; see radio_backend_tx_power_levels
    uint32_t   interval_ms;   // target advertising/beacon interval
    // High-priority emissions (mesh HELLO/DATA) get guaranteed adv slots ahead
    // of replay ghosts, so chatter can't starve them off the air. Replay leaves
    // this 0 (default); mesh sets it true.
    bool       priority;
} ag_emit_t;

// The backend interface. Implementations populate this vtable.
typedef struct {
    const char *name;

    esp_err_t (*init)(void);
    esp_err_t (*deinit)(void);

    // Capture: register a callback invoked per observed beacon. The single-radio
    // backend time-slices Wi-Fi promiscuous + BLE passive scan.
    esp_err_t (*capture_start)(ag_capture_cb_t cb, void *user);
    esp_err_t (*capture_stop)(void);

    // Replay: emit one ghost. On the single-radio backend this is serialized
    // through the round-robin scheduler.
    esp_err_t (*emit)(const ag_emit_t *e);

    // TX-power ladder introspection (BLE 16 levels / Wi-Fi ~11).
    // Returns the number of discrete hardware levels for proto.
    int (*tx_power_levels)(ag_proto_t proto);

    // True if capture and replay can run concurrently (dual-radio).
    bool concurrent_capture_replay;
} radio_backend_t;

// Returns the active backend (default: radio_single). Set via menuconfig later.
const radio_backend_t *radio_backend_get(void);

#ifdef __cplusplus
}
#endif
