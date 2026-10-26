# hestia (Victim A) Firmware

µT-Kernel 3.0 dual-core SMP (`SMP=1`, IEEE 2050-2018) implementation for **hestia** (Raspberry Pi Pico RP2040 seated on Cytron Maker Pi Pico Base).

---

## System Role & Peripheral Allocation

| Subsystem | Core Binding (`TA_ASSPRC`) | Pins | Parameters | Role |
|---|---|---|---|---|
| **UART0 TX** | Processor 1 (`TP_PRC1`, Core 0) | GP0 TX (Row 10)<br>GP1 RX | 115200 baud, 8N1<br>Agile: 1, 2, 5, 10 Hz | Transmits telemetry bursts (`HESTIA:<counter>:<checksum>\n`) or 0xAA/0x55 stress bursts. Accepts runtime rate/mode commands on RX. |
| **USB CDC Console** | Processor 1 (Core 0) | USB (Micro-USB) | 115200 baud CDC ACM | Mirrors transmitted packet telemetry under SMP-safe console lock. Supports software reboot into BOOTSEL on 1200 baud or `B` command. |

---

## Message Protocol & Transmission Agility

### 1. Canonical Telemetry Line
Format:
```text
HESTIA:<counter>:<checksum>\n
```
- `<counter>`: 32-bit unsigned decimal integer incremented on each transmission.
- `<checksum>`: 16-bit bitwise NOT of the counter (`(uint16_t)(~counter)`), formatted as 4 uppercase hexadecimal digits (`%04X`).
- Default rate: 2 Hz (500 ms periodic interval).

### 2. Rate Agility
Configurable dynamically via API `hestia_uart_set_rate(hz)` or UART input commands:
- **1 Hz** (`'1'`): 1000 ms period (low bus traffic baseline).
- **2 Hz** (`'2'`): 500 ms period (default canonical rate).
- **5 Hz** (`'5'`): 200 ms period (intermediate stress rate).
- **10 Hz** (`'0'` / `'9'`): 100 ms period (high-frequency stress rate).

### 3. Test Bit Pattern Modes
Configurable dynamically via API `hestia_uart_set_mode(mode)` or UART input commands:
- **Mode 0 - CANONICAL** (`'C'`): Pure canonical telemetry ASCII line `HESTIA:<counter>:<checksum>\n`.
- **Mode 1 - PATTERN 0xAA** (`'A'`): Continuous burst of `0xAA` (`10101010b`) bytes for maximum edge transition density testing.
- **Mode 2 - PATTERN 0x55** (`'U'`): Continuous burst of `0x55` (`01010101b`) bytes for inverted phase transition testing.
- **Mode 3 - ALT BURST** (`'T'`): Alternates `0xAA` and `0x55` bursts on consecutive transmission intervals.
- **Mode 4 - INTERLEAVED** (`'I'`): Transmits canonical ASCII packet immediately followed by an alternating `0xAA` / `0x55` edge-stress burst.
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
