// test_txad.c — TX Power Level (0x0A) AD field policy.
#include "ag_core/ag_txad.h"
#include "test_util.h"
#include <string.h>

// AdvData with: Flags (02 01 06), TX Power (02 0A <dbm>), Complete Name (03 09 41 42).
static uint8_t mk_ad_with_txpower(uint8_t *buf, int8_t dbm)
{
    uint8_t i = 0;
    buf[i++] = 0x02; buf[i++] = 0x01; buf[i++] = 0x06;                 // Flags
    buf[i++] = 0x02; buf[i++] = 0x0A; buf[i++] = (uint8_t)dbm;         // TX Power
    buf[i++] = 0x03; buf[i++] = 0x09; buf[i++] = 0x41; buf[i++] = 0x42; // Name "AB"
    return i;
}

// AdvData with NO TX Power field: Flags + Name only.
static uint8_t mk_ad_no_txpower(uint8_t *buf)
{
    uint8_t i = 0;
    buf[i++] = 0x02; buf[i++] = 0x01; buf[i++] = 0x06;
    buf[i++] = 0x03; buf[i++] = 0x09; buf[i++] = 0x41; buf[i++] = 0x42;
    return i;
}

// Find the dBm byte of a 0x0A field, or return INT8 sentinel 127 if absent.
static int8_t read_txpower(const uint8_t *ad, uint8_t len)
{
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t flen = ad[i];
        if (flen == 0) break;
        if (i + 1 + flen > len) break;
        if (ad[i + 1] == 0x0A) return (int8_t)ad[i + 2];
        i = (uint8_t)(i + 1 + flen);
    }
    return 127;
}

int main(void)
{
    TEST_BEGIN("txad");
    uint8_t buf[31];

    // --- REWRITE: an existing 0x0A is set to the emitted power.
    {
        uint8_t len = mk_ad_with_txpower(buf, -59);
        uint8_t out = ag_txad_apply(buf, len, AG_TXAD_POLICY_REWRITE, -9);
        CHECK_MSG(out == len, "REWRITE changed AdvData length (%u != %u)", out, len);
        CHECK_MSG(read_txpower(buf, out) == -9, "REWRITE did not set the dBm byte");
    }

    // --- PER_CLASS: present field is rewritten...
    {
        uint8_t len = mk_ad_with_txpower(buf, 12);
        uint8_t out = ag_txad_apply(buf, len, AG_TXAD_POLICY_PER_CLASS, -3);
        CHECK(out == len);
        CHECK_MSG(read_txpower(buf, out) == -3, "PER_CLASS did not rewrite present field");
    }

    // --- PER_CLASS: ...but no field is synthesized when the source omits it.
    {
        uint8_t len = mk_ad_no_txpower(buf);
        uint8_t out = ag_txad_apply(buf, len, AG_TXAD_POLICY_PER_CLASS, -3);
        CHECK_MSG(out == len, "PER_CLASS resized an AdvData with no TX Power field");
        CHECK_MSG(read_txpower(buf, out) == 127, "PER_CLASS synthesized a TX Power field");
    }

    // --- STRIP: the field is removed and the trailing structures shift up.
    {
        uint8_t len = mk_ad_with_txpower(buf, -40);
        uint8_t out = ag_txad_apply(buf, len, AG_TXAD_POLICY_STRIP, 0);
        CHECK_MSG(out == (uint8_t)(len - 3), "STRIP removed the wrong byte count (%u)", out);
        CHECK_MSG(read_txpower(buf, out) == 127, "STRIP left a TX Power field behind");
        // The Flags field is untouched and the Name field survives intact.
        CHECK(buf[0] == 0x02 && buf[1] == 0x01 && buf[2] == 0x06);
        CHECK(buf[3] == 0x03 && buf[4] == 0x09 && buf[5] == 0x41 && buf[6] == 0x42);
    }

    // --- STRIP with no field present: unchanged.
    {
        uint8_t len = mk_ad_no_txpower(buf);
        uint8_t out = ag_txad_apply(buf, len, AG_TXAD_POLICY_STRIP, 0);
        CHECK(out == len);
    }

    // --- TX Power as the LAST field: STRIP closes it with no tail to move.
    {
        uint8_t i = 0;
        buf[i++] = 0x02; buf[i++] = 0x01; buf[i++] = 0x06;        // Flags
        buf[i++] = 0x02; buf[i++] = 0x0A; buf[i++] = (uint8_t)-7; // TX Power (last)
        uint8_t out = ag_txad_apply(buf, i, AG_TXAD_POLICY_STRIP, 0);
        CHECK(out == 3);
        CHECK(read_txpower(buf, out) == 127);
    }

    // --- guards: NULL / too-short buffers are returned unchanged.
    {
        CHECK(ag_txad_apply(NULL, 10, AG_TXAD_POLICY_STRIP, 0) == 10);
        uint8_t two[2] = {0x01, 0x0A};
        CHECK(ag_txad_apply(two, 2, AG_TXAD_POLICY_REWRITE, -9) == 2);
    }

    TEST_SUMMARY();
}
