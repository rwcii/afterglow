// adv_stimulus_main.c — controllable BLE advertiser for the on-air test rig.
//
// Reads single-character commands on the console UART and advertises a known
// source accordingly, printing a ground-truth line per command so a host test
// can correlate what a device under test should observe:
//
//   s  static-random (0b11) broadcast-only beacon   -> cloneable, replayable
//   n  NRPA (0b00) broadcast-only beacon            -> cloneable, replayable
//   c  static-random but CONNECTABLE                -> must never be replayed
//   r  RPA (0b01) broadcast-only                    -> must be gated (uncloneable)
//   v  reserved (0b10) broadcast-only               -> must be gated (unsettable)
//   x  stop advertising (the "walk out")
//
// On each advertise command it prints: STIM <cmd> addr=<aa:bb:..> type=<t>.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "adv-stimulus";

// A recognized broadcast-beacon payload (iBeacon-shaped: manufacturer-specific
// 0xFF). The classifier requires a beacon-like payload to promote a random
// source, so the cloneable cases must carry one.
static uint8_t s_adv_data[] = {
    0x02, 0x01, 0x06,                                     // Flags
    0x0A, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0xAB, 0xCD, 0xEF, 0x01, // mfg (iBeacon-ish)
    0x02, 0x0A, 0xF4,                                     // TX Power = -12 dBm
};

// Fixed address bodies; the top two bits of byte[5] (printed MSB-first as the
// first address octet) carry the random subtype and are set per command.
static uint8_t s_addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x00 };

static esp_ble_adv_params_t s_params = {
    .adv_int_min       = 0xA0,   // 100 ms
    .adv_int_max       = 0xB0,
    .own_addr_type     = BLE_ADDR_TYPE_RANDOM,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static bool s_data_set;
static bool s_addr_set;

static void set_subtype(uint8_t top_bits)
{
    s_addr[5] = (uint8_t)((s_addr[5] & 0x3F) | (top_bits << 6));
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    switch (event) {
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
        s_addr_set = (p->set_rand_addr_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_data_set = true;
        break;
    default:
        break;
    }
}

// Start advertising the given subtype + PDU type. subtype is the random-address
// top-two-bits (0b00/0b01/0b10/0b11); connectable selects ADV_IND vs
// ADV_NONCONN_IND. tag distinguishes the address body so records from different
// commands never collide in the device-under-test's pool.
static void advertise(char cmd, uint8_t subtype, bool connectable, uint8_t tag)
{
    esp_ble_gap_stop_advertising();
    vTaskDelay(pdMS_TO_TICKS(50));   // let advertising actually stop before re-addr
    s_addr[0] = tag;
    set_subtype(subtype);

    s_addr_set = false;
    esp_err_t rc = esp_ble_gap_set_rand_addr(s_addr);
    // For the reserved/RPA negative cases the controller may reject the address;
    // report it but still print ground truth so the host knows what was intended.
    for (int i = 0; i < 100 && !s_addr_set && rc == ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_params.adv_type = connectable ? ADV_TYPE_IND : ADV_TYPE_NONCONN_IND;
    s_data_set = false;
    esp_ble_gap_config_adv_data_raw(s_adv_data, sizeof s_adv_data);
    for (int i = 0; i < 20 && !s_data_set; i++) vTaskDelay(pdMS_TO_TICKS(10));
    esp_ble_gap_start_advertising(&s_params);

    printf("STIM %c addr=%02x:%02x:%02x:%02x:%02x:%02x type=%s set=%d\n",
           cmd, s_addr[5], s_addr[4], s_addr[3], s_addr[2], s_addr[1], s_addr[0],
           connectable ? "conn" : "nonconn", s_addr_set);
}

static void console_task(void *arg)
{
    (void)arg;
    uint8_t ch;
    for (;;) {
        // The console is on the USB-Serial-JTAG peripheral (not UART0), so read
        // host commands from that driver.
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(200));
        if (n <= 0) continue;
        switch (ch) {
        case 's': advertise('s', 0x3, false, 0x51); break; // static-random, nonconn
        case 'n': advertise('n', 0x0, false, 0x52); break; // NRPA, nonconn
        case 'c': advertise('c', 0x3, true,  0x53); break; // static-random, CONNECTABLE
        case 'r': advertise('r', 0x1, false, 0x54); break; // RPA, nonconn
        case 'v': advertise('v', 0x2, false, 0x55); break; // reserved, nonconn
        case 'x': esp_ble_gap_stop_advertising(); printf("STIM x stopped\n"); break;
        default: break;
        }
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

    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));

    xTaskCreate(console_task, "stim_console", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "adv-stimulus up (cmds: s n c r v x)");
}
