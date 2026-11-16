# LilyGo T-Call WiFi + BLE ring connector (ESP-IDF via PlatformIO)
#
#   make setup    install PlatformIO into a private venv
#   make build    compile the firmware
#   make flash    upload to the board
#   make clean    remove build output
#
# Overrides (command line or environment):
#   PORT=/dev/ttyUSB5          serial port of the board's CP2104
#   VENV=$HOME/.venvs/platformio

VENV ?= $(HOME)/.venvs/platformio
PIO  ?= $(VENV)/bin/pio
PORT ?= /dev/ttyUSB5

.PHONY: help setup build flash clean

help:
	@echo "Targets: setup build flash clean"

setup:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade platformio
	$(PIO) --version

build:
	$(PIO) run

flash:
	$(PIO) run -t upload --upload-port $(PORT)

clean:
	$(PIO) run -t clean
