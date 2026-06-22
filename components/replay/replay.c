// replay.c — round-robin ghost emitter
#include "replay.h"
#include "radio_backend.h"
#include "pool.h"
#include "classifier.h"
#include "txentropy.h"
#include "afterglow_config.h"
#include "ag_core/ag_txad.h"
#include "entropy.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "replay";

static afterglow_config_t s_cfg;
static uint16_t s_rr_cursor;       // round-robin position over the pool
static uint64_t s_wifi_seq_origin; // monotonic TSF base for synthesized beacons

esp_err_t replay_init(void)
{
    afterglow_config_load(&s_cfg);
    s_rr_cursor = 0;
    s_wifi_seq_origin = (uint64_t)esp_timer_get_time();
    ESP_LOGI(TAG, "replay init (ble=%d wifi=%d ghosts=%u rotate=%ums)",
             s_cfg.ble_enabled, s_cfg.wifi_beacons_enabled,
             s_cfg.max_concurrent_ghosts, s_cfg.rotate_ms);
    return ESP_OK;
}

// Interval for this ghost: source cadence ± jitter, NEVER pinned equal. Returns
// milliseconds. interval_q is in 0.625 ms TU units.
static uint32_t ghost_interval_ms(const ag_beacon_record_t *r)
{
    uint32_t base_ms = r->interval_q ? (r->interval_q * 5u) / 8u : 1000u;
    int pct = s_cfg.interval_jitter_pct ? s_cfg.interval_jitter_pct : 3;
    float frac = ag_rand_uniform(-(float)pct / 100.0f, (float)pct / 100.0f);
    int32_t jittered = (int32_t)base_ms + (int32_t)(base_ms * frac);
    if (jittered < 20) jittered = 20;
    return (uint32_t)jittered;
}

// Overwrite the 8-byte TSF (offset 24) and 2-byte sequence-control (offset 22)
// of a Wi-Fi beacon template immediately before TX, so neither is a frozen
// captured value. Template is MAC header + body; en_sys_seq=false leaves these
// fields for us to write.
static void wifi_stamp(uint8_t *frame, uint16_t len, ag_beacon_record_t *r)
{
    if (len < 36) return;
    uint64_t tsf = (uint64_t)esp_timer_get_time() - s_wifi_seq_origin;
    memcpy(frame + 24, &tsf, 8);                  // beacon TSF field
    r->wifi_seq = (uint16_t)((r->wifi_seq + 1) & 0x0FFF);
    uint16_t seqctl = (uint16_t)(r->wifi_seq << 4); // frag subfield = 0
    memcpy(frame + 22, &seqctl, 2);               // sequence-control field
}

void replay_tick(void)
{
    if (!s_cfg.ble_enabled && !s_cfg.wifi_beacons_enabled) return;
    uint16_t n = pool_count();
    if (n == 0) return;

    // Advance the round-robin to the next replay-eligible record.
    for (uint16_t scanned = 0; scanned < n; scanned++) {
        uint16_t idx = (uint16_t)((s_rr_cursor + scanned) % n);
        ag_beacon_record_t *r = pool_record_mut(idx);
        if (!r || !classifier_replay_eligible(r)) continue;

        bool is_wifi = (r->cls == AG_CLASS_WIFI);
        if (is_wifi && !s_cfg.wifi_beacons_enabled) continue;
        if (!is_wifi && !s_cfg.ble_enabled) continue;

        ag_proto_t proto = is_wifi ? AG_PROTO_WIFI : AG_PROTO_BLE;

        // Working copy of the payload. For BLE the stored payload is
        // AdvA(6) || AdvData: the address is handed to the backend separately and
        // only the AdvData is the emitted frame. For Wi-Fi the whole template is
        // the frame and gets TSF/seq stamped here.
        static uint8_t frame[192];
        uint16_t flen = r->payload_len > sizeof(frame) ? sizeof(frame) : r->payload_len;
        memcpy(frame, r->payload, flen);

        int8_t tx_idx = txentropy_level_for_ghost(r, proto);
#ifdef AG_ONAIR_TEST
        // On-air rig only: replace the slow walk value with a deterministic ramp
        // across the full BLE ladder, so a second-board observer can measure
        // radiated RSSI against the commanded index over the whole range. Each
        // BLE slot advances one step; production builds carry none of this.
        if (proto == AG_PROTO_BLE) {
            static uint8_t s_sweep_idx;
            tx_idx = (int8_t)(s_sweep_idx % radio_backend_get()->tx_power_levels(proto));
            s_sweep_idx++;
        }
#endif
        ag_emit_t e = {
            .proto = proto,
            .channel = r->channel,
            .tx_power_idx = tx_idx,
            .interval_ms = ghost_interval_ms(r),
        };

        if (is_wifi) {
            wifi_stamp(frame, flen, r);
            e.frame = frame;
            e.frame_len = flen;
        } else {
            // Split AdvA(6) from AdvData; emit the address via e.addr and only
            // the AdvData bytes as the frame. A record short of a full address is
            // skipped (it cannot be re-emitted with a matching address).
            if (flen < 6) continue;
            uint8_t *ad = frame + 6;
            uint8_t ad_len = (uint8_t)(flen - 6);
            // Keep the TX Power Level (0x0A) AD field consistent with the power
            // actually radiated this slot: adv_apply_power applies tx_power_idx
            // as the esp_power_level_t step, whose dBm is -24 + 3*idx.
            int8_t emit_dbm = (int8_t)(-24 + 3 * (int)e.tx_power_idx);
            ad_len = ag_txad_apply(ad, ad_len, (uint8_t)s_cfg.txpower_ad_policy,
                                   emit_dbm);
            e.addr = frame;
            e.addr_type = r->addr_type;
            e.frame = ad;
            e.frame_len = ad_len;
        }
        radio_backend_get()->emit(&e);

        s_rr_cursor = (uint16_t)((idx + 1) % n);
        return;
    }
    // No eligible record this tick.
}
