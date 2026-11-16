# LilyGo T-Call: WiFi station + BLE ring connector

ESP-IDF firmware for the LilyGo T-Call board (ESP32-WROVER-B, 4 MB flash).
At boot it does three things at once:

- **WiFi:** scans and logs visible SSIDs, then connects to `WIFI_SSID` with retries.
- **BLE:** scans for a ring whose name starts with `RING_NAME`, connects, and bonds
  with it (legacy Just Works: bonding ON, MITM OFF, Secure Connections OFF).
- **Fire TV:** once the ring is bonded, it subscribes to the ring's key-event
  notifications (ARCX service 0x1666, characteristic 0x1669) and sends the matching
  remote key to the Fire TV at `FIRETV_HOST`.

Ring buttons (Fire TV mode):

| Button    | Fire TV key |
|-----------|-------------|
| center    | select      |
| forward   | dpad up     |
| backward  | dpad down   |
| left      | dpad left   |
| right     | dpad right  |
| SOS       | home        |

The onboard blue LED (GPIO13) shows the BLE ring state:

| LED         | Meaning                                   |
|-------------|-------------------------------------------|
| Slow blink  | Scanning for the ring (1 Hz)              |
| Rapid blink | Ring found, connecting / pairing (10 Hz)  |
| Solid on    | Connected and bonded                      |

WiFi status is logged only and doesn't affect the LED.

## Hardware

- LilyGo T-Call board (ESP32-WROVER-B). The SIM800L is not used.
- USB-C cable to the board.
- BLE ring advertising as `RING_NAME`.

## Requirements

- Python 3 with `venv`.
- Linux user in the `dialout` group, so you can open the serial port without `sudo`:
  ```
  sudo usermod -aG dialout $USER
  ```
  Log out and back in afterwards.

## Configure

Copy the template and fill it in. `src/secrets.h` is gitignored and must never be committed:

```
cp src/secrets.h.example src/secrets.h
```

Edit `src/secrets.h`:
- `WIFI_SSID`, `WIFI_PASSWORD`: your network.
- `RING_NAME`: the ring's advertised name (prefix match).
- `FIRETV_HOST`: the Fire TV's IP address.

The Fire TV must be paired once. On first boot without a stored token, the firmware
asks for the PIN shown on the TV. Type it into the serial console when prompted
(`Enter the PIN shown on the TV:`). The token is saved in NVS. To pair again,
erase the flash (`pio run -t erase`) and reflash.

## Build and flash

```
make setup     # once: installs PlatformIO into ~/.venvs/platformio
make build     # first build downloads the ESP-IDF framework and toolchain
make flash     # uploads to /dev/ttyUSB5 (override with PORT=...)
make clean
```

PlatformIO builds the project with the `espidf` framework (`platformio.ini`),
using the CMake layout in `CMakeLists.txt` and `src/CMakeLists.txt`.

Find the serial port with `~/.venvs/platformio/bin/pio device list`. The board's
USB-UART is a CP2104 (`10C4:EA60`).

Close any terminal program on the port (`screen`, `picocom`, `microcom`) before flashing.

## Security note

The Fire TV API uses a self-signed HTTPS certificate on port 8080. Certificate
verification is turned off for all TLS in this firmware
(`CONFIG_ESP_TLS_INSECURE` and `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` in
`sdkconfig.defaults`). Anyone on the network can impersonate the TV. Only use this
on a network you trust.

## View the console log

The firmware prints lines ending in LF. Use `picocom` with CR+LF mapping so lines
don't stair-step:

```
picocom -b 115200 --omap crlf /dev/ttyUSB5     # exit: Ctrl-A then Ctrl-X
```

Some tools reset the board when they open the port. Press EN (reset) on the
board if you need the full boot log.

## Expected log (successful pairing)

```
app: started: wifi station + ble ring connector
wifi_sta: found N SSID(s): ...
ble_ring: security: bonding ON, MITM OFF, SC OFF, IO cap NONE (Just Works), keys: enc+id
ble_ring: scanning for "<RING_NAME>" (ESP_OK)
ble_ring: found "<RING_NAME>" addr=xx:xx:xx:xx:xx:xx rssi=-40
ble_ring: connecting to xx:xx:xx:xx:xx:xx
ble_ring: connected, conn_id=0
ble_ring: esp_ble_set_encryption() -> ESP_OK (0x0)
ble_ring: [sec] auth complete: OK  addr=xx:xx:xx:xx:xx:xx
ble_ring: paired - LED solid
```

## Troubleshooting

- **`port is busy`:** another program has the port open. Close it and retry.
- **Garbled log text:** the terminal is mixing output from two programs, or the baud rate is wrong. Use 115200 and close other sessions.
- **Ring not found:** check that it's advertising, is in range, and that `RING_NAME` in `src/secrets.h` matches exactly.
- **`esp_ble_set_encryption()` returns an error:** the log shows the `esp_err_t`. Check the bonding settings in `src/ble_ring.c` and whether the ring needs pairing mode first.
- **`auth complete: FAILED`:** the log shows the `fail_reason` (SMP reason code).

## Project layout

```
CMakeLists.txt        project root (ESP-IDF)
platformio.ini        PlatformIO config (espidf framework, board, partitions)
no_ota.csv            partition table (2 MB app, SPIFFS)
sdkconfig.defaults    ESP-IDF options (4 MB flash, BLE-only Bluedroid)
Makefile              setup / build / flash / clean
src/
  CMakeLists.txt      component registration
  main.c              app_main, LED state
  wifi_sta.c/.h       WiFi station: scan and connect
  ble_ring.c/.h       BLE scan, bond, ARCX GATT, key events
  firetv.c/.h         Fire TV remote client (HTTPS, PIN pairing, NVS token)
  secrets.h.example   template for src/secrets.h (gitignored)
```
