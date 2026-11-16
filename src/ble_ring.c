/*
 * BLE ring connector (ESP-IDF, Bluedroid).
 *
 * Scans for a ring whose name starts with RING_NAME, connects as a GATT client,
 * and bonds with legacy Just Works (bonding ON, MITM OFF, SC OFF, IO cap NONE).
 * After pairing it discovers the ARCX service (0x1666), enables notifications on
 * the Extended Key Event characteristic (0x1669), and maps button presses to Fire
 * TV actions. GATT layout and key codes are the ones used by firetv_ctrl.
 */

#include <stdbool.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"

#include "ble_ring.h"
#include "firetv.h"
#include "secrets.h"

#define GATTC_APP_ID  0
#define SCAN_SECONDS  0   /* 0 = scan until we stop it */

#define ARCX_UUID_SERVICE        0x1666
#define ARCX_UUID_EXT_KEY_EVENT  0x1669
#define UUID_CCCD                0x2902

/* Button states as sent by the ring (same numbering as firetv_ctrl). */
typedef enum {
    CENTER_PRESS = 0,
    CENTER_RELEASE,
    FORWARD_PRESS,
    FORWARD_RELEASE,
    BACKWARD_PRESS,
    BACKWARD_RELEASE,
    RIGHT_PRESS,    /* ids 6/7 are right, 8/9 left (observed; the overview doc has them swapped) */
    RIGHT_RELEASE,
    LEFT_PRESS,
    LEFT_RELEASE,
    SOS,
} arcx_ring_state_t;

static const char *const ARCX_STATE_NAMES[] = {
    "CENTER_PRESS",   "CENTER_RELEASE", "FORWARD_PRESS", "FORWARD_RELEASE",
    "BACKWARD_PRESS", "BACKWARD_RELEASE", "RIGHT_PRESS", "RIGHT_RELEASE",
    "LEFT_PRESS",     "LEFT_RELEASE",   "SOS"};

static const char *TAG = "ble_ring";

typedef enum {
    GATT_IDLE,
    GATT_SEARCHING,    /* looking for the ARCX service */
    GATT_REGISTERING,  /* registering for notifications on 0x1669 */
    GATT_CCCD,         /* writing the CCCD to enable notifications */
    GATT_READY,        /* notifications on; presses reach the Fire TV */
} gatt_stage_t;

static volatile ble_ring_state_t s_state = BLE_RING_SCANNING;
static volatile bool s_ring_found = false;
static uint8_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static esp_bd_addr_t s_ring_addr;
static esp_ble_addr_type_t s_ring_addr_type;

static gatt_stage_t s_gatt_stage = GATT_IDLE;
static bool s_srv_found = false;
static uint16_t s_srv_start = 0, s_srv_end = 0;
static uint16_t s_ext_handle = 0;

ble_ring_state_t ble_ring_get_state(void)
{
    return s_state;
}

/* 128-bit ARCX UUID: 00001666-1212-efde-1523-785feabcd124 (little endian). */
static esp_bt_uuid_t arcx_uuid(uint16_t id)
{
    static const uint8_t base[16] = {0x24, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
                                     0xDE, 0xEF, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00};
    esp_bt_uuid_t u = {.len = ESP_UUID_LEN_128};
    memcpy(u.uuid.uuid128, base, sizeof(base));
    u.uuid.uuid128[12] = id & 0xFF;
    u.uuid.uuid128[13] = id >> 8;
    return u;
}

static esp_bt_uuid_t uuid16(uint16_t id)
{
    esp_bt_uuid_t u = {.len = ESP_UUID_LEN_16};
    u.uuid.uuid16 = id;
    return u;
}

static bool uuid_equal(const esp_bt_uuid_t *a, const esp_bt_uuid_t *b)
{
    return a->len == b->len && memcmp(&a->uuid, &b->uuid, a->len) == 0;
}

static void start_scan(void)
{
    s_state = BLE_RING_SCANNING;
    esp_err_t err = esp_ble_gap_start_scanning(SCAN_SECONDS);
    ESP_LOGI(TAG, "scanning for \"%s*\" (%s)", RING_NAME, esp_err_to_name(err));
}

/* True if the advertised name (complete or short) starts with RING_NAME. */
static bool name_matches(uint8_t *adv, uint16_t len, char *name_out, size_t name_out_size)
{
    uint8_t name_len = 0;
    uint8_t *name = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (name == NULL) {
        name = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (name == NULL) {
        return false;
    }
    size_t n = name_len < name_out_size - 1 ? name_len : name_out_size - 1;
    memcpy(name_out, name, n);
    name_out[n] = '\0';
    return strncmp(name_out, RING_NAME, strlen(RING_NAME)) == 0;
}

static void set_security_params(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;   /* bonding, no MITM, no SC */
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;         /* Just Works */
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)));
    ESP_LOGI(TAG, "security: bonding ON, MITM OFF, SC OFF, IO cap NONE (Just Works), keys: enc+id");
}

static void on_key_event(const uint8_t *data, uint16_t len)
{
    if (len == 0 || data[0] > SOS) {
        ESP_LOGW(TAG, "unknown key event len=%u", len);
        return;
    }
    const char *action = NULL;
    switch (data[0]) {
    case CENTER_PRESS:   action = "select";     break;
    case FORWARD_PRESS:  action = "dpad_up";    break;
    case BACKWARD_PRESS: action = "dpad_down";  break;
    case LEFT_PRESS:     action = "dpad_left";  break;
    case RIGHT_PRESS:    action = "dpad_right"; break;
    case SOS:            action = "home";       break;
    default: break;   /* releases are ignored */
    }
    ESP_LOGI(TAG, "key %s", ARCX_STATE_NAMES[data[0]]);
    if (action) {
        firetv_queue_action(action);
    }
}

/* Step 1 of GATT setup, after pairing: find the ARCX service. */
static void gatt_start(void)
{
    s_gatt_stage = GATT_SEARCHING;
    s_srv_found = false;
    esp_bt_uuid_t svc = arcx_uuid(ARCX_UUID_SERVICE);
    esp_ble_gattc_search_service(s_gattc_if, s_conn_id, &svc);
}

static void gatt_fail(const char *what)
{
    ESP_LOGE(TAG, "%s; giving up on this connection", what);
    s_gatt_stage = GATT_IDLE;
    esp_ble_gattc_close(s_gattc_if, s_conn_id);
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        start_scan();
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        struct ble_scan_result_evt_param *r = &param->scan_rst;
        if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || s_ring_found || s_state != BLE_RING_SCANNING) {
            break;
        }
        char name[32];
        uint16_t len = r->adv_data_len + r->scan_rsp_len;
        if (name_matches(r->ble_adv, len, name, sizeof(name))) {
            ESP_LOGI(TAG, "found \"%s\" addr=" ESP_BD_ADDR_STR " rssi=%d",
                     name, ESP_BD_ADDR_HEX(r->bda), r->rssi);
            memcpy(s_ring_addr, r->bda, sizeof(esp_bd_addr_t));
            s_ring_addr_type = r->ble_addr_type;
            s_ring_found = true;
            esp_ble_gap_stop_scanning();
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (s_ring_found) {
            s_ring_found = false;
            s_state = BLE_RING_CONNECTING;
            ESP_LOGI(TAG, "connecting to " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(s_ring_addr));
            esp_err_t err = esp_ble_gattc_open(s_gattc_if, s_ring_addr, s_ring_addr_type, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ble_gattc_open() failed: %s", esp_err_to_name(err));
                start_scan();
            }
        } else if (s_state == BLE_RING_SCANNING) {
            start_scan();
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "[sec] security request from ring; accepting");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGI(TAG, "[sec] passkey request from ring; replying 000000");
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, 0);
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "[sec] passkey: %06lu", (unsigned long) param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        ESP_LOGI(TAG, "[sec] confirm numeric comparison %06lu: yes", (unsigned long) param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGI(TAG, "[sec] key event: key_type=%d", param->ble_security.ble_key.key_type);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        esp_ble_auth_cmpl_t *auth = &param->ble_security.auth_cmpl;
        if (auth->success) {
            s_state = BLE_RING_PAIRED;
            ESP_LOGI(TAG, "[sec] auth complete: OK  addr=" ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(auth->bd_addr));
            ESP_LOGI(TAG, "[sec]   auth_mode=0x%02x key_present=%d key_type=%d",
                     auth->auth_mode, auth->key_present, auth->key_type);
            ESP_LOGI(TAG, "paired - LED solid");
            gatt_start();
        } else {
            ESP_LOGE(TAG, "[sec] auth complete: FAILED  addr=" ESP_BD_ADDR_STR " fail_reason=0x%02x",
                     ESP_BD_ADDR_HEX(auth->bd_addr), auth->fail_reason);
            esp_ble_gattc_close(s_gattc_if, s_conn_id);   /* disconnect event restarts the scan */
        }
        break;
    }

    default:
        break;
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "gattc app register failed, status=%d", param->reg.status);
            break;
        }
        s_gattc_if = gattc_if;
        break;

    case ESP_GATTC_OPEN_EVT: {
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "connect failed, status=%d", param->open.status);
            start_scan();
            break;
        }
        s_conn_id = param->open.conn_id;
        ESP_LOGI(TAG, "connected, conn_id=%d", s_conn_id);

        /* Start encryption/pairing. Uses stored keys if already bonded. */
        esp_err_t err = esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
        ESP_LOGI(TAG, "esp_ble_set_encryption() -> %s (0x%x)", esp_err_to_name(err), err);
        if (err != ESP_OK) {
            esp_ble_gattc_close(gattc_if, s_conn_id);
        }
        break;
    }

    case ESP_GATTC_SEARCH_RES_EVT: {
        esp_bt_uuid_t svc = arcx_uuid(ARCX_UUID_SERVICE);
        if (!s_srv_found && uuid_equal(&param->search_res.srvc_id.uuid, &svc)) {
            s_srv_found = true;
            s_srv_start = param->search_res.start_handle;
            s_srv_end = param->search_res.end_handle;
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (s_gatt_stage != GATT_SEARCHING) {
            break;
        }
        if (!s_srv_found) {
            gatt_fail("ARCX service (0x1666) not found");
            break;
        }
        ESP_LOGI(TAG, "ARCX service handles 0x%04x-0x%04x", s_srv_start, s_srv_end);

        esp_bt_uuid_t ext = arcx_uuid(ARCX_UUID_EXT_KEY_EVENT);
        esp_gattc_char_elem_t ch;
        uint16_t count = 1;
        esp_gatt_status_t st = esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id,
                                                              s_srv_start, s_srv_end, ext,
                                                              &ch, &count);
        if (st != ESP_GATT_OK || count == 0) {
            gatt_fail("Extended Key Event characteristic (0x1669) not found");
            break;
        }
        s_ext_handle = ch.char_handle;
        s_gatt_stage = GATT_REGISTERING;
        esp_ble_gattc_register_for_notify(gattc_if, s_ring_addr, s_ext_handle);
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (s_gatt_stage != GATT_REGISTERING) {
            break;
        }
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            gatt_fail("register for notify failed");
            break;
        }
        /* Find the CCCD on the key-event characteristic and enable notifications. */
        esp_bt_uuid_t cccd = uuid16(UUID_CCCD);
        esp_gattc_descr_elem_t d;
        uint16_t dcount = 1;
        esp_gatt_status_t st = esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id,
                                                                      s_ext_handle, cccd, &d, &dcount);
        if (st != ESP_GATT_OK || dcount == 0) {
            gatt_fail("CCCD not found on key-event characteristic");
            break;
        }
        uint8_t value[2] = {0x01, 0x00};   /* notifications on */
        s_gatt_stage = GATT_CCCD;
        esp_ble_gattc_write_char_descr(gattc_if, s_conn_id, d.handle, sizeof(value), value,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (s_gatt_stage != GATT_CCCD) {
            break;
        }
        if (param->write.status != ESP_GATT_OK) {
            gatt_fail("enabling notifications failed");
            break;
        }
        s_gatt_stage = GATT_READY;
        ESP_LOGI(TAG, "notifications enabled; press the ring");
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (s_gatt_stage == GATT_READY && param->notify.handle == s_ext_handle) {
            on_key_event(param->notify.value, param->notify.value_len);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "disconnected, reason=0x%x", param->disconnect.reason);
        s_gatt_stage = GATT_IDLE;
        start_scan();
        break;

    default:
        break;
    }
}

esp_err_t ble_ring_start(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(GATTC_APP_ID));

    set_security_params();

    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));
    return ESP_OK;
}
