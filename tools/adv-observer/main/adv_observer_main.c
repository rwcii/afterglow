// adv_observer_main.c — standalone BLE advertisement observer.
//
// Passively scans BLE and prints one line per legacy advertisement:
//   ADV <addr-type> <AdvA> rssi=<dBm> len=<n> data=<hex> [txp=<dBm>]
// where <addr-type> is "pub" or "rnd" (the advertiser address type from the
// scan-result event), and txp is the decoded TX Power Level (0x0A) AD value
// when the advertisement carries one.
//
// Intended as a second-board observer for inspecting what a device under test
// actually radiates: the advertising address and its type, the raw AdvData, and
// the TX Power AD field. It only receives; it never transmits.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"

static const char *TAG = "adv-observer";

static esp_ble_scan_params_t scan_params = {
    .scan_type          = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,   // 50 ms
    .scan_window        = 0x30,   // 30 ms
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

// Decode the TX Power Level (0x0A) AD field, returning the dBm value in *out and
// true if present. Walks the AD length/type list; reads no other field.
static bool decode_tx_power(const uint8_t *ad, uint8_t len, int8_t *out)
{
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t flen = ad[i];
        if (flen == 0) break;
        if (i + 1 + flen > len) break;
        if (ad[i + 1] == 0x0A && flen == 2) {
            *out = (int8_t)ad[i + 2];
            return true;
        }
        i = (uint8_t)(i + 1 + flen);
    }
    return false;
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0);   // 0 = scan until stopped
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (p->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan start failed: %d", p->scan_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "scanning");
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        struct ble_scan_result_evt_param *r = &p->scan_rst;
        if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        const char *type = (r->ble_addr_type == BLE_ADDR_TYPE_PUBLIC) ? "pub" : "rnd";
        uint8_t n = r->adv_data_len > 31 ? 31 : r->adv_data_len;
        char hex[2 * 31 + 1];
        for (uint8_t i = 0; i < n; i++) {
            static const char *H = "0123456789abcdef";
            hex[2 * i]     = H[r->ble_adv[i] >> 4];
            hex[2 * i + 1] = H[r->ble_adv[i] & 0xF];
        }
        hex[2 * n] = '\0';

        int8_t txp;
        char txp_str[16] = "";
        if (decode_tx_power(r->ble_adv, n, &txp)) {
            snprintf(txp_str, sizeof(txp_str), " txp=%d", txp);
        }

        printf("ADV %s %02x:%02x:%02x:%02x:%02x:%02x rssi=%d len=%u data=%s%s\n",
               type, r->bda[0], r->bda[1], r->bda[2], r->bda[3], r->bda[4],
               r->bda[5], r->rssi, n, hex, txp_str);
        break;
    }
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    esp_bt_controller_config_t bcfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bcfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));
    ESP_LOGI(TAG, "adv-observer up");
}
