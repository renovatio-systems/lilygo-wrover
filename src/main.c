/*
 * LilyGo T-Call (ESP32-WROVER-B): WiFi station + BLE ring connector.
 *
 * WiFi and BLE start at boot and run independently. The onboard blue LED
 * (GPIO13) shows the BLE ring state:
 *   slow blink (1 Hz)   scanning for RING_NAME
 *   rapid blink (10 Hz) ring found, connecting / pairing
 *   solid on            connected and bonded
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_ring.h"
#include "firetv.h"
#include "wifi_sta.h"

/* Onboard LED, per LilyGo T-Call (IP5306 variant) pin map */
#define LED_GPIO  13
#define LED_ON    1
#define LED_OFF   0

static const char *TAG = "app";

static void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, LED_OFF);
}

static void led_task(void *arg)
{
    bool level = false;
    for (;;) {
        switch (ble_ring_get_state()) {
        case BLE_RING_PAIRED:
            gpio_set_level(LED_GPIO, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        case BLE_RING_CONNECTING:
            level = !level;
            gpio_set_level(LED_GPIO, level ? LED_ON : LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(50));    /* 10 Hz */
            break;
        case BLE_RING_SCANNING:
        default:
            level = !level;
            gpio_set_level(LED_GPIO, level ? LED_ON : LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(500));   /* 1 Hz */
            break;
        }
    }
}

void app_main(void)
{
    led_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(wifi_sta_start());
    ESP_ERROR_CHECK(firetv_init());     /* before BLE: ring presses are queued to it */
    ESP_ERROR_CHECK(ble_ring_start());

    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "started: wifi station + ble ring connector");
}
