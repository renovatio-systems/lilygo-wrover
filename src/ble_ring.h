#pragma once

#include "esp_err.h"

typedef enum {
    BLE_RING_SCANNING,    /* looking for RING_NAME */
    BLE_RING_CONNECTING,  /* found it; connecting and pairing */
    BLE_RING_PAIRED,      /* connected and bonded */
} ble_ring_state_t;

/* Bring up Bluedroid (BLE only), configure bonding, and start scanning for
 * RING_NAME. Connects and pairs automatically; rescans after a disconnect. */
esp_err_t ble_ring_start(void);

ble_ring_state_t ble_ring_get_state(void);
