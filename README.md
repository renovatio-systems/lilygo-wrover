# BLE ring connector (LilyGo T-CALL / ESP32-WROVER)

Firmware for the LilyGo T-CALL board (ESP32-WROVER module). It scans for a BLE
ring advertising as `arcxp-1234`, connects, and bonds with it using legacy
Just Works pairing. The onboard blue LED shows the state.

## LED states

| LED         | Meaning                                  |
|-------------|------------------------------------------|
| Slow blink  | Booted, scanning for the ring (1 Hz)     |
| Rapid blink | Ring found, connecting / pairing (10 Hz) |
| Solid on    | Connected and paired (bonded)            |

The LED is on GPIO13 (`LED_G1` in the schematic).

## Hardware

- LilyGo T-CALL board (ESP32-WROVER, SIM800L on the same board; the SIM800L is not used here)
- USB cable to the board's USB-C port
- BLE ring advertising as `arcxp-1234`

## Requirements

- Python 3 with `venv`
- Linux user in the `dialout` group, so you can open the serial port without `sudo`:
  ```
  sudo usermod -aG dialout $USER
  ```
  Log out and back in afterwards.

## Install PlatformIO

Install it in its own venv so nothing is installed system-wide:

```
python3 -m venv ~/.venvs/platformio
~/.venvs/platformio/bin/pip install --upgrade platformio
```

The first build downloads the ESP32 toolchain and Arduino core (several hundred MB).

## Build

```
cd ble_ring
~/.venvs/platformio/bin/pio run
```

### Target device name

The firmware defaults to `arcxp-1234`. To target a different ring, pass its
advertised name at build time. Don't put the real name in any tracked file:

```
PLATFORMIO_BUILD_FLAGS='-DTARGET_NAME=\"<advertised-name>\"' ~/.venvs/platformio/bin/pio run
```

## Find the serial port

```
~/.venvs/platformio/bin/pio device list
```

The board's USB-UART is a CP2104 (`VID:PID 10C4:EA60`). Use that port,
for example `/dev/ttyUSB5`. Other USB serial adapters may appear too.

## Flash

Close any terminal session (`screen`, `picocom`, `microcom`) that has the port open.
Then:

```
~/.venvs/platformio/bin/pio run -t upload --upload-port /dev/ttyUSB5
```

The upload resets the board when it finishes.

## View the console log

The firmware prints lines ending in CR+LF. Use a terminal that handles them:

```
picocom -b 115200 --omap crlf /dev/ttyUSB5     # exit: Ctrl-A then Ctrl-X
```

`screen /dev/ttyUSB5 115200` works too, but lines may stair-step. Exit with
`Ctrl-A` then `K`.

Opening the port with some tools resets the board. Press the EN (reset) button
on the board if you need the full boot log.

## Expected log (successful pairing)

```
BLE ring connector starting
[sec] bonding ON, MITM OFF, SC OFF, IO cap NONE (Just Works), keys: enc+id
Scanning for "arcxp-1234"...
[scan] found "arcxp-1234" addr=xx:xx:xx:xx:xx:xx rssi=-40
[ble] connecting to arcxp-1234 addr=xx:xx:xx:xx:xx:xx
[ble] connected
[ble] connect ok in 174 ms
[sec] auth complete: OK  addr=xx:xx:xx:xx:xx:xx
[sec]   auth_mode=0x01 key_present=0 key_type=0
[ble] secureConnection() returned 1
[ble] wait ended: authDone=1 authOk=1 linkDown=0
[ble] paired - LED solid
```

## Troubleshooting

- **Upload fails with "port is busy":** another program has the port open. Find it with
  `ps -eo pid,cmd | grep -E "screen|picocom|microcom"` and close it.
- **Garbled log text:** the terminal is mixing output from two programs, or the baud rate doesn't match. Close other sessions and reconnect at 115200.
- **Ring never found:** check that the ring is advertising and within range. The name must match `arcxp-1234` exactly; the constant `TARGET_NAME` is in `src/main.cpp`.
- **Pairing fails with `secureConnection() returned 0`:** check the bonding settings in `src/main.cpp` (`setAuthenticationMode` and `setEncryptionLevel`). The ring may also need to be put into pairing mode first; see its own documentation.

## Project layout

```
ble_ring/
  platformio.ini   PlatformIO config (board, framework, speeds)
  src/main.cpp     BLE scan, connect, pair, and LED logic
  README.md        this file
```
