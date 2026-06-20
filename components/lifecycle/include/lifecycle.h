// lifecycle.h — ghost rotation/departure successor logic
//
// Decides, per ghost, between the DEPARTURE model (fade-out: uncorrelated
// disappearance, default) and the ROTATION model (correlated death+birth: spawn
// a successor with a NEW address but a CONTINUOUS RSSI trajectory, for sources
// that behaved like rotating phones) — Also handles spawn
// de-correlation (spawn jitter) and operator-own-device exclusion (never
// replay co-located high-RSSI gear).
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lifecycle_init(void);

// Periodic tick: promote departing records, retire expired ghosts, and spawn
// rotation successors where the source behaved phone-like.
void lifecycle_tick(void);

#ifdef __cplusplus
}
#endif
