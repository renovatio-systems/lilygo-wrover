/*
 * BLE ring connector (ESP-IDF, Bluedroid).
 *
 * Scans for RING_NAME, connects as a GATT client, starts encryption with
 * legacy Just Works bonding (bonding ON, MITM OFF, SC OFF, IO cap NONE),
 * and reports the state via ble_ring_get_state().
 *
 * ESP-IDF calls the link-level encryption explicitly with
 * esp_ble_set_encryption() after the connection opens. Pairing is driven
 * by the GAP auth-complete event.
 */

#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"

#include "ble_ring.h"
#include "secrets.h"

#define GATTC_APP_ID  0
#define SCAN_SECONDS  0   /* 0 = scan until we stop it */

static const char *TAG = "ble_ring";

static volatile ble_ring_state_t s_state = BLE_RING_SCANNING;
static volatile bool s_ring_found = false;
static uint8_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static esp_bd_addr_t s_ring_addr;
static esp_ble_addr_type_t s_ring_addr_type;

ble_ring_state_t ble_ring_get_state(void)
{
    return s_state;
}

static void start_scan(void)
{
    s_state = BLE_RING_SCANNING;
    esp_err_t err = esp_ble_gap_start_scanning(SCAN_SECONDS);
    ESP_LOGI(TAG, "scanning for \"%s\" (%s)", RING_NAME, esp_err_to_name(err));
}

/* True if the advertising data (adv + scan response) names the ring. */
static bool name_matches(uint8_t *adv, uint16_t len)
{
    uint8_t name_len = 0;
    uint8_t *name = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (name == NULL) {
        name = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (name == NULL) {
        return false;
    }
    return name_len == strlen(RING_NAME) && memcmp(name, RING_NAME, name_len) == 0;
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
        uint16_t len = r->adv_data_len + r->scan_rsp_len;
        if (name_matches(r->ble_adv, len)) {
            ESP_LOGI(TAG, "found \"%s\" addr=" ESP_BD_ADDR_STR " rssi=%d",
                     RING_NAME, ESP_BD_ADDR_HEX(r->bda), r->rssi);
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

    case ESP_GATTC_OPEN_EVT:
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

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "disconnected, reason=0x%x", param->disconnect.reason);
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
