// radio_single.c — single shared-radio backend + serial scheduler
//
// Key APIs (wired up across P1/P2):
//   capture: esp_wifi_set_promiscuous, esp_wifi_set_promiscuous_filter,
//            esp_wifi_set_channel (hop 1/6/11), esp_ble_gap_set_scan_params,
//            esp_ble_gap_start_scanning
//   emit:    esp_ble_gap_set_rand_addr, esp_ble_gap_config_adv_data_raw,
//            esp_ble_gap_start_advertising (BLE);
//            esp_wifi_80211_tx (Wi-Fi, FCS-stripped, en_sys_seq=false);
//   power:   esp_ble_tx_power_set (ESP_BLE_PWR_TYPE_ADV),
//            esp_wifi_set_max_tx_power  -- set in the per-ghost pre-TX hook.
#include "radio_single.h"
#include "esp_log.h"

static const char *TAG = "radio_single";

// : BLE Bluedroid enum spans 16 levels; Wi-Fi PHY rounds to ~11.
#define RADIO_SINGLE_BLE_LEVELS  16
#define RADIO_SINGLE_WIFI_LEVELS 11

static ag_capture_cb_t s_cap_cb;
static void           *s_cap_user;

static esp_err_t rs_init(void)
{
    // TODO(P1): esp_wifi_init (WIFI_MODE_NULL) + promiscuous setup; BLE
    // controller + Bluedroid init; esp_coex preference. Start the serial
    // time-division scheduler task (one segment on air at a time).
    ESP_LOGI(TAG, "radio_single init");
    return ESP_OK;
}

static esp_err_t rs_deinit(void)
{
    ESP_LOGI(TAG, "radio_single deinit");
    return ESP_OK;
}

static esp_err_t rs_capture_start(ag_capture_cb_t cb, void *user)
{
    s_cap_cb = cb;
    s_cap_user = user;
    // TODO(P1): enable promiscuous RX cb (mgmt filter) + BLE passive scan
    // marshal both into ag_capture_t and invoke s_cap_cb in the scan segments.
    ESP_LOGI(TAG, "capture_start");
    return ESP_OK;
}

static esp_err_t rs_capture_stop(void)
{
    s_cap_cb = NULL;
    return ESP_OK;
}

static esp_err_t rs_emit(const ag_emit_t *e)
{
    // Per-ghost pre-TX power hook: apply e->tx_power_idx via
    // esp_ble_tx_power_set / esp_wifi_set_max_tx_power BEFORE transmitting, so
    // each emitted beacon gets its own RSSI despite the global power register.
    (void)e;
    // TODO(P2): BLE — set rand addr, config_adv_data_raw, start_advertising for
    // one rotate_ms slot. Wi-Fi — strip FCS, overwrite TSF (H') + seq (H)
    // esp_wifi_80211_tx(ifx, frame, len, en_sys_seq=false).
    ESP_LOGD(TAG, "emit proto=%d len=%u", e ? e->proto : -1, e ? e->frame_len : 0);
    return ESP_OK;
}

static int rs_tx_power_levels(ag_proto_t proto)
{
    return (proto == AG_PROTO_BLE) ? RADIO_SINGLE_BLE_LEVELS : RADIO_SINGLE_WIFI_LEVELS;
}

const radio_backend_t radio_single_backend = {
    .name = "radio_single",
    .init = rs_init,
    .deinit = rs_deinit,
    .capture_start = rs_capture_start,
    .capture_stop = rs_capture_stop,
    .emit = rs_emit,
    .tx_power_levels = rs_tx_power_levels,
    .concurrent_capture_replay = false, // single radio: strict serial
};

// Backend selection lives with the concrete backend to keep the radio_backend
// component a pure interface (acyclic dependency graph). A future radio_dual
// backend would add a compile-time/menuconfig choice here.
const radio_backend_t *radio_backend_get(void)
{
    return &radio_single_backend;
}
