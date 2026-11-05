# Engineering Handoff: Packet Tracer + Wireshark Hardware Workbench Specification

Date: 2026-11-05. Next session: implement Hestia/Daphne firmware polish (`UART0` collision fix + GPIO LED instrumentation) and build the `@xyflow/react` + `uPlot` + Wireshark Diagnostic Workbench.

## Suggested skills

- `impeccable` — enforcing the `Operate` mode Lab-Instrument visual specification, layout rhythm, and mechanical design audit (`impeccable detect --json`).
- `uv` — running `dashboard_bridge.py` and `verify_bench_traceability.py`.
- `playwright-cli` — verifying interactive node/wire/PDU selection and live layout at `http://localhost:8080`.
- `writing-for-agents` — keeping docs, comments, and UI copy direct and concrete.

---

## 1. Completed in This Session

### Physical Breadboard Re-Layout & Live Hardware Verification
- **Updated Wiring (`whattheflip/whattheflip.md` & `reports/diagrams/hardware_interconnect.puml/.png`)**:
  - **Clean UART (`Row 22` $\leftrightarrow$ `Row 59`)**: `hestia` `GP0` (`A22`) $\to$ $220\ \Omega$ resistor `R1` (`B22`–`B13`) & bridge `C22`–`C59` $\to$ `whattheflip` `GPIO5` (`Probe 1`, `A59`).
  - **Glitched UART (`Row 13` $\leftrightarrow$ `Row 60`)**: `R1` (`B13`) $\to$ `daphne` `GP1 RX` (`E13`) & bridge `D13`–`D60` $\to$ `whattheflip` `GPIO4` (`Probe 0`, `A60`).
  - **I2C Bus (`Row 55 SCL` / `Row 58 SDA`, Cols F–J)**:
    - `Row 55 (SCL)`: `whattheflip` `GPIO7` (`F55`), `daphne` `GP7` (`G55`), `GY-511` `SCL` (`I55`).
    - `Row 58 (SDA)`: `whattheflip` `GPIO6` (`F58`), `daphne` `GP6` (`G58`), `GY-511` `SDA` (`I58`).
  - **Power & Ground**: `daphne` `3V3_OUT` (`Row 61 (+)`) powering `GY-511` `VCC` (`Row 55 (+)`); common grounds on `(-)` rail (`hestia` `Row 13`, `daphne` `Row 43`, `GY-511` `Row 49`, `whattheflip` `Row 55`).
- **Live Silicon Verification**:
  - Verified live USB serial endpoints:
    - `whattheflip` (ESP32-C6): `/dev/cu.usbmodem101`
    - `hestia` (RP2040 Core 0 TX): `/dev/cu.usbmodemMTK3PICO1`
    - `daphne` (RP2040 Core 0 UART RX + Core 1 I2C1 Master): `/dev/cu.usbmodem4`
  - Executed `uv run whattheflip/tests/verify_bench_traceability.py --wtf-port /dev/cu.usbmodem101 --hestia-port /dev/cu.usbmodemMTK3PICO1 --daphne-port /dev/cu.usbmodem4`: **12/12 test cases PASS (100.0%)**.
- **Root-Cause Identified on Hestia**:
  - Every 5th packet in `hestia/hestia_uart.c` calls `tm_printf("[HESTIA:TX] PKT ...")`, which outputs onto `UART0` (`GP0`) and collides with raw telemetry frames (`[[HESTIA:TX] PKT HESTIA:126:FF81]`), inflating baseline BER on Daphne. Removing this `tm_printf` call restores a clean `0.000%` baseline BER.

---

## 2. Approved Product Concept: Cisco Packet Tracer + Wireshark Hardware Workbench

Combine **Cisco Packet Tracer's macro topology + PDU envelope inspection** with **Wireshark's micro packet filtering + bit-level protocol dissection** and **Saleae Logic's digital waveform reconstruction**. Every visual element maps 1-to-1 to real hardware signals across `whattheflip`, `hestia`, `daphne`, `iris`, and the `GY-511` IMU.

### A. Dual Operating Modes
1. **Real-Time Mode (Default)**:
   - Packets flow continuously along the topology edges (`Hestia -> R1 -> Daphne`, `GY-511 -> Daphne`, `Iris -> whattheflip`) while the Wireshark table streams frames and `uPlot` renders live `GY-511` sensor waveforms.
2. **Capture / Freeze Mode ("Bullet-Time" on Fault)**:
   - A $10\ \mu\text{s}$ UART bit-flip or $50\ \mu\text{s}$ I2C ACK-kill happens too fast for the human eye.
   - When `Auto-Freeze on Glitch` is active and a hardware fault is triggered (or the user presses `Space`), the workbench **captures and freezes the glitched PDU envelope directly on the wire (`Row 13` or `Row 55/58`)** and automatically opens its **Inbound (Pre-Glitch) vs. Outbound (Post-Glitch)** bit-by-bit comparison in the right-hand inspector.

### B. Interactive Use Cases

| # | User Interaction | Topology Canvas (`@xyflow/react`) | Inspector Drawer & Wireshark Table | Physical Bench Feedback (`hestia` / `daphne`) |
|:---:|---|---|---|---|
| **UC-A** | **Click a Device Node** (`Hestia`, `Daphne`, `whattheflip`, `Iris`, `GY-511`, `R1 220 Ω`) | Selects node, highlights connected pin handles (`GP0`, `A22`, `E13`, `GPIO4..7`), dims unrelated links. | - Auto-sets Wireshark filter (`node == daphne`, etc.).<br>- Opens **Device Internals**: live $\mu\text{T-Kernel}$ 3.0 tasks, RP2040 SMP Core 0 vs. Core 1 split, `FUNCSEL` registers, and live controls (e.g. Hestia TX rate `1/2/10 Hz` & pattern `Canonical`/`0xAA`/`0x55`). | - Target board LEDs reflect active rate/mode immediately. |
| **UC-B** | **Click a Breadboard Wire / Bus** (`Row 22 Clean UART`, `Row 13 Glitched UART`, `Row 55/58 I2C`, `2.4 GHz RF`) | Highlights physical bus path and breadboard tie-point badges (`A22-B22-C59`, `B13-D13-E13-A60`). | - Filters Wireshark table (`bus == uart.row13` or `bus == i2c1`).<br>- Renders **Digital Logic Waveform** ($8.68\ \mu\text{s}$ UART 8N1 bit cells or $400\text{ kHz}$ I2C clock/data timing). | - Displays exact physical jumper mapping for viva verification. |
| **UC-C** | **Click a Packet Envelope (`PDU`) or Wireshark Row** | Freezes/highlights that PDU on its wire and traces its path across `R1 (220 Ω)`. | Opens **Packet Tracer PDU Inspector (Inbound vs. Outbound)**:<br>- **Inbound (`Row 22` / `Probe 1` Clean)**: Transmitted ASCII, Hex, and 8N1 bits.<br>- **Outbound (`Row 13` / `Daphne GP1` Post-Glitch)**: Received ASCII, Hex, and bits with **flipped bits highlighted in red**, checksum validation (`Expected ~counter` vs `Received`), and contention math ($3.3\text{ V} / 220\ \Omega = 15.0\text{ mA}$ for $10\ \mu\text{s}$). | - Indicates which physical fault LED (`Daphne GP3` UART or `GP2` I2C) and buzzer (`GP18`) triggered. |
| **UC-D** | **Move / Tilt Physical `GY-511` IMU** | `GY-511` node updates its live 3-axis tilt indicator (`X, Y, Z mg`) and streams cyan I2C envelopes (`0x19` Accel, `0x1E` Mag) along `Row 55/58` to `Daphne`. | - `uPlot` scope plots **Raw I2C Sample vs. Daphne's 8-Sample Moving Average** at 60 FPS.<br>- Expanding an I2C frame in Wireshark shows live raw two's-complement register bytes (`0xA8`: `[XL, XH, YL, YH, ZL, ZH]`). | - **Daphne's Maker Pi Base blue LEDs (`GP8`–`GP15`)** act as a live **8-LED physical tilt/spirit level** sliding left/right as you tilt the breadboard. |
| **UC-E** | **Trigger Hardware Fault** (`10 µs UART Flip`, `50 µs I2C ACK-Kill`, `50 µs Stretch`, `802.15.4 Jam`) | Sends hardware command to `whattheflip`, animates red glitch pulse down `GPIO4`/`GPIO6`/`GPIO7`, and freezes the glitched PDU on the wire. | - Logs red `[GLITCHED / CRC FAIL]` or `[I2C NACK -> 9-CLK RECOVERY]` row.<br>- Displays recovery time ($< 1\text{ ms}$) and duty cycle ($\le 0.005\% \le 5\%$). | - **Daphne `GP3` / `GP2` blue LED** lights.<br>- **Daphne `GP18` piezo buzzer** chirps at 2 kHz. |
| **UC-F** | **Trigger `GPIO9` E-Stop** (Physical `BOOT` button or UI button) | All 4 probe wires from `whattheflip` turn into dashed grey open-circuit lines (`HIGH-Z < 200 ns`); fault controls lock out. | - Logs `GPIO.enable_w1tc` atomic float ($\sim 6.25\text{ ns} < 200\text{ ns}$) and task settle ($412\ \mu\text{s} < 5\text{ ms}$). | - Hardware probes float immediately; `Re-Arm` restores operation. |

### C. Edge Cases to Handle
1. **Clean Baseline BER (`0.000%`)**: Strip the colliding `tm_printf` from Hestia's `UART0` loop so zero packets corrupt unless `whattheflip` physically injects a fault.
2. **High-Rate Traffic Throttling & Virtualization**:
   - Cap visual animated envelopes on `@xyflow/react` edges to $\le 4$ active envelopes per link ($600\text{ ms}$ transit duration) so the canvas never drops below 60 FPS.
   - Keep a rolling ring buffer of `500` frames in browser memory (full capture goes to `capture.pcap`), and pause table auto-scroll whenever the user hovers or selects a row.
3. **Disconnected / Resetting Boards**:
   - Display explicit per-node hardware status badges (`ONLINE` emerald, `DISCOVERING` amber, `OFFLINE / EMULATED` slate).
   - When `whattheflip` resets, animate the boot auto-discovery sequence (`50 ms` high-Z strapping guard $\to$ `TSK_DISCOVERY` edge-sampler $\to$ `GPIO Matrix Remap < 10 ms` $\to$ `LOCKED`).
4. **Wedged I2C Slave State Machine**:
   - Surface Daphne Core 1's bare-metal `DW_apb_i2c` disable $\to$ `GPIO_FUNC_SIO` switch $\to$ 9-clock `SCL` pulse + `STOP` condition recovery sequence in both the topology node badge and the PDU inspector.

---

## 3. Tech Stack & Library Architecture

### A. Frontend Workbench (`whattheflip/frontend`)
- **Topology Graph & Animated Packet Links**: **`@xyflow/react` (React Flow v12)** (`react`, `react-dom`, `@xyflow/react`)
  - Custom PCB/Hardware node components (`BoardNode`, `ResistorNode`, `SensorNode`) with exact pin handles (`GP0`, `GP1`, `GP6`, `GP7`, `GPIO4..7`, `A22`, `E13`, `F55`, `F58`).
  - Custom animated SVG edges (`PacketEdge`) using `<BaseEdge>` + `<animateMotion>` to glide clickable packet envelopes (`✉ #126`) along orthogonal breadboard wires.
- **High-Speed Oscilloscope & DSP Plot**: **`uPlot`**
  - Renders 60 FPS live time-series traces comparing `GY-511` **Raw I2C Register Reads vs. 8-Sample Moving Average** and bus latency.
- **Digital Logic Bit-Cell Waveform**: **Custom SVG Timing Renderer (`WaveDrom` style)**
  - Draws exact $8.68\ \mu\text{s}$ UART `8N1` bit cells (`START`, `D0..D7` LSB-first, `STOP`) and $400\text{ kHz}$ I2C `SCL`/`SDA` waveforms, overlaying the red $10\ \mu\text{s}$ / $50\ \mu\text{s}$ glitch pulse window.
- **Icons & Build**: **`lucide-react`** + **`esbuild`**.

### B. Dual-Tier Deployment (Solving ESP32-C6 512 KB SRAM Limits)
1. **Primary Bench Host (`uv run whattheflip/dashboard_bridge.py` on `localhost:8080`) / MicroSD `/WWW/`**:
   - Python bridge connects concurrently to `/dev/cu.usbmodem101` (`whattheflip`), `/dev/cu.usbmodemMTK3PICO1` (`hestia`), `/dev/cu.usbmodem4` (`daphne`), and `/dev/cu.usbmodem11301` (`iris`).
   - Pairs Probe 1's clean pre-resistor UART frames with Daphne's post-resistor RX frames by packet counter so the UI has true differential **Inbound vs. Outbound** byte data.
   - Serves the full `@xyflow/react` + `uPlot` + Wireshark Workbench with zero MCU RAM limits (and static assets can also live in `/WWW/` on the 32 GB FAT32 MicroSD card).
2. **On-Chip ESP32-C6 SoftAP (`192.168.4.1` in `wtf_dashboard.c`)**:
   - Streams static assets from MicroSD `/WWW/` or compact Flash fallback so `FR-9` and `FR-10` pass with zero additional SRAM pressure.

### C. Victim Firmware Upgrades (`hestia` & `daphne`)
- **`whattheflip/hestia/hestia_uart.c`**:
  - Remove `tm_printf` on `UART0` (`GP0`) during canonical packet transmission to eliminate the every-5th-packet collision.
  - Configure Maker Pi Base GPIO LEDs (`GP2`–`GP9`) via RP2040 `SIO` to sweep/pulse on every transmitted frame.
- **`whattheflip/daphne/daphne_i2c.c` & `daphne_uart.c`**:
  - Drive Maker Pi Base GPIO LEDs (`GP8`–`GP15`) via `sio_hw` as an **8-LED physical tilt meter** mapped to the `GY-511` X-axis accelerometer reading.
  - Emit raw hex register bytes (`[XL XH YL YH ZL ZH]`) alongside raw and 8-sample moving-average $\text{mg}$ values so the Wireshark dissector and `uPlot` scope can display both raw I2C bytes and filtered output.

---

## 4. Impeccable Design & Feel Specification (`Operate` Mode)

### A. Layout Architecture (`100vh` Zero-Scroll Split Workbench)
- **Top Command & Transport Bar (`48px` height)**:
  - Brand & safety badge (`ARMED` / `INJECTING` / `E-STOP ABORTED`).
  - **Packet Tracer Transport**: `▶ Live`, `⏸ Pause`, `⚡ Auto-Freeze on Glitch` toggle, `Step`.
  - **Hardware Fault Injectors**: `⚡ 10µs UART Bit-Flip (Row 13)`, `⚡ 50µs I2C ACK-Kill (Row 58)`, `⚡ 50µs I2C Stretch (Row 55)`, `📡 802.15.4 Jam (Ch 15)`.
  - **Hestia TX Control**: Rate (`1 Hz`, `2 Hz`, `10 Hz`) & Mode (`Canonical`, `0xAA`, `0x55`).
  - **Safety & Export**: `[Reset Baseline]`, `[🛑 EMERGENCY STOP]`, `[Download .pcap]`.
- **Upper Left — Interactive Hardware Topology Canvas (`58% width × 52% height`)**:
  - `@xyflow/react` dark schematic canvas (`#090D14` background with `16px` `#1E293B` dot grid).
  - Nodes: `Hestia (RP2040)`, `R1 (220 Ω)`, `Daphne (RP2040 SMP)`, `GY-511 IMU (0x19/0x1E)`, `whattheflip (ESP32-C6)`, `Iris (ESP32-C6 RF)`.
- **Upper Right — Contextual PDU & Hardware Inspector (`42% width × 52% height`)**:
  - Tabs:
    1. **PDU Bit-Diff & Logic Scope**: Inbound (`Row 22`) vs. Outbound (`Row 13`) byte/bit comparison + $8.68\ \mu\text{s}$ bit-cell waveform + Ohm's law contention readout ($15.0\text{ mA}$).
    2. **Device & RTOS Internals**: Selected board's $\mu\text{T-Kernel}$ 3.0 tasks, SMP Core 0/1 allocation, `MPF`/`MBF`/`FLG` status, and GPIO Matrix crossbar (`FR-3`).
    3. **GY-511 I2C Register & DSP Scope**: Live `0xA8` register byte decode + `uPlot` Raw vs. 8-Sample Moving Average chart + 9-clock recovery counter.
- **Bottom — Wireshark Filter & Multi-Protocol Dissector (`100% width × remaining height`)**:
  - **Filter Bar**: Monospace expression input (`node == daphne && uart.corrupt == 1`) + quick-filter chips (`All`, `UART`, `I2C`, `Wi-Fi 6`, `802.15.4`, `Faults / Glitches Only`).
  - **Left (62%)**: Virtualized high-density packet list (`No.`, `Time (µs)`, `Src -> Dst`, `Bus/Row`, `Proto`, `Len`, `Decoded Summary & Checksum`).
  - **Right (38%)**: Canonical 16-byte aligned Hex + ASCII + Bit-level inspector.

### B. Color Tokens (Restrained Lab Instrument Palette)

| Token | Hex | Semantic Role |
|---|---|---|
| `--bg-canvas` | `#090D14` | Topology schematic canvas background |
| `--bg-panel` | `#0F141F` | Inspector and Wireshark panel background |
| `--bg-elevated` | `#161D2B` | Hardware node cards, toolbar, table headers |
| `--border-subtle` | `#1E293B` | `1px solid` crisp panel and node borders |
| `--fg-primary` | `#F1F5F9` | Primary labels, active values, decoded ASCII |
| `--fg-muted` | `#94A3B8` | Pin numbers, breadboard coordinates, units |
| `--proto-uart` | `#10B981` | Clean UART 115200 traffic (`Row 22` / valid frames) |
| `--proto-i2c` | `#06B6D4` | 400 kHz I2C sensor traffic (`Row 55/58` / `GY-511`) |
| `--proto-rf` | `#A855F7` | Wi-Fi 6 & IEEE 802.15.4 frames (`Iris`) |
| `--state-glitch` | `#F43F5E` | Hardware glitch pulse, flipped bits, CRC fail, E-Stop |
| `--state-warn` | `#F59E0B` | $220\ \Omega$ contention resistor, I2C 9-clock recovery |

### C. Typography & Anti-Patterns
- **Fonts**: System UI Sans (`Inter` / `-apple-system`) for labels/controls; `JetBrains Mono` / `SF Mono` with `font-variant-numeric: tabular-nums` for every number, timestamp, pin, register, and hex byte so fast updates never jitter horizontally.
- **Banned Slop**: No marketing hero banners, no neon glassmorphism, no multi-axis decorative 3D toys, no unaligned hex dumps, and no scrolling past `100vh`.
