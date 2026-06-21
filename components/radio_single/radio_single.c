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
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "radio_single";

// BLE Bluedroid enum spans 16 levels; Wi-Fi PHY rounds to ~11 effective steps.
#define RADIO_SINGLE_BLE_LEVELS  16
#define RADIO_SINGLE_WIFI_LEVELS 11

static ag_capture_cb_t s_cap_cb;
static void           *s_cap_user;
static bool            s_capturing;
static TaskHandle_t    s_sched_task;
static const uint8_t   s_wifi_channels[3] = {1, 6, 11};
static uint8_t         s_wifi_ch_idx;

// --- Wi-Fi promiscuous RX: surface management beacons to the capture cb -----
static void wifi_promisc_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_capturing || !s_cap_cb || type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    // 802.11 frame control: type 0 (mgmt) subtype 8 (beacon) → frame[0]==0x80.
    if (pkt->rx_ctrl.sig_len < 24 || pkt->payload[0] != 0x80) return;
    ag_capture_t cap = {
        .proto = AG_PROTO_WIFI,
        .rssi = (int8_t)pkt->rx_ctrl.rssi,
        .channel = (uint8_t)pkt->rx_ctrl.channel,
        .ts_us = (uint64_t)esp_timer_get_time(),
        .frame = pkt->payload,
        // sig_len includes the 4-byte FCS; strip it (templates store body only).
        .frame_len = (uint16_t)(pkt->rx_ctrl.sig_len - 4),
    };
    s_cap_cb(&cap, s_cap_user);
}

// --- BLE passive scan: surface legacy advertisements to the capture cb ------
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
    struct ble_scan_result_evt_param *r = &p->scan_rst;
    if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;
    if (!s_capturing || !s_cap_cb) return;
    // Marshal AdvA(6) + AdvData so the pool's identity extraction reads AdvA
    // from frame[0..5], matching pool.c extract_identity.
    static uint8_t buf[6 + 31];
    uint8_t adv_len = r->adv_data_len > 31 ? 31 : r->adv_data_len;
    memcpy(buf, r->bda, 6);
    memcpy(buf + 6, r->ble_adv, adv_len);
    ag_capture_t cap = {
        .proto = AG_PROTO_BLE,
        .rssi = (int8_t)r->rssi,
        .channel = 37,                 // legacy adv primary set; informational
        .ts_us = (uint64_t)esp_timer_get_time(),
        .frame = buf,
        .frame_len = (uint16_t)(6 + adv_len),
    };
    s_cap_cb(&cap, s_cap_user);
}

// --- Serial time-division scheduler ----------------------------------------
// One segment on air at a time (concurrent sniffer+BLE is unstable). Each cycle
// runs a BLE scan window, then hops the Wi-Fi channel for a sniff dwell. Replay
// emits in their own slots via rs_emit, called from the replay task between
// cycles; the scheduler simply yields the radio for capture segments.
static void scheduler_task(void *arg)
{
    (void)arg;
    // The scanner's random address + scan params are configured asynchronously
    // from rs_init via the GAP event chain (set_rand_addr ->
    // SET_STATIC_RAND_ADDR_COMPLETE -> set_scan_params). Give that a moment to
    // settle before the first scan window.
    vTaskDelay(pdMS_TO_TICKS(200));

    while (s_capturing) {
        // BLE scan segment.
        esp_ble_gap_start_scanning(0); // 0 = until stopped
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_ble_gap_stop_scanning();

        // Wi-Fi sniff segment: hop to the next channel, dwell.
        s_wifi_ch_idx = (uint8_t)((s_wifi_ch_idx + 1) % 3);
        esp_wifi_set_channel(s_wifi_channels[s_wifi_ch_idx], WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    s_sched_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t rs_init(void)
{
    // Wi-Fi in NULL mode (no association) for promiscuous capture + 80211_tx.
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promisc_rx));

    // BLE controller + Bluedroid, scan-only/advertising.
    esp_bt_controller_config_t bcfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bcfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap_cb));

    // Passive scanner: it never transmits, so it uses the public device address
    // (no random-address registration dance). Scan params can be set directly.
    static esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,   // 50 ms
        .scan_window = 0x30,     // 30 ms
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));

    // Wi-Fi + BLE software coexistence is enabled via Kconfig
    // (CONFIG_ESP_COEX_SW_COEXIST_ENABLE in sdkconfig.defaults); the two stacks
    // then time-share the single radio automatically once both are started.

    ESP_LOGI(TAG, "radio_single init (wifi NULL + ble scan)");
    return ESP_OK;
}

static esp_err_t rs_deinit(void)
{
    s_capturing = false;
    esp_wifi_set_promiscuous(false);
    esp_ble_gap_stop_scanning();
    ESP_LOGI(TAG, "radio_single deinit");
    return ESP_OK;
}

static esp_err_t rs_capture_start(ag_capture_cb_t cb, void *user)
{
    s_cap_cb = cb;
    s_cap_user = user;
    s_capturing = true;
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    if (!s_sched_task) {
        xTaskCreate(scheduler_task, "ag_sched", 4096, NULL, 5, &s_sched_task);
    }
    ESP_LOGI(TAG, "capture_start");
    return ESP_OK;
}

static esp_err_t rs_capture_stop(void)
{
    s_capturing = false;
    s_cap_cb = NULL;
    esp_wifi_set_promiscuous(false);
    esp_ble_gap_stop_scanning();
    return ESP_OK;
}

// Map a normalized ladder index to the per-proto hardware power setting and
// apply it (the per-ghost pre-TX hook), so each emitted beacon owns its RSSI
// despite the global power register.
static void apply_tx_power(const ag_emit_t *e)
{
    if (e->proto == AG_PROTO_BLE) {
        // Bluedroid ADV power enum spans the discrete levels up to ESP_PWR_LVL_P21.
        esp_power_level_t lvl = (esp_power_level_t)e->tx_power_idx;
        if (lvl < ESP_PWR_LVL_N24) lvl = ESP_PWR_LVL_N24;
        if (lvl > ESP_PWR_LVL_P21) lvl = ESP_PWR_LVL_P21;
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, lvl);
    } else {
        // Wi-Fi max-tx-power is in 0.25 dBm units; idx is a normalized step.
        int8_t q = (int8_t)(e->tx_power_idx);
        esp_wifi_set_max_tx_power(q);
    }
}

static esp_err_t rs_emit(const ag_emit_t *e)
{
    if (!e || !e->frame || e->frame_len == 0) return ESP_ERR_INVALID_ARG;
    apply_tx_power(e); // per-ghost pre-TX power hook

    if (e->proto == AG_PROTO_BLE) {
        // Regenerate a random adv address for this ghost slot, set raw AdvData,
        // and advertise non-connectable for one rotate window.
        static esp_ble_adv_params_t adv = {
            .adv_int_min = 0xA0, .adv_int_max = 0xC0,
            .adv_type = ADV_TYPE_NONCONN_IND,
            .own_addr_type = BLE_ADDR_TYPE_RANDOM,
            .channel_map = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        };
        // Map interval_ms to the 0.625ms adv unit, ± a jitter the caller folded.
        uint16_t unit = (uint16_t)((e->interval_ms * 8) / 5);
        adv.adv_int_min = unit;
        adv.adv_int_max = (uint16_t)(unit + (unit / 32 ? unit / 32 : 1)); // never equal
        esp_ble_gap_config_adv_data_raw((uint8_t *)e->frame, e->frame_len);
        esp_ble_gap_start_advertising(&adv);
        return ESP_OK;
    }

    // Wi-Fi: transmit the prepared beacon template. The replay layer has already
    // stripped the FCS and overwritten the TSF + sequence-control fields; we set
    // en_sys_seq=false so the stack leaves those bytes untouched.
    return esp_wifi_80211_tx(WIFI_IF_STA, e->frame, e->frame_len, false);
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
