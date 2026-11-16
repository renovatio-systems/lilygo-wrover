#pragma once

#include "esp_err.h"

/* Start WiFi in station mode: scan and log visible SSIDs, then connect to
 * WIFI_SSID with retries. Non-blocking after the scan; the result is logged. */
esp_err_t wifi_sta_start(void);
