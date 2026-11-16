# BLE ring connector: build helpers for PlatformIO
#
#   make setup    install PlatformIO into a private venv
#   make build    compile the firmware
#   make flash    upload to the board
#   make clean    remove build output
#
# Overrides (command line or environment):
#   PORT=/dev/ttyUSB5          serial port of the board's CP2104
#   RING_NAME=<advertised>     name the firmware scans for
#   VENV=$HOME/.venvs/platformio

VENV      ?= $(HOME)/.venvs/platformio
PIO       ?= $(VENV)/bin/pio
PORT      ?= /dev/ttyUSB5
RING_NAME ?= arcxp-1234

# Machine-local settings (gitignored), e.g. RING_NAME := <real name>
-include local.mk

.PHONY: help setup build flash clean

help:
	@echo "Targets: setup build flash clean"

setup:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade platformio
	$(PIO) --version

build:
	PLATFORMIO_BUILD_FLAGS='-DTARGET_NAME=\"$(RING_NAME)\"' $(PIO) run

flash:
	PLATFORMIO_BUILD_FLAGS='-DTARGET_NAME=\"$(RING_NAME)\"' $(PIO) run -t upload --upload-port $(PORT)

clean:
	$(PIO) run -t clean
