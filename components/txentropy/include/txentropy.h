// txentropy.h — per-ghost TX-power RSSI entropy
//
// Each ghost carries a continuous virtual power p_virt driven by a
// mean-reverting Gaussian random walk:
//   p_virt += N(0,sigma_step) - k*(p_virt - p_center)  [+ rare shadow excursion]
// quantized to the nearest hardware ladder level at emission. Because the
// single-radio backend emits ONE ghost per round-robin slot, the level is set
// in a per-ghost pre-TX hook — yielding effective per-beacon RSSI control even
// though the hardware register is global per radio.
// Variance is ambient-adaptive and 0x0A AD bytes are kept consistent.
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "radio_backend.h" // ag_proto_t
#include "pool.h"          // ag_beacon_record_t

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t txentropy_init(void);

// Advance a ghost's p_virt by elapsed time (called on T_walk) and
// return the backend-normalized tx_power_idx to stamp onto ag_emit_t for this
// ghost's next slot. Quantizes p_virt to the nearest of
// radio_backend->tx_power_levels(proto) hardware steps.
int8_t txentropy_level_for_ghost(ag_beacon_record_t *ghost, ag_proto_t proto);

// Refresh the ambient RSSI-variance estimate from currently-observed real
// sources; falls back to the 6-10 dB pedestrian prior when sparse.
void txentropy_update_ambient(void);

#ifdef __cplusplus
}
#endif
