# Engineering Handoff: whattheflip

Date: 2026-10-27. Next session: physical full-bench bring-up, then report/viva prep.

## Suggested skills

- `uv` — running the traceability suite and any Python tooling (`uv run`).
- `playwright-cli` — verifying the dashboard frontend at http://localhost:8080/.
- `web-quality-audit` — if the UI polish backlog is picked up this session.
- `writing-for-agents` — wording for any report, comment, or doc written next.

## 1. Completed

### Hardware & firmware

- whattheflip (ESP32-C6 @ 160 MHz, `8C:FD:49:0D:18:40`):
  - Flash partition table expanded to 2 MB app partition (`partitions.csv`).
  - Multi-protocol sniffer (`wtf_sniffer.c`): passive UART1 eavesdropping on Probe 1 (115200 8N1), Wi-Fi 6 promiscuous callback filter, IEEE 802.15.4 frame receiver on Ch 15.
  - Single-radio time-slicing state machine alternating between SoftAP Ch 6 hosting and target channel hopping.
  - `wtf_fuzzer.c` converted from runaway loop to demand-driven triggers (`s_fuzzer_auto_mode = false`); pushes `PROTO_FAULT_ALERT` to `g_mbf_telemetry` on all injections.
  - Targeted 802.15.4 jam burst (`inject_wireless_target_jam_50us()`) restricted strictly to the Iris BSSID (`02:00:00:00:00:03` / `0x0003`, FR-6).
  - Direct USB-Serial/JTAG FIFO register reading (`USB_SERIAL_JTAG_EP1_REG`) in `wtf_safety.c`; added `wtf_safety_rearm()` (c=`R`).
  - Flashed and verified live on `/dev/cu.usbmodem101`.
- iris (bare ESP32-C6, `8C:FD:49:0C:87:50`):
  - Complete µT-Kernel 3.0 firmware in `whattheflip/iris/`, 2 MB app partition, static task stacks (`TA_USERBUF`, zero malloc).
  - Hosts Wi-Fi SoftAP on Ch 6 (`iris-victim`, BSSID `02:00:00:00:00:03`).
  - Emits periodic IEEE 802.15.4 data frames on Ch 15 (2 Hz, PAN `0x1234`, short addr `0x0003`).
  - Flashed and verified live on `/dev/cu.usbmodem11301`.
- daphne & hestia (RP2040 dual-core SMP, `mtk3smp-rp2040`): rebuilt clean `daphne.uf2` and `hestia.uf2`.

### Tooling, tests & dashboard

- Tooling standardized on uv: PEP 723 inline dependency metadata (pyserial) added to `dashboard_bridge.py` and `verify_bench_traceability.py`.
- V-Model traceability suite (`whattheflip/tests/verify_bench_traceability.py`): added TC-2.1 (Wi-Fi 6 promiscuous), TC-2.2 (802.15.4 sniffing), TC-3.3 (targeted wireless jam). Passes 12/12 test cases (100.0%) on physical silicon; exports `whattheflip/tests/TRACEABILITY_MATRIX.md`.
- Clean-room frontend (`whattheflip/frontend/`): VanJS + vanilla CSS + esbuild + gzip pipeline (7.10 KB gzipped, 7,271 bytes in flash), embedded in `wtf_dashboard.c` as `dashboard_html.h` with `Content-Encoding: gzip`.
  - Wireshark-style display filter toolbar (All Protocols / UART / I2C / Wi-Fi / Zigbee / Faults dropdown + text filter + Clear).
  - Contextual 4-scenario card (UART → I2C → Wi-Fi → Zigbee).
  - Persistent red [EMERGENCY STOP] button in header; turns green [Re-Arm System] on abort.
  - Aligned 48-character padded hex dump + ASCII inspector.
  - Verified live via Playwright CLI at http://localhost:8080/.

## 2. Current device endpoints

| Device      | Silicon / role             | Port                      | Hardware ID                            | Status              |
|-------------|----------------------------|---------------------------|----------------------------------------|---------------------|
| whattheflip | ESP32-C6 diagnostic        | `/dev/cu.usbmodem101`     | MAC `8c:fd:49:0d:18:40`                | Online / flashed    |
| iris        | Bare ESP32-C6 victim       | `/dev/cu.usbmodem11301`   | MAC `8C:FD:49:0C:87:50`, BSSID `02:00:00:00:00:03` | Online / flashed |
| daphne      | RP2040 victim B (I2C+UART) | `/dev/cu.usbmodemMTK3PICO1` | Chip `E66480454F73A231`              | Built (`daphne.uf2`) |

## 3. What needs to be done

1. Physical full-bench wiring & test.
   - Plug daphne and hestia into USB ports (flash the `.uf2` files if wiped).
   - Run the automated harness across all 4 devices:
     ```bash
     uv run whattheflip/tests/verify_bench_traceability.py --output-markdown
     ```
     Output lands in `whattheflip/tests/TRACEABILITY_MATRIX.md`.
2. MicroSD native SPI migration (optional / stretch).
   - Currently 4 µs software bitbang SPI on GPIO18..21. Evaluate native ESP-IDF SPI master with GPIO21 CS if PCAP write throughput requires optimization.
3. Reports & viva prep.
   - Final report write-up integrating verified bench numbers and updated PlantUML diagrams (`reports/diagrams`).
   - Statechart update to include FR-6 targeted wireless injection and E-Stop exit arcs.

## 4. UI polish backlog (later)

- Design tweaks: refine typography, padding, borders for the editorial aesthetic.
- Filter dropdown styling: custom-styled select matching the brwne parchment theme instead of the browser default.
- Packet search autocomplete: quick filter tokens (`proto:uart`, `len>20`, `src:iris`).
- Visual fault feedback: real-time waveform/timing visualizer or packet delta diff view when a fault fires.
