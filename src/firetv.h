#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Start the Fire TV task. It waits for WiFi, pairs if no token is stored
 * (PIN is typed on the serial console), then sends queued key actions.
 * Non-blocking. */
esp_err_t firetv_init(void);

/* Queue a remote key action, e.g. "select", "dpad_up", "home". The string must
 * be a literal (it is queued by pointer). Safe to call from BLE callbacks. */
void firetv_queue_action(const char *action);

/* True when no queued action is waiting (used to pace key repeat). */
bool firetv_idle(void);
