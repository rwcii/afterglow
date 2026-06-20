// radio_single.h — single shared-radio backend ( radio_single/radio_sched)
//
// Implements the radio_backend_t vtable against the ESP32-S3's single shared
// 2.4 GHz radio. Owns the strict serial time-division scheduler: in any cycle
// only ONE of {Wi-Fi sniff dwell, BLE scan window, replay-adv window, mesh
// burst} is on air (concurrent sniffer+BLE is C1-unstable). The
// per-ghost pre-TX power hook lives in emit. A future radio_dual
// backend relaxes the mutual exclusion.
#pragma once

#include "radio_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

// The single-radio backend instance, selected by radio_backend_get.
extern const radio_backend_t radio_single_backend;

#ifdef __cplusplus
}
#endif
