# Makefile for Hestia (Victim A) - µT-Kernel 3.0 SMP on RP2040
#
# Usage:
#   make          - Build hestia.uf2
#   make flash    - Flash to mounted /Volumes/RPI-RP2

ROOT_DIR     := $(shell pwd)
PORT_DIR     := $(ROOT_DIR)/../third_party/mtk3smp-rp2040
BUILD_DIR    := $(PORT_DIR)/build_make
APP_DIR      := $(PORT_DIR)/app_program
PICO_SDK_DIR := ../../pico-sdk

.PHONY: all clean flash

all: hestia.uf2

hestia.uf2: hestia_main.c hestia_uart.c hestia_uart.h ../shared/packet_types.h
	@echo "==> Staging Hestia application files into $(APP_DIR)"
	@mkdir -p $(APP_DIR)
	@rm -f $(APP_DIR)/*.c $(APP_DIR)/*.h
	@cp hestia_main.c hestia_uart.c hestia_uart.h ../shared/packet_types.h $(APP_DIR)/
	@echo "==> Building µT-Kernel 3.0 SMP for RP2040 (CONSOLE=usb_cdc)..."
	$(MAKE) -C $(BUILD_DIR) SMP=1 CONSOLE=usb_cdc PICO_SDK_PATH=$(PICO_SDK_DIR)
	@cp $(BUILD_DIR)/mtk3pico_smp1_usb_cdc.uf2 ./hestia.uf2
	@echo "==> Built: ./hestia.uf2"

flash: hestia.uf2
	@if [ ! -d "/Volumes/RPI-RP2" ]; then \
		echo "ERROR: /Volumes/RPI-RP2 is not mounted. Put Hestia into BOOTSEL mode."; \
		exit 1; \
	fi
	@echo "==> Flashing hestia.uf2 to /Volumes/RPI-RP2 via dd (bs=512)..."
	dd if=hestia.uf2 of=/Volumes/RPI-RP2/hestia.uf2 bs=512 conv=sync
	@echo "==> Flashed successfully. Device will reboot."

clean:
	$(MAKE) -C $(BUILD_DIR) clean
	rm -f hestia.uf2
