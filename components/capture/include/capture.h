// capture.h — capture glue ( capture_wifi/capture_ble)
//
// Registers the radio_backend capture callback, copies each observed frame out
// of the backend-owned buffer into internal RAM, and feeds the pool + classifier.
// The actual radio time-slicing lives in the radio_single backend
// this module is protocol-agnostic glue.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Begin capturing via the active radio backend. Idempotent.
esp_err_t capture_start(void);

// Stop capturing.
esp_err_t capture_stop(void);

#ifdef __cplusplus
}
#endif
