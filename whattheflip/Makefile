# Makefile for whattheflip on ESP32-C6 (µT-Kernel 3.0)
#
# Usage:
#   make          - Build ESP-IDF firmware binary
#   make flash    - Flash to ESP32-C6 via USB Serial/JTAG (/dev/cu.usbmodem101)

PORT ?= /dev/cu.usbmodem101
ROOT_DIR := $(shell pwd)
IDF_EXPORT := . $(ROOT_DIR)/../third_party/esp-idf/export.sh

.PHONY: all build flash clean

all: build

build:
	@echo "==> Building whattheflip with ESP-IDF and µT-Kernel 3.0..."
	$(IDF_EXPORT) && idf.py build

flash:
	@echo "==> Flashing whattheflip to $(PORT)..."
	$(IDF_EXPORT) && idf.py -p $(PORT) flash

clean:
	@$(IDF_EXPORT) && idf.py fullclean
