# hestia (Victim A) Firmware

µT-Kernel 3.0 dual-core SMP (`SMP=1`, IEEE 2050-2018) implementation for **hestia** (Raspberry Pi Pico RP2040 seated on Cytron Maker Pi Pico Base).

---

## System Role & Peripheral Allocation

| Subsystem | Core Binding (`TA_ASSPRC`) | Pins | Parameters | Role |
|---|---|---|---|---|
| **UART0 TX** | Processor 1 (`TP_PRC1`, Core 0) | GP0 (Row 10) | 115200 baud, 8N1, 2 Hz | Transmits formatted telemetry bursts (`HESTIA:<counter>:<checksum>\n`) with bitwise NOT checksum (`~counter`). |
| **USB CDC Console** | Processor 1 (Core 0) | USB (Micro-USB) | 115200 baud CDC ACM | Mirrors transmitted packet telemetry under SMP-safe console lock. Supports software reboot into BOOTSEL on 1200 baud or `B` command. |

---

## Message Protocol Specification

Line format:
```text
HESTIA:<counter>:<checksum>\n
```
- `<counter>`: 32-bit unsigned decimal integer incremented on each transmission.
- `<checksum>`: 16-bit bitwise NOT of the counter (`(uint16_t)(~counter)`), formatted as 4 uppercase hexadecimal digits (`%04X`).
- Rate: 2 Hz (500 ms periodic interval).

---

## Building and Flashing

### 1. Build UF2
```bash
make -C whattheflip/hestia
```
Generates `whattheflip/hestia/hestia.uf2`.

### 2. Flash to Device
1. Put Hestia into BOOTSEL mode (hold BOOTSEL button and press RESET or reconnect cable).
2. Run:
```bash
make -C whattheflip/hestia flash
```
Flashes `hestia.uf2` to `/Volumes/RPI-RP2` using `dd` block-aligned transfer.
