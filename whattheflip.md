---
module: INF2004
group: SE06
lab: P1
updated: 2026-10-27
os:
  whattheflip: esp32c6-mtk3
  iris: esp32c6-mtk3
  hestia: mtk3smp-rp2040 (SMP=1)
  daphne: mtk3smp-rp2040 (SMP=1)
---

# whattheflip

Sniffs UART and I2C, captures Wi-Fi 6 and 802.15.4 over the air, injects small synchronized controlled faults into the hestia and daphne buses, writes standard binary PCAP 2.4 to microSD, and streams live telemetry to a web dashboard.

## The hardware

Boards are enumerated dynamically by silicon USB serial, eFuse MAC, and RP2040 unique flash chip ID rather than fixed tty paths.

- whattheflip:
  - Silicon USB serial: 8C:FD:49:0D:18:40
  - eFuse MAC: 8c:fd:49:0d:18:40 (SoftAP MAC: 8c:fd:49:0d:18:41)
  - Waveshare ESP32-C6-Pico https://docs.waveshare.com/ESP32-C6-Pico
  - Cytron Maker Pi Pico Base https://sg.cytron.io/p-maker-pi-pico-base
  - Delkin USDCOEM-32GB MicroSD https://www.delkin.com/product/usdcoem-32gb/
- hestia:
  - Flash chip ID: E66430A64B586C38
  - Raspberry Pi Pico (RP2040) https://www.raspberrypi.com/products/raspberry-pi-pico/
  - Cytron Maker Pi Pico Base https://sg.cytron.io/p-maker-pi-pico-base
- daphne:
  - Flash chip ID: E66480454F73A231
  - Raspberry Pi Pico (RP2040) https://www.raspberrypi.com/products/raspberry-pi-pico/
  - Cytron Maker Pi Pico Base https://sg.cytron.io/p-maker-pi-pico-base
- iris:
  - Silicon USB serial: 8C:FD:49:0C:87:50
  - eFuse MAC: 8c:fd:49:0c:87:50 (target BSSID: 02:00:00:00:00:03)
  - Waveshare ESP32-C6-Pico (bare board) https://docs.waveshare.com/ESP32-C6-Pico
- accessories:
  - Zhong Yi Breadboard MB-102 https://www.alibaba.com/product-detail/MB-102-830-Tie-point-Solderless_60706552524.html
  - GY-511 sensor breakout module (LSM303DLHC) resources/OKY3231-2 Sensor Breakout Module
  - 220 Ω resistor
  - 2× Micro-USB cables, 2× USB-C cables
  - 19× M-M jumper wires, 4× F-M jumper wires

## Tech Stack

### Firmware & OS
- Operating Systems: µT-Kernel 3.0 (ESP32-C6 single-core RV32IMAC), µT-Kernel 3.0 SMP (RP2040 dual-core Cortex-M0+, SMP=1)
- Language: Embedded C (Barr C standard, zero dynamic memory allocation)
- SDKs: ESP-IDF 6.2, Raspberry Pi Pico SDK
- Toolchains: riscv32-esp-elf-gcc, arm-none-eabi-gcc, CMake, Ninja, Make

### Dashboard & Host
- Host Automation: uv (PEP 723 inline dependency management)
- Frontend:
```text
Frontend
├── VanJS
├── Vanilla CSS
├── Native browser WebSocket API
├── Native Fetch API
└── esbuild + gzip for production builds
```

## Physical Allocation

### Diagnostic Probes
- Probe 0 (GPIO4): Row B12. Fault injection probe for 10 µs bit flips into Daphne GP1 RX. Protected by 220 Ω resistor R1.
- Probe 1 (GPIO5): Row B10. Clean passive UART listener on Hestia GP0 TX at 115200 8N1.
- Probe 2 (GPIO6): Row C43. I2C SDA passive sniffer and 50 µs ACK-kill injector.
- Probe 3 (GPIO7): Row C42. I2C SCL passive sniffer and 50 µs clock-stretch injector.

### Board Peripherals
- E-Stop (GPIO9): BOOT button. Active-low edge interrupt triggers GPIO.enable_w1tc to float Probes 0..3 in < 200 ns.
- Status RGB LED (GPIO8): WS2812 NeoPixel. Green = Armed, Yellow = Fault Injecting (150 ms hold), Red = E-Stop Aborted.
- MicroSD SPI: GPIO18 (SCK), GPIO19 (MOSI), GPIO20 (MISO), GPIO21 (CS via wire W19 from GP15 to GP13). 4 µs software delay on SCK transitions.
- Daphne Buzzer: Native GP18 driven via SIO. 2 kHz acoustic chirp on startup, UART corruptions, and I2C stalls.
- Daphne Status LEDs: GP2 Blue LED on I2C recovery, GP3 Blue LED on UART corruptions.

### Breadboard Wiring Schedule

All signal connections use columns A–E on the left side of the breadboard. Zero bridge wires across the center trough.

| # | Wire Type | Connect From (Board & Pin) | Connect To (Breadboard) | What It Does |
|---|:---:|---|---|---|
| R1 | 220 Ω resistor | Breadboard E10 | Breadboard E12 | Limits fault contention current into driven lines to <= 15 mA |
| W1 | M-M | whattheflip pin 38 (GND) | (−) Blue rail (Row 3) | Common ground reference |
| W2 | M-M | hestia pin 3 (GND) | (−) Blue rail (Row 5) | Common ground reference |
| W3 | M-M | daphne pin 3 (GND) | (−) Blue rail (Row 10) | Common ground reference |
| W4 | M-M | daphne pin 36 (3V3_OUT) | (+) Red rail (Row 10) | Powers breadboard with 3.3 V from daphne |
| W5 | M-M | (−) Blue rail (Row 31) | (−) Blue rail (Row 33) | Bridges ground across power rail break |
| W6 | M-M | (+) Red rail (Row 31) | (+) Red rail (Row 33) | Bridges 3.3 V across power rail break |
| W7 | M-M | hestia pin 1 (GP0 TX) | Row A10 | Transmits 115200 8N1 UART telemetry from hestia |
| W8 | M-M | whattheflip pin 5 (GPIO5) | Row B10 | Probe 1: listens to clean UART messages on Row 10 |
| W9 | M-M | daphne pin 2 (GP1 RX) | Row A12 | Receives glitched UART messages on daphne |
| W10 | M-M | whattheflip pin 4 (GPIO4) | Row B12 | Probe 0: injects 10 µs UART bit flips into Row 12 |
| W11 | F-M | GY-511 sensor pin VCC | (+) Red rail (Row 40) | Powers LSM303DLHC sensor with 3.3 V |
| W12 | F-M | GY-511 sensor pin GND | (−) Blue rail (Row 41) | Grounds LSM303DLHC sensor |
| W13 | M-M | daphne pin 7 (GP5 SCL) or pin 10 (GP7) | Row A42 | Sends 400 kHz I2C clock from daphne master to sensor |
| W14 | F-M | GY-511 sensor pin SCL | Row B42 | Connects sensor clock into Row 42 |
| W15 | M-M | whattheflip pin 7 (GPIO7) | Row C42 | Probe 3: listens to SCL clock and tests 50 µs clock stretching |
| W16 | M-M | daphne pin 6 (GP4 SDA) or pin 9 (GP6) | Row A43 | Sends and receives I2C data between daphne and sensor |
| W17 | F-M | GY-511 sensor pin SDA | Row B43 | Connects sensor data into Row 43 |
| W18 | M-M | whattheflip pin 6 (GPIO6) | Row C43 | Probe 2: listens to sensor data and tests 50 µs ACK killing |
| W19 | M-M | whattheflip carrier pin 20 (GP15) | whattheflip carrier pin 19 (GP13) | Jumper routing SD CS to native GPIO21 |

## Interconnect Architecture

![hardware interconnect](reports/diagrams/hardware_interconnect.png)

## Who owns what

| Person | Subsystems | Key Invariants |
|---|---|---|
| Tan Yan Qi | Auto Discovery | 50 ms burst edge sampler; 16 valid rhythms before lock; remap < 10 ms. |
| Annie Goh | Fault Injection, Hestia, Daphne | Synchronized glitch triggers; 10 µs UART / 50 µs I2C; duty < 0.005%; Daphne 9-clock recovery. |
| Zayne Lee | Wireless Sniffer | Wi-Fi 6 promiscuous callback & 802.15.4 capture; time-sliced against Ch 6 SoftAP. |
| Ryan Ke | PCAP Storage | Delkin MicroSD 4 µs bitbang SPI; standard binary PCAP 2.4 off MBF_STORAGE. |
| Joseph Poon | Web UI, E-Stop, Iris | Hardware E-stop < 200 ns atomic float; 2 Hz WebSocket telemetry; Iris SoftAP & 802.15.4 TX. |

## Requirements Traceability

| ID | Requirement | Test | Expected Result |
|---|---|---|---|
| FR-1 | Passive sampling on all 4 pins, 1 µs edge resolution, 1 µA probe leakage | TC-1.1 | 1 µs tick timestamping; BER = 0.0% baseline; 0 V DC bus load. |
| FR-2 | Classify UART (115200) and I2C (400 kHz) within 500 ms, 95% accuracy, ±2% | TC-1.2 | Lock latency <= 500 ms; 16 valid edges; tolerance <= ±2%. |
| FR-3 | Remap any probe to any decoder in < 10 ms, no rewiring | TC-1.3 | Remap execution latency strictly < 10 ms. |
| FR-4 | Capture Wi-Fi Ch 1–13 and 802.15.4 Ch 11–26 with RSSI and timestamps | TC-2.1, TC-2.2 | Frames parsed into wtf_record_t with microsecond headers. |
| FR-5 | Faults bounded: bit flip 10 µs, ACK-kill & clock-stretch 50 µs, 5% duty | TC-3.1, TC-3.2 | Pulse width <= 10 µs (UART), <= 50 µs (I2C); duty <= 0.005%. |
| FR-6 | Targeted wireless injection strictly directed to Iris BSSID, never broadcast | TC-3.3 | Filter matches 02:00:00:00:00:03; zero broadcast drops. |
| FR-7 | E-stop floats probes in < 200 ns, background tasks settle within < 5 ms | TC-4.1 | Hardware atomic register clear in < 200 ns; task settle < 5 ms. |
| FR-8 | PCAP opens clean in Wireshark | TC-4.2 | Binary compatibility with PCAP 2.4 (Magic 0xA1B2C3D4, LinkType 147). |
| FR-9 | Live dashboard updates at 1 to 2 Hz | TC-5.1 | WebSocket JSON streaming rate bounded within [1.0 Hz, 2.0 Hz]. |
| FR-10 | Fault alert reaches dashboard in < 200 ms | TC-5.2 | Asynchronous alert dispatch latency < 200 ms. |

## Tasks & Concurrency Model

All firmware tasks run concurrently under µT-Kernel 3.0:

| Task | Priority | Stack | Function | Sync & IPC |
|---|:---:|:---:|---|---|
| TSK_SAFETY | 4 | 2048 B | Monitors GPIO9 active-low E-stop. Direct register write floats probes in < 200 ns. Drives WS2812 status LED. | FLG_SYSTEM |
| TSK_DASHBOARD | 6 | 4096 B | SoftAP (Ch 6, 192.168.4.1) and HTTP/WebSocket server on Port 80. Broadcasts telemetry at 2 Hz and alerts < 200 ms. | MBF_TELEMETRY (depth 8) |
| TSK_DISCOVERY | 7 | 3072 B | 50 ms burst edge sampler. Locks UART @ 115200 and I2C @ 400 kHz upon 16 valid edges; remaps decoders in < 10 ms. | MTX_PROBE_ACCESS, FLG_SYSTEM |
| TSK_SNIFFER | 8 | 3072 B | Passive UART1 on Probe 1 and I2C monitor on Probes 2 & 3. Dispatches wtf_record_t to storage and telemetry queues. | MBF_STORAGE, MBF_TELEMETRY |
| TSK_FUZZER | 9 | 3072 B | Synchronized fault injector. Injects 10 µs UART bit flips on Probe 0 and 50 µs I2C ACK-kills on Probe 2. Duty < 0.005%. | MTX_PROBE_ACCESS |
| TSK_STORAGE | 10 | 4096 B | Delkin MicroSD writer via 4 µs bitbang SPI (GPIO18..21). Writes binary PCAP 2.4 records off MBF_STORAGE. | MBF_STORAGE (depth 16) |
| TSK_TELEMETRY | 11 | 3072 B | Periodic status broadcaster at 1..2 Hz off MBF_TELEMETRY. | MBF_TELEMETRY (depth 8) |
| TSK_IDLE | 32 | 1024 B | Lowest priority WFI power-saver. | Native scheduler |

Victim tasks bind to cores with TA_ASSPRC: Hestia runs UART TX on Core 0; Daphne runs UART RX on Core 0 and GY-511 I2C master at 400 kHz on Core 1; Iris runs SoftAP and 802.15.4 bursts. Buffers come from 32 KB pool MPF_PACKETS (16 blocks of 2048 B, zero malloc).

![main flow](reports/diagrams/main_flow.png)

## Engine Flows

### Protocol Auto-Discovery
Tight 50 ms burst edge sampler measures shortest pulse widths. 16 consecutive valid rhythms lock UART 115200 and I2C 400 kHz within ±2%. Probe remap executes in < 10 ms.

![discovery flow](reports/diagrams/discovery_flow.png)

### Synchronized Fault Injection
Glitch injection synchronizes to bus transitions. Emits 10 µs UART bit flip on Probe 0 or 50 µs ACK-kill on Probe 2. Daphne detects corruption, updates BER, lights GP2/GP3 LEDs, beeps GP18 buzzer, and executes 9-clock I2C recovery.

![fuzzer flow](reports/diagrams/fuzzer_flow.png)

### Multi-Protocol Sniffer Engine
Passive wired eavesdropping on Probes 1..3 and time-sliced wireless 802.11 / 802.15.4 capture. Records stream to MicroSD PCAP storage and WebSocket dashboard.

![sniffer flow](reports/diagrams/sniffer_flow.png)

## Remaining Work Packages

### 1. Wireless Victim Firmware (Iris)
- [x] Implement Iris firmware on bare ESP32-C6 (`whattheflip/iris/`):
  - SoftAP beacon broadcast on Channel 6 (BSSID 02:00:00:00:00:03).
  - Periodic 802.15.4 frame transmitter on Channel 15.
- [x] Flash and verify over USB serial 8C:FD:49:0C:87:50.

### 2. Wireless Sniffer Engine (TSK_WIRELESS: FR-4, TC-2.1, TC-2.2)
- [x] Implement Wi-Fi 6 promiscuous callback filter (`esp_wifi_set_promiscuous`) on whattheflip.
- [x] Implement IEEE 802.15.4 sniffer (`esp_ieee802154`) on Channel 15.
- [x] Time-slicing state machine between SoftAP dashboard hosting (Ch 6) and sniffer hopping windows (1 to 2 s).
- [x] Stream wireless 802.11 / 802.15.4 frames into `MBF_STORAGE` for PCAP logging.

### 3. Targeted Wireless Fault Injection (TSK_FUZZER: FR-6, TC-3.3)
- [x] Implement targeted 802.15.4 channel-jam / stress bursts directed strictly at Iris BSSID (02:00:00:00:00:03), never broadcast.
### 4. Full Multi-Board Hardware Verification
- [ ] Connect Hestia and Daphne to USB ports.
- [ ] Run `uv run whattheflip/tests/verify_bench_traceability.py` across all physical ports to achieve 100% binary PASS across all test cases.
