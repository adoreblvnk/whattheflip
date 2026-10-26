# whattheflip Diagnostic & Fuzzing Tool

µT-Kernel 3.0 (IEEE 2050-2018) embedded security diagnostic, sniffer, and fault injection firmware running natively on the **ESP32-C6** (RISC-V single-core RV32IMAC @ 160 MHz) mounted on the Cytron Maker Pi Base carrier.

---

## Subsystem Architecture & Tasks

| Task | Priority | Stack | Function | Requirement |
|---|---|---|---|---|
| **TSK_SAFETY** | 4 | 2048 B | Hardware E-stop on GPIO9 (BOOT button). Direct register write floats Probes 0..3 in $< 200\text{ ns}$. Latches `FLG_SAFETY_ABORT`. | FR-7 (TC-4.1) |
| **TSK_DISCOVERY** | 7 | 3072 B | High-speed edge rhythm classification. Measures bit periods; locks UART @ 115200 baud and I2C @ 400 kHz upon 16 consecutive valid edges. | FR-2 (TC-1.2) |
| **TSK_SNIFFER** | 8 | 3072 B | Passive hardware UART1 listener on Probe 1 (GPIO5). Dispatches framed records to storage and telemetry message buffers. | FR-1 (TC-1.1) |
| **TSK_FUZZER** | 9 | 3072 B | Controlled hardware fault injector. Emits 10 µs bit flips on Probe 0 (Row 12) and 50 µs ACK kills on Probe 2 (Row 43). Duty cycle $< 0.005\% \ll 5\%$. | FR-5 (TC-3.1, TC-3.2) |
| **TSK_STORAGE** | 10 | 4096 B | Delkin USDCOEM-32GB MicroSD writer via software bitbang SPI (GPIO18..21). Writes standard PCAP format off `MBF_STORAGE` (depth 16). | FR-8 (TC-4.2) |
| **TSK_TELEMETRY** | 11 | 3072 B | Periodic status broadcaster at 2 Hz off `MBF_TELEMETRY` (depth 8). Emits packet counters and fault alert events. | FR-9 (TC-5.1), FR-10 (TC-5.2) |
| **TSK_IDLE** | 32 | 1024 B | Lowest priority WFI power-saver; ensures scheduler invariant when all tasks sleep. | System Invariant |

---

## Physical Probe & Pin Assignments

| Pin | Identifier | Breadboard Node | Function |
|---|---|---|---|
| **GPIO4** | Probe 0 | Row B12 | Fault Injection: 10 µs glitch into Daphne GP1 RX (220 Ω series limited). |
| **GPIO5** | Probe 1 | Row B10 | Clean Sniffer: Passive UART1 listening to Hestia GP0 TX at 115200 baud. |
| **GPIO6** | Probe 2 | Row C43 | I2C SDA Sniffer / 50 µs ACK-kill injector. |
| **GPIO7** | Probe 3 | Row C42 | I2C SCL Sniffer / clock stretch monitor. |
| **GPIO9** | E-Stop | Onboard Button | Hardware negative-edge interrupt; immediately floats all probes in $< 200\text{ ns}$. |
| **GPIO18..21** | MicroSD SPI | Slot | SCK (18), MOSI (19), MISO (20), CS (21, jumpered from GP15). Bitbang SPI. |

---

## Building and Flashing

### 1. Build Firmware Binary
```bash
make -C whattheflip/whattheflip
```
Generates `build/whattheflip.bin`.

### 2. Flash to Device (Zero Physical Buttons Required)
```bash
make -C whattheflip/whattheflip flash
```
Flashing is fully automated over on-chip USB Serial/JTAG (`/dev/cu.usbmodem101`).

---

## Hardware Verification Telemetry

Live output captured from `whattheflip` (`/dev/cu.usbmodem101`):
```text
[mtkernel] __wrap_esp_startup_start_app: bypassing FreeRTOS
[mtkernel] heap 0x4080fd48..0x40817d48 (32768 bytes)
[mtkernel] calling knl_main()

microT-Kernel Version 3.00

============================================================
  whattheflip - Multi-Protocol Diagnostic & Fuzzing Tool
  µT-Kernel 3.0 (IEEE 2050-2018) on ESP32-C6 RISC-V
============================================================
[INIT] Kernel objects created (MPF_PACKETS, MBF_STORAGE, MTX_PROBE_ACCESS)
[SAFETY] TSK_SAFETY online: E-stop monitored on GPIO9 (200 ns gate)
[DISCOVERY] TSK_DISCOVERY online: Monitoring probes for edge rhythms (FR-2)
[SNIFFER] TSK_SNIFFER online: Passive UART1 listening on GPIO5 (Probe 1)
[FUZZER] TSK_FUZZER online: Bounded fault injector (10 µs UART, 50 µs I2C, FR-5)
[STORAGE] TSK_STORAGE online: MicroSD bitbang SPI (GPIO18..21, card_ready=1, FR-8)
[TELEMETRY] TSK_TELEMETRY online: Periodic broadcaster at 1..2 Hz (FR-9, FR-10)
[DASHBOARD:2Hz] Telemetry: UART_pkts=20 | I2C_pkts=0 | Alerts=0 | Safety=ARMED
[FUZZER] INJECT: 10 µs UART bit flip on Row 12 (Probe 0, duty=0.0005%)
[STORAGE] PCAP sync: 30 records buffered (latest proto=1, len=16, 12536196 us)
```

Simultaneously observed on Daphne (`/dev/cu.usbmodemMTK3PICO1`) receiving the injected glitch:
```text
[DAPHNE:UART] CORRUPT FRAME: len=32 raw=[[HESTIA:TX] PKTÿHESTIA:9146:DC45]
[DAPHNE:UART] PKT #9147 VALID (total=13989, valid=12155, corrupt=1834)
```
- Proves end-to-end fault injection and victim detection with strict duty cycle bounding.
