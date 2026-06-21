// ag_txad.c — TX Power Level (0x0A) AD field policy (host-tested in test_txad.c).
#include "ag_core/ag_txad.h"
#include <string.h>

// Walk the AD list to find a TX Power Level field. Sets *field to the index of
// its length byte and returns true; returns false when none is present or the
// list is malformed. A well-formed 0x0A field is length=2 (type + 1 dBm byte).
static int find_tx_power(const uint8_t *ad, uint8_t len)
{
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t flen = ad[i];
        if (flen == 0) break;                 // padding / end of list
        if (i + 1 + flen > len) break;        // truncated field
        if (ad[i + 1] == AG_AD_TYPE_TX_POWER) return (int)i;
        i = (uint8_t)(i + 1 + flen);
    }
    return -1;
}

uint8_t ag_txad_apply(uint8_t *ad, uint8_t len, uint8_t policy, int8_t emit_dbm)
{
    if (ad == NULL || len < 3) return len;

    int at = find_tx_power(ad, len);

    if (policy == AG_TXAD_POLICY_STRIP) {
        if (at < 0) return len;
        uint8_t flen = ad[at];
        uint8_t whole = (uint8_t)(flen + 1);  // length byte + payload
        // Close the gap left by the removed field.
        uint8_t tail = (uint8_t)(len - (at + whole));
        if (tail > 0) memmove(&ad[at], &ad[at + whole], tail);
        return (uint8_t)(len - whole);
    }

    // PER_CLASS and REWRITE both rewrite an existing field to the emitted power;
    // neither synthesizes one where the source carried none.
    if (at >= 0 && ad[at] == 2) {
        ad[at + 2] = (uint8_t)emit_dbm;
    }
    return len;
}
