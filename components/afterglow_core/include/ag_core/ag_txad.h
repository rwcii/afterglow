// ag_txad.h — TX Power Level (0x0A) AD field policy (portable).
//
// BLE AdvData may carry a TX Power Level AD structure (length=2, type=0x0A,
// one signed dBm byte). A scanner uses it with the measured RSSI to estimate
// path loss, so a re-emitted advertisement must not carry a value that
// disagrees with the power actually radiated. This applies a policy to the AD
// list in place before emission.
//
// The policy values mirror ag_txad_policy_t in afterglow_config.h (kept as a
// plain uint8_t here so ag_core carries no component dependency):
//   0 PER_CLASS — rewrite a 0x0A field that is present (keep it consistent with
//                 the emitted power) but never synthesize one where the source
//                 carried none.
//   1 STRIP     — remove any 0x0A field from the AD list.
//   2 REWRITE   — if a 0x0A field is present, set its dBm byte to emit_dbm;
//                 leave the AD list otherwise unchanged.
//
// Operates on the AdvData only (the AdvA is handled separately). Pure: no ESP
// deps; never inspects or moves the address.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// AD type for the TX Power Level structure.
#define AG_AD_TYPE_TX_POWER 0x0A

// Policy values (mirror ag_txad_policy_t).
enum {
    AG_TXAD_POLICY_PER_CLASS = 0,
    AG_TXAD_POLICY_STRIP     = 1,
    AG_TXAD_POLICY_REWRITE   = 2,
};

// Apply the policy to the AdvData buffer `ad` of length `len` in place, using
// emit_dbm as the value to write under REWRITE / PER_CLASS. Returns the new
// AdvData length (<= len; shorter only when STRIP removes a field). A NULL or
// too-short buffer is returned unchanged.
uint8_t ag_txad_apply(uint8_t *ad, uint8_t len, uint8_t policy, int8_t emit_dbm);

#ifdef __cplusplus
}
#endif
