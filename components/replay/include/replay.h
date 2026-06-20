// replay.h — round-robin ghost emission (replay_ble / replay_wifi)
//
// Selects up to max_concurrent_ghosts REPLAY_ELIGIBLE records and emits them
// one-at-a-time on a rotate_ms cadence (single legacy BLE advertising instance
// effective per-ghost interval floor ~= N*rotate_ms). For BLE:
// regenerates a static-random/NRPA address, copies raw AdvData verbatim, and
// matches the source cadence +-interval_jitter_pct (never exactly equal).
// For Wi-Fi (gated off by default): reconstructs the beacon, strips FCS, and
// PER-TX overwrites the 8-byte TSF (monotonic 64-bit, H') and 2-byte
// sequence-control (synthetic 12-bit seq, H) immediately before each
// esp_wifi_80211_tx. TX power per ghost comes from txentropy.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t replay_init(void);

// Advance the round-robin by one slot: pick the next ghost, fetch its txentropy
// level, build the ag_emit_t, and hand it to radio_backend->emit. Called every
// rotate_ms from the scheduler.
void replay_tick(void);

#ifdef __cplusplus
}
#endif
