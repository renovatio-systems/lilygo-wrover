/*
 * WiFi station: scan, then connect to WIFI_SSID with retries.
 * Ported from lilygo-wifi. It no longer drives the LED; main.c owns the LED.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "secrets.h"
#include "wifi_sta.h"

#define WIFI_MAX_RETRY 10

static const char *TAG = "wifi_sta";
static int s_retry_count = 0;

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2_WPA3_PSK";
    default:                        return "UNKNOWN";
    }
}

/* Blocking active scan; must be called after esp_wifi_start() */
static void scan_and_log_networks(void)
{
    wifi_scan_config_t scan_config = { 0 };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "out of memory for %u scan results", ap_count);
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    ESP_LOGI(TAG, "found %u SSID(s):", ap_count);
    for (int i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "  [%2d] %-32s RSSI:%4d ch:%2d auth:%s",
                 i, (const char *) ap_records[i].ssid, ap_records[i].rssi,
                 ap_records[i].primary, auth_mode_str(ap_records[i].authmode));
    }

    free(ap_records);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "retrying connection to AP (%d/%d)", s_retry_count, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "failed to connect to SSID:%s after %d retries", WIFI_SSID, WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
    }
}

esp_err_t wifi_sta_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    scan_and_log_networks();

    ESP_LOGI(TAG, "connecting to SSID:%s", WIFI_SSID);
    return esp_wifi_connect();
}
