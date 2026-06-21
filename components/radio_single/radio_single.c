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
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "radio_single";

// BLE Bluedroid enum spans 16 levels; Wi-Fi PHY rounds to ~11 effective steps.
#define RADIO_SINGLE_BLE_LEVELS  16
#define RADIO_SINGLE_WIFI_LEVELS 11

// --- BLE advertising arbiter -----------------------------------------------
// The ESP32-S3 exposes a single legacy advertising instance. Every BLE emit
// (replay ghost, mesh HELLO, mesh DATA) wants it, and the naive path
// (reconfigure + restart on every rs_emit) means each frame holds the air for
// only milliseconds before the next emit clobbers it — far too short for a
// peer's ~30 ms scan window to land on it. So zero mesh frames ever reached the
// peer board even though scan RX worked.
//
// The arbiter serializes the instance through one task. rs_emit copies the
// frame into an adv_req_t and enqueues it (non-blocking); the task drains the
// queues one request at a time: config raw AdvData -> wait for the async
// ADV_DATA_RAW_SET_COMPLETE_EVT -> start advertising -> hold on air a minimum
// dwell (> one scan window) -> stop -> next. Mesh requests ride a separate
// high-priority queue drained ahead of ghosts, so replay chatter can never
// starve HELLO/DATA off the air.
#define ADV_REQ_FRAME_MAX 31     // legacy AdvData ceiling
#define ADV_MIN_DWELL_MS  90     // floor: hold each frame on air past a 30 ms scan window
#define ADV_DWELL_INTERVALS 4    // dwell >= this many adv intervals, so a frame is
                                 // actually broadcast several times (across all 3
                                 // channels) and overlaps multiple peer scan windows
#define ADV_MAX_DWELL_MS  400    // cap so one slow request can't hog the instance
#define ADV_Q_HI_DEPTH    40     // mesh: a full transfer is up to 16 records x ~2
                                 // frags, optionally repeated — size so a burst fits
                                 // without dropping fragments
#define ADV_Q_LO_DEPTH    8      // replay ghosts: best-effort, drop when full
#define ADV_DATA_SET_BIT  BIT0   // event-group bit: raw AdvData config completed

typedef struct {
    uint8_t  frame[ADV_REQ_FRAME_MAX];
    uint16_t frame_len;
    uint32_t interval_ms;
    int8_t   tx_power_idx;
} adv_req_t;

static QueueHandle_t       s_adv_q_hi;   // mesh
static QueueHandle_t       s_adv_q_lo;   // replay
static EventGroupHandle_t  s_adv_evt;
static TaskHandle_t        s_adv_task;

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
        // A beacon (mgmt subtype 8) is a broadcast mgmt frame by definition; we
        // only forward subtype-8 frames here, so the observed behavior is fixed.
        .adv_kind = AG_ADV_NONCONN_NONSCAN,
    };
    s_cap_cb(&cap, s_cap_user);
}

// Map the Bluedroid adv-report event type to the portable PDU-behavior kind.
// The eligibility gate clones only broadcast-only (NONCONN_NONSCAN) sources, so
// every connectable/scannable variant — and a scan response, which only exists
// for a scannable advertiser — must surface as the corresponding unsafe kind.
// Unrecognized values fail closed to AG_ADV_UNKNOWN.
static ag_adv_kind_t ble_evt_to_adv_kind(esp_ble_evt_type_t t)
{
    switch (t) {
        case ESP_BLE_EVT_NON_CONN_ADV: return AG_ADV_NONCONN_NONSCAN; // ADV_NONCONN_IND
        case ESP_BLE_EVT_DISC_ADV:     return AG_ADV_SCANNABLE;       // ADV_SCAN_IND
        case ESP_BLE_EVT_CONN_ADV:                                    // ADV_IND
        case ESP_BLE_EVT_CONN_DIR_ADV: return AG_ADV_CONNECTABLE;     // ADV_DIRECT_IND
        case ESP_BLE_EVT_SCAN_RSP:     return AG_ADV_SCANNABLE;       // implies scannable
        default:                       return AG_ADV_UNKNOWN;
    }
}

// --- BLE passive scan: surface legacy advertisements to the capture cb ------
static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    // The arbiter blocks on this bit after config_adv_data_raw; the controller
    // raises this event when the raw AdvData is actually loaded, so starting the
    // advertisement only then guarantees the peer scans the intended frame.
    if (event == ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT) {
        if (s_adv_evt) xEventGroupSetBits(s_adv_evt, ADV_DATA_SET_BIT);
        return;
    }
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
        // Observed PDU behavior from the adv-report event type. A connectable or
        // scannable source surfaces as such here so the eligibility gate refuses
        // to clone it as a broadcast-only ghost.
        .adv_kind = ble_evt_to_adv_kind(r->ble_evt_type),
    };
    s_cap_cb(&cap, s_cap_user);
}

// Apply the BLE ADV power level for one request (the per-ghost pre-TX hook,
// now run inside the arbiter so it lands on the right frame).
static void adv_apply_power(int8_t tx_power_idx)
{
    esp_power_level_t lvl = (esp_power_level_t)tx_power_idx;
    if (lvl < ESP_PWR_LVL_N24) lvl = ESP_PWR_LVL_N24;
    if (lvl > ESP_PWR_LVL_P21) lvl = ESP_PWR_LVL_P21;
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, lvl);
}

// Serialize the single legacy adv instance: one frame fully on air (configured,
// started, held a minimum dwell, stopped) before the next request is serviced.
static void adv_service(const adv_req_t *req)
{
    static esp_ble_adv_params_t adv = {
        .adv_type = ADV_TYPE_NONCONN_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    // Map interval_ms to the 0.625 ms adv unit; keep min<max (some stacks reject
    // equal bounds).
    uint16_t unit = (uint16_t)((req->interval_ms * 8) / 5);
    if (unit == 0) unit = 0xA0;
    adv.adv_int_min = unit;
    adv.adv_int_max = (uint16_t)(unit + (unit / 32 ? unit / 32 : 1));

    adv_apply_power(req->tx_power_idx);

    // Config raw AdvData, then block until the controller signals it loaded.
    xEventGroupClearBits(s_adv_evt, ADV_DATA_SET_BIT);
    esp_err_t rc = esp_ble_gap_config_adv_data_raw((uint8_t *)req->frame, req->frame_len);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "adv cfg failed: %d", (int)rc);
        return;
    }
    EventBits_t bits = xEventGroupWaitBits(s_adv_evt, ADV_DATA_SET_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(200));
    if (!(bits & ADV_DATA_SET_BIT)) {
        ESP_LOGW(TAG, "adv data set timed out");
        return;
    }
    rc = esp_ble_gap_start_advertising(&adv);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "adv start failed: %d", (int)rc);
        return;
    }
    // Hold long enough for the frame to actually broadcast several times: a dwell
    // shorter than the adv interval can stop the instance before a single TX
    // fires. Dwell across ADV_DWELL_INTERVALS intervals (floored/capped) so each
    // frame is repeated on all 3 channels and spans multiple peer scan windows.
    uint32_t dwell = req->interval_ms * ADV_DWELL_INTERVALS;
    if (dwell < ADV_MIN_DWELL_MS) dwell = ADV_MIN_DWELL_MS;
    if (dwell > ADV_MAX_DWELL_MS) dwell = ADV_MAX_DWELL_MS;
    vTaskDelay(pdMS_TO_TICKS(dwell));
    esp_ble_gap_stop_advertising();
}

// Arbiter task: drain mesh requests ahead of replay ghosts, one frame at a time.
static void adv_task(void *arg)
{
    (void)arg;
    adv_req_t req;
    for (;;) {
        // Mesh first (guaranteed slots). If none pending, block briefly on the
        // ghost queue so a stream of HELLOs can preempt between ghosts.
        if (xQueueReceive(s_adv_q_hi, &req, 0) == pdTRUE) {
            adv_service(&req);
        } else if (xQueueReceive(s_adv_q_lo, &req, pdMS_TO_TICKS(20)) == pdTRUE) {
            adv_service(&req);
        }
    }
}

// --- Serial time-division scheduler ----------------------------------------
// One segment on air at a time (concurrent sniffer+BLE is unstable). Each cycle
// runs a BLE scan window, then hops the Wi-Fi channel for a sniff dwell. BLE
// emits go through the adv arbiter task (independent of this scheduler); the
// controller interleaves passive scan and advertising on its own. The scheduler
// only owns the mutually-exclusive Wi-Fi sniff dwell.
static void scheduler_task(void *arg)
{
    (void)arg;
    // The scanner's random address + scan params are configured asynchronously
    // from rs_init via the GAP event chain (set_rand_addr ->
    // SET_STATIC_RAND_ADDR_COMPLETE -> set_scan_params). Give that a moment to
    // settle before the first scan window.
    vTaskDelay(pdMS_TO_TICKS(200));

    // BLE scanning runs continuously; the controller interleaves it with any
    // active advertising (mesh HELLO / replay) on its own. Wi-Fi sniffing is the
    // segment that must be mutually exclusive with BLE, so the scheduler stops
    // scanning only for the Wi-Fi dwell, then resumes.
    esp_ble_gap_start_scanning(0);
    while (s_capturing) {
        // Longer BLE scan window so adv bursts from peers reliably overlap it.
        vTaskDelay(pdMS_TO_TICKS(300));

        // Wi-Fi sniff segment: pause BLE, hop channel, dwell, resume BLE.
        esp_ble_gap_stop_scanning();
        s_wifi_ch_idx = (uint8_t)((s_wifi_ch_idx + 1) % 3);
        esp_wifi_set_channel(s_wifi_channels[s_wifi_ch_idx], WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_ble_gap_start_scanning(0);
    }
    esp_ble_gap_stop_scanning();
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

    // Stand up the advertising arbiter: queues + completion event group + task.
    s_adv_q_hi = xQueueCreate(ADV_Q_HI_DEPTH, sizeof(adv_req_t));
    s_adv_q_lo = xQueueCreate(ADV_Q_LO_DEPTH, sizeof(adv_req_t));
    s_adv_evt  = xEventGroupCreate();
    if (!s_adv_q_hi || !s_adv_q_lo || !s_adv_evt) {
        ESP_LOGE(TAG, "adv arbiter alloc failed");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(adv_task, "ag_adv", 4096, NULL, 6, &s_adv_task);

    ESP_LOGI(TAG, "radio_single init (wifi NULL + ble scan + adv arbiter)");
    return ESP_OK;
}

static esp_err_t rs_deinit(void)
{
    s_capturing = false;
    esp_wifi_set_promiscuous(false);
    esp_ble_gap_stop_scanning();
    esp_ble_gap_stop_advertising();
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

static esp_err_t rs_emit(const ag_emit_t *e)
{
    if (!e || !e->frame || e->frame_len == 0) return ESP_ERR_INVALID_ARG;

    if (e->proto == AG_PROTO_BLE) {
        // Non-blocking handoff to the adv arbiter: copy the frame into a request
        // (the caller's buffer is transient) and enqueue. The arbiter applies tx
        // power, configures raw AdvData, and holds each frame on air a minimum
        // dwell before servicing the next. Mesh (priority) frames ride a separate
        // queue drained ahead of replay ghosts so chatter can't starve them.
        if (e->frame_len > ADV_REQ_FRAME_MAX) return ESP_ERR_INVALID_ARG;
        adv_req_t req = {
            .frame_len = e->frame_len,
            .interval_ms = e->interval_ms,
            .tx_power_idx = e->tx_power_idx,
        };
        memcpy(req.frame, e->frame, e->frame_len);
        QueueHandle_t q = e->priority ? s_adv_q_hi : s_adv_q_lo;
        // Always non-blocking. Mesh transfer enqueues a whole burst from the GAP
        // callback context, so blocking here would stall the scan/capture path;
        // the hi queue is sized to hold a full repeated burst, and a drop on a
        // full queue is acceptable for unacked gossip.
        if (xQueueSend(q, &req, 0) != pdTRUE) {
            if (e->priority) ESP_LOGW(TAG, "mesh adv queue full, dropped");
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    // Wi-Fi: transmit the prepared beacon template synchronously (its own TX
    // path, no contention with the adv instance). The replay layer has already
    // stripped the FCS and overwritten the TSF + sequence-control fields; we set
    // en_sys_seq=false so the stack leaves those bytes untouched.
    int8_t q = (int8_t)(e->tx_power_idx);     // Wi-Fi max-tx-power, 0.25 dBm units
    esp_wifi_set_max_tx_power(q);
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
