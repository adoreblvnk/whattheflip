# Design Review: whattheflip

**Module:** INF2004 Embedded Systems Programming  
**Group ID:** SE06 (Lab P1)  
**Project Title:** whattheflip — Embedded Multi-Bus Sniffer, Wireless Monitor & Chaos Fuzzer  
**Target Hardware:** 2 × ESP32-C6 RISC-V @ 160 MHz (diagnostic tool + wireless victim) + Raspberry Pi Pico (RP2040) wired victim; GY-511 I2C sensor target  
**Operating System:** $\mu\text{T-Kernel}$ 3.0 (`esp32c6-mtk3`)  
**Submission Milestone:** Week 6 Architecture Freeze  

---

## Purpose

Document the system requirements, hardware connections, RTOS task structure, behavioural models, assumptions, and AI-supported design reasoning before major implementation. Treat this Week 6 submission as an architecture freeze before full integration and Tribunal 1. The design may change later, but major changes must be justified with engineering evidence.

---

## Team Role and Subsystem Ownership

| Buddy       | Student Name | Student ID      | Primary Subsystem / Responsibility                                                                                                                                                                                                              |
| ----------- | ------------ | --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Buddy 1** | Tan Yan Qi   | *Student Entry* | **Wired Protocol Auto-Discovery Engine:** 4-pin signal timing acquisition, rise-time measurement, UART/I2C/SPI classification, and dynamic pin role mapping.                                                                                    |
| **Buddy 2** | Annie Goh    | *Student Entry* | **Physical Chaos & Fault Injection Engine:** Frame-synchronized glitch injection (bit-flips, I2C ACK suppression, clock stretching), bus contention protection, and hardware E-stop driver release.                                             |
| **Buddy 3** | Zayne Lee    | *Student Entry* | **Wireless Multi-Protocol Sniffer Engine:** ESP32-C6 2.4 GHz promiscuous radio capture for Wi-Fi 6 (802.11ax) management frames and IEEE 802.15.4 (Zigbee 3.0), extracting RSSI, channel, and timestamps.                                       |
| **Buddy 4** | Ryan Ke      | *Student Entry* | **Telemetry Ingestion & PCAP Storage Engine:** Zero-copy fixed memory ring buffers, packet serialization into standard `libpcap` format (`.pcap`), and high-speed FAT32 MicroSD card logging.                                                   |
| **Buddy 5** | Joseph Poon  | *Student Entry* | **System Supervisor, Web UI & Victim Control:** $\mu\text{T-Kernel}$ supervisor coordination, WebSocket live streaming dashboard over local Wi-Fi, hardware E-stop interrupt handling, NeoPixel/buzzer indicators, and victim stimulus control. |

---

## 1. Requirements

### 1.1 Functional Requirements (FR)

| ID | Requirement Statement |
|---|---|
| **R-1.1** | The system shall passively acquire digital signals across 4 probe pins with an input leakage current $\le 1\,\mu\text{A}$ ($R_{in} \ge 1\text{ M}\Omega$) and a timing resolution $\le 1\,\mu\text{s}$. |
| **R-1.2** | The system shall classify UART ($9600\text{ to }115200\text{ baud} \pm 2\%$), I2C ($100\text{ kHz} \pm 2\%$ and $400\text{ kHz} \pm 2\%$), and SPI (clock rates up to $1\text{ MHz}$) signals within $\le 500\text{ ms}$ of continuous bus activity, with a classification accuracy $\ge 95\%$. |
| **R-1.3** | The system shall remap any of the 4 probe pins to any internal protocol decoding channel without physical rewiring, with a remap-to-ready latency $\le 10\text{ ms}$. |
| **R-2.1** | The system shall capture 802.11 management frames (beacons, probe requests) and IEEE 802.15.4 frames across 2.4 GHz channels 11–26, extracting received signal strength (RSSI, $\pm 3\text{ dB}$), channel ID, and timestamps ($\pm 1\,\mu\text{s}$). |
| **R-3.1** | When armed by the user, the system shall inject bounded protocol faults into the target bus — single bit-inversions, I2C ACK $\to$ NACK suppressions, and clock stretching up to $50\,\mu\text{s} \pm 10\%$ — with each active drive bounded to $\le 50\,\mu\text{s}$ before releasing the line. |
| **R-3.2** | When armed by the user, the system shall transmit over-the-air stress frames (including 802.11 deauthentication frames) targeting the testbed wireless node, within $\le 100\text{ ms}$ of the arm command. |
| **ER-1** | On actuation of the emergency stop input, the system shall disarm all active output drivers and return all probe lines to high-impedance (High-Z) within $\le 5\text{ ms}$. |
| **R-4.1** | The system shall record captured packets, each with a timestamp ($\pm 1\,\mu\text{s}$) and raw packet bytes, to non-volatile removable media in a format importable by standard packet-analysis tooling. |
| **R-5.1** | The system shall broadcast decoded packet summaries and bus error statistics over a local wireless interface to connected client dashboards at a refresh period of $500\text{ ms to }1000\text{ ms}$ ($\pm 10\%$), and shall issue a fault-state alert within $\le 200\text{ ms}$ of the fault event. |

### 1.2 Non-Functional Requirements (NFR)

| ID | Requirement Statement |
|---|---|
| **NFR-1** | The emergency stop and safety disarm path shall preempt all background processing within $\le 5\text{ ms}$ of switch contact. |
| **NFR-2** | The firmware shall perform zero dynamic heap allocations during active monitoring and fault-injection phases (0 allocations measured over a $\ge 10\text{ min}$ continuous run). |
| **NFR-3** | The capture pipeline shall sustain a $\le 0.1\%$ packet drop rate for continuous wired streams up to $500\text{ kbps}$, and shall buffer wireless bursts of $\ge 32$ consecutive frames without loss. |
| **NFR-4** | The probe interface shall tolerate continuous input voltages up to $5.5\text{ V} \pm 5\%$ without presenting more than $3.6\text{ V}$ to any microcontroller pad. |
| **NFR-5** | Fault-injection pulses shall synchronize to target bus trigger edges with a timing jitter of $< 500\text{ ns}$. |

---

## 2. Hardware Integration Pin Layout

### 2.1 Hardware Interconnect & Maker Pi Pico Base Integration

The diagnostic tool (Node 1) consists of the **Waveshare ESP32-C6-Pico-M** mounted directly onto the **Cytron Maker Pi Pico Base** carrier board. This integration provides three major hardware advantages:
1. **Integrated MicroSD Card Socket:** Utilizes the onboard spring-push MicroSD slot hardwired to physical pins 14–16, eliminating loose external SPI breakout modules.
2. **Per-Pin Real-Time LED Diagnostics:** Every GPIO header pin on the Maker Pi Base features an onboard blue LED indicator, providing immediate physical visual feedback when probe lines and SPI buses toggle.
3. **Electrical Safety Invariant (I2C Bus Protection):** On the ESP32-C6-Pico, physical pins 26 and 27 connect to native `GPIO22` (SDA) and `GPIO23` (SCL), which drive the onboard TCA9554PWR expander. The Maker Pi base exposes its onboard user buttons on the header positions mapped to `GP20`, `GP21`, and `GP22`; on this carrier those nets are tied to the expander/I2C domain. The team has therefore mandated using the **native `BOOT` pushbutton on Pin 12 (`GPIO9`)** for Emergency Stop, and leaves all three Maker Pi base user buttons (`GP20`/`GP21`/`GP22`) unused so none can short or contend the expander's I2C bus.

**Figure 1: Hardware Interconnect & Maker Pi Base Pinout**
![Figure 1: Hardware Interconnect & Maker Pi Base Pinout](diagrams/hardware_interconnect.png)

### 2.2 Pin Allocation Table (Maker Pi Base Carrier Integration)

*Note: All 4 probe pins are dynamically shared between `AutoDiscovery_Task` (listening mode) and `Chaos_Fuzzer_Task` (injection mode), arbitrated via a mutex (`MTX_PROBE_ACCESS`). Mounting the ESP32-C6 on the Maker Pi Base maps physical header pins to native high-speed GPIOs for sniffing, while utilizing the onboard SD card socket and WS2812 LED.*

| Maker Pi Pin | Signal Name | ESP32-C6 Pin | Interface Type | Owning Task / Arbitration | Electrical & Hardware Notes |
|:---:|---|:---:|---|---|---|
| **Pin 1 (GP0)** | `PROBE_0` | **GPIO16** | GPIO / Edge IRQ | Shared (`AutoDiscovery` / `Fuzzer`) | High-speed native GPIO. Level-shifted to Wired Victim GP0. Baseboard blue LED indicator. |
| **Pin 2 (GP1)** | `PROBE_1` | **GPIO17** | GPIO / Edge IRQ | Shared (`AutoDiscovery` / `Fuzzer`) | High-speed native GPIO. Level-shifted to Wired Victim GP1. Baseboard blue LED indicator. |
| **Pin 4 (GP2)** | `PROBE_2` | **GPIO4** | GPIO / Edge IRQ | Shared (`AutoDiscovery` / `Fuzzer`) | High-speed native GPIO. Level-shifted to Wired Victim GP4 (SDA). Baseboard blue LED indicator. |
| **Pin 5 (GP3)** | `PROBE_3` | **GPIO5** | GPIO / Edge IRQ | Shared (`AutoDiscovery` / `Fuzzer`) | High-speed native GPIO. Level-shifted to Wired Victim GP5 (SCL). Baseboard blue LED indicator. |
| **Pin 14 (GP10)** | `SD_SCK` | **GPIO18** | Hardware SPI | `Storage_Task` | **Hardwired to Maker Pi onboard MicroSD slot**. 10 MHz SPI Clock. |
| **Pin 15 (GP11)** | `SD_MOSI` | **GPIO19** | Hardware SPI | `Storage_Task` | **Hardwired to Maker Pi onboard MicroSD slot**. SPI Master Out Slave In. |
| **Pin 16 (GP12)** | `SD_MISO` | **GPIO20** | Hardware SPI | `Storage_Task` | **Hardwired to Maker Pi onboard MicroSD slot**. SPI Master In Slave Out. |
| **Pin 20 (GP15)** | `SD_CS` | **EXIO7** | IO Expander / CS | `Storage_Task` | **Hardwired to Maker Pi onboard MicroSD slot**. Active-Low Chip Select via TCA9554. |
| **Pin 12 (GP9)** | `E_STOP_BTN` | **GPIO9** | GPIO Input (Pull-Up)| `Safety_Supervisor_Task` | **Onboard BOOT pushbutton**. Active-Low, $\le 5\text{ ms}$ High-Z interrupt (avoids I2C short). |
| **Pin 11 (GP8)** | `STATUS_LED` | **GPIO8** | WS2812 / DIN | `Safety_Supervisor_Task` | **Onboard WS2812 Addressable RGB LED** (Green: Ready, Blue: Sniff, Red: Fault). |
| **Pin 24** | `BUZZER_OUT` | **EXIO3** | PWM / Audio | `Safety_Supervisor_Task` | Maker Pi Base onboard piezo buzzer, driven via the TCA9554 expander (EXIO3). |
| **Pins 26 / 27** | `EXP_I2C` | **GPIO22 / 23** | I2C Master Bus | System Init | Dedicated to onboard TCA9554PWR expander. **Baseboard Buttons 1 & 2 must NOT be pressed**. |
| **Pin 36 (3V3)** | `VCC_LV` | **3V3 OUT** | Power Output | Hardware Interconnect | 3.3V reference power to low-voltage side of TXS0104E level shifter. |
| **Pin 38 (GND)** | `GND` | **GND** | Ground Plane | Hardware Interconnect | Common reference ground unified across Maker Pi Base, Level Shifter, and Pico Victim. |

---

## 3. RTOS Task Table ($\mu\text{T-Kernel}$ 3.0)

*Control-logic tasks do not access GPIO/radio/SPI registers directly; all hardware access is mediated by thin driver modules (`D7`–`D9`), keeping the control and peripheral layers separated.*

| Box ID | Task Name | Purpose | Inputs | Outputs | Trigger | Priority |
|:---:|---|---|---|---|---|:---:|
| **D1** | `Safety_Supervisor_Task` | Monitor emergency stop, system faults, and enforce High-Z probe isolation | GPIO9 E-stop IRQ, watchdog, bus short flags | High-Z disarm request to probe driver, RGB LED, buzzer | Event-driven (IRQ / Fault flag) | **Highest (1)** |
| **D2** | `Wireless_Engine_Task` | Drain 2.4 GHz radio baseband FIFOs and extract packet metadata | Radio promiscuous RX baseband event | Ingest packet records into memory pool | Event-driven (Radio Baseband IRQ)| **High (2)** |
| **D3** | `Chaos_Fuzzer_Task` | Inject synchronized glitch pulses into target bus transmissions | Trigger pattern config, edge events | Timed glitch pulse request to probe driver (bit-flip / NACK) | Event-driven (Frame sync event) | **Medium (3)** |
| **D4** | `AutoDiscovery_Task` | Sample edge transitions, measure pulse widths, and resolve pin mappings | Probe edge FIFO timestamps | Resolved protocol ID, pin role table | Periodic (50 ms) / Edge event | **Medium (4)** |
| **D5** | `Storage_Task` | Serialize packet buffers to capture files on removable media | Ingested packet records from message buffer | Disk sector writes via storage driver | Event-driven (Buffer threshold) | **Low (5)** |
| **D6** | `Web_Dashboard_Task` | Serve dashboard assets and stream live telemetry over the wireless interface | Decoded packet headers, bus error stats | Telemetry frames to network driver | Periodic (500–1000 ms) | **Lowest (6)** |

#### Priority and Ownership Justification:
`Safety_Supervisor_Task` is allocated Priority 1 (highest) to guarantee that an emergency stop actuation or electrical contention event preempts all other tasks within $\le 5\text{ ms}$, immediately floating all probe pins to protect connected target hardware. `Wireless_Engine_Task` is set to Priority 2 to prevent hardware radio FIFO overruns during dense 802.11 beacon bursts. `Chaos_Fuzzer_Task` sits at Priority 3 to ensure deterministic, sub-microsecond glitch timing when a target frame trigger is recognized. `AutoDiscovery_Task` sits at Priority 4 to perform edge timing analysis without preempting active frame capture. `Storage_Task` and `Web_Dashboard_Task` are assigned the lowest priorities (5 and 6) because disk and network operations are decoupled through pre-allocated fixed memory pools (`MPF_PACKETS`), ensuring slow SD card sector writes never block time-critical bus sampling.

---

## 4. Task Interaction Diagram
**Figure 2: µT-Kernel 3.0 Task Interaction & IPC Architecture**
![Figure 2: µT-Kernel 3.0 Task Interaction & IPC Architecture](diagrams/task_interaction.png)

#### Interface Payloads (unique IDs):
Every interface arrow in Figure 2 carries a unique ID and a specific payload. ISRs perform no blocking work and no `printf`; they only timestamp/capture and signal via an event flag or mailbox, deferring all processing to tasks.

| Interface ID | From → To | Payload / Data-Control |
|:---:|---|---|
| **I1** | E-Stop ISR → `D1` Safety Supervisor | EventFlag `FLG_SAFETY_ABORT` (bit set, no payload) |
| **I2** | Probe Edge ISR → `D4` Auto-Discovery | Mailbox `MBX_EDGES` (edge timestamp records) |
| **I3** | Radio ISR → `D2` Wireless Engine | EventFlag `FLG_RF_READY` + pool buffer handle |
| **I4** | `D4` ⇄ `D3` (Discovery ⇄ Fuzzer) | Mutex `MTX_PROBE_ACCESS` (probe-line ownership token) |
| **I5** | `D2`/`D4` → Fixed Pool `MPF_PACKETS` | Pool buffer acquire (`tk_get_mpf`), zero-copy packet record |
| **I6** | `MPF_PACKETS` → `D5` Storage | Message Buffer `MBF_STORAGE` (packet record reference) |
| **I7** | `MPF_PACKETS` → `D6` Web Dashboard | Message Buffer `MBF_TELEMETRY` (summary record reference) |
| **I8** | `D1` Safety Supervisor → `D3` Fuzzer | Disarm command (force High-Z, abort injection) |
| **I9** | `D3` Fuzzer → Probe Driver (`D7`) | Timed glitch pulse request (bounded $\le 50\,\mu\text{s}$) |
| **I10** | `D5` Storage → Storage Driver (`D8`) | Sector-write request (capture record) |
| **I11** | `D6` Web Dashboard → Network Driver (`D9`) | Telemetry frame to wireless interface |

#### Explanation of Main Communication Paths:
1. **Safety Interrupt Path (I1, I8, I9):** When the E-stop input falls LOW, the `E-Stop ISR` sets EventFlag `FLG_SAFETY_ABORT` (and does nothing else — no register writes, no `printf`), waking `D1` (Priority 1). `D1` issues a High-Z disarm request to the probe driver module, which floats all probe pins to High-Z within $\le 5\text{ ms}$. Routing the actual pin-direction change through the thin probe driver (rather than `D1` touching output-enable registers directly) keeps control logic separated from peripheral access; the driver call is a bounded, non-blocking register write sized to meet the $\le 5\text{ ms}$ budget.
2. **Probe Arbitration & Timing Path (I2, I4):** `Probe Edge ISR` captures hardware timer timestamps and enqueues them via Mailbox `MBX_EDGES` to `D4`. Probe pin access between discovery and fuzzing is serialized via Mutex `MTX_PROBE_ACCESS`.
3. **Zero-Copy Ingestion & Storage Pipeline (I3, I5, I6, I7):** When the `Radio ISR` receives a frame, it acquires a buffer from Fixed Memory Pool `MPF_PACKETS` (no dynamic allocation) and signals `D2`. The populated packet is referenced into Message Buffers `MBF_STORAGE` and `MBF_TELEMETRY`. Even if a removable-media write incurs a $\sim 30\text{ ms}$ latency spike, the pre-allocated pool absorbs incoming bursts without dropping frames.

---

## 5. System-Level Statechart
**Figure 3: Unified System-Level Statechart**
![Figure 3: Unified System-Level Statechart](diagrams/system_statechart.png)

#### Main States and Assumptions:
* `BOOT_SELFTEST`: Verifies MicroSD card mounting, SPI communication, and radio baseband initialization. Transitions to `BOOT_FAILED` if hardware initialization fails.
* `BOOT_FAILED`: Terminal fault state halting system execution with a blinking red LED if critical boot hardware is unreadable.
* `IDLE_STANDBY`: Default safe operational state. Probe pins are configured as High-Z inputs; fuzzer output drivers are completely disabled.
* `MODE_CONFIG`: Processes user configuration commands to set operating profiles (Wired vs. Wireless, RF channels, or baud search parameters).
* `AUTO_DISCOVERING`: Measures edge counts, pulse widths, and clock period stability across the 4 probes to resolve protocol type and pinout.
* `SNIFFING_ACTIVE`: Ingests packet streams into zero-copy memory pools, writing `.pcap` files to MicroSD and broadcasting live WebSocket telemetry.
* `ARMED_TRIGGER`: Fuzzer armed; synchronizes to target bus framing and awaits a matched address/header trigger pattern.
* `GLITCH_INJECTION`: Injects a precision in-flight glitch pulse (bit-flip or ACK suppression bounded to $t < 50\,\mu\text{s}$) and verifies bus release.
* `CAPTURE_COMPLETE`: Flushes remaining packet ring buffers to MicroSD and closes active PCAP sessions before returning to `IDLE_STANDBY`.
* `FAULT_ESTOP`: Global emergency preemption state invoked by physical E-stop press or bus contention. Forces all probe lines to High-Z in $\le 5\text{ ms}$, sounds buzzer, and latches until manually reset.
---

## 6. Subsystem Statecharts

### 6.1 Subsystem Statechart 1: Wired Auto-Discovery Engine

* **Subsystem Name:** Wired Protocol Auto-Discovery Engine
* **Related RTOS Task:** `AutoDiscovery_Task` (`TSK_DISCOVERY`)
* **Purpose:** Passively classify unknown bus protocols and resolve signal pin assignments.
* **Reason a Statechart is Needed:** Enforces a multi-stage sequential filter (pin triage $\to$ rhythm verification $\to$ confirmation) to reject noise spikes and prevent false protocol locks.
**Figure 4: Subsystem 1 — Wired Auto-Discovery Statechart**
![Figure 4: Subsystem 1 — Wired Auto-Discovery Statechart](diagrams/subsystem_autodiscovery.png)

#### States, Events, and Fault Handling:
* `DISC_LISTEN`: Samples probe transitions. Upon receiving edge interrupts, moves to `GLOBAL_TRIAGE`.
* `GLOBAL_TRIAGE`: Applies exclusivity rules (1 pin = UART candidate, 2 pins = I2C candidate, 3–4 pins = SPI candidate). If active pin count is invalid, triggers `ev_triage_fail`.
* `VERIFY_RHYTHM`: Evaluates baud rate pulse widths ($\pm 5\%$ tolerance) or I2C clock period stability ($\pm 25\%$ jitter bound).
* `PROTO_LOCKED`: Emits `ev_proto_locked`, updates shared pinout tables, and releases `MTX_PROBE_ACCESS`.
* **Fault Handling:** If rhythm verification fails or no valid protocol locks within $2\text{ s}$, transitions to `DISC_TIMEOUT`, notifies the dashboard, and retries up to 3 times before resetting to `DISC_LISTEN`.

---

### 6.2 Subsystem Statechart 2: Chaos Fuzzing Engine

* **Subsystem Name:** Physical Chaos & Fault Injection Engine
* **Related RTOS Task:** `Chaos_Fuzzer_Task` (`TSK_FUZZER`)
* **Purpose:** Inject synchronized non-destructive faults (bit-flips, ACK suppression, clock stretching) into active bus traffic.
* **Reason a Statechart is Needed:** Guarantees that active electrical drive is strictly bounded in time, preventing bus shorts or damaged target drivers.
**Figure 5: Subsystem 2 — Chaos Fuzzing Engine Statechart**
![Figure 5: Subsystem 2 — Chaos Fuzzing Engine Statechart](diagrams/subsystem_fuzzer.png)

#### States, Events, and Fault Handling:
* `FUZZ_STANDBY`: Probes remain in High-Z mode until user issues arm command.
* `AWAIT_TRIGGER`: Evaluates incoming bytes to synchronize with the target frame header.
* `INJECT_GLITCH`: Drives the probe line for a single bit duration (e.g. $8.68\,\mu\text{s}$ at 115200 baud).
* `VERIFY_FLOAT`: Re-samples line state to ensure the line returned to idle HIGH.
* **Fault Handling:** If a line remains driven or pulled LOW for $> 100\,\mu\text{s}$, `FAULT_CONTENTION` is asserted, immediately switching all outputs to High-Z and alerting `Safety_Supervisor_Task`.

---

### 6.3 Subsystem Statechart 3: Wireless Multi-Protocol Sniffer


<svg viewBox="0 0 1000 1000" width="100%" height="100%" xmlns="http://www.w3.org/2000/svg">
  <!-- Clean Background -->
  <rect width="1000" height="1000" fill="#ffffff" />

  <!-- ============================================================= -->
  <!-- 1. TOP BADGES & TEST POINT WIRES                              -->
  <!-- ============================================================= -->

  <!-- Top Badges -->
  <!-- GND -->
  <rect x="390" y="38" width="34" height="15" rx="4" fill="#141414" />
  <text x="407" y="45.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- D_P -->
  <rect x="432" y="38" width="34" height="15" rx="4" fill="#808488" />
  <text x="449" y="45.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">D_P</text>

  <!-- D_N -->
  <rect x="474" y="38" width="34" height="15" rx="4" fill="#808488" />
  <text x="491" y="45.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">D_N</text>

  <!-- NC/GPIO4 -->
  <rect x="516" y="38" width="58" height="15" rx="4" fill="#3ea234" />
  <text x="545" y="45.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">NC/GPIO4</text>

  <!-- GPIO8 -->
  <rect x="582" y="38" width="46" height="15" rx="4" fill="#a2c836" />
  <text x="605" y="45.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO8</text>

  <!-- GPIO9 -->
  <rect x="636" y="38" width="46" height="15" rx="4" fill="#3ea234" />
  <text x="659" y="45.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO9</text>

  <!-- Top Wires (Explicit fill="none" prevents weird triangle fills) -->
  <!-- Wire 1: GND (red) to TP1 -->
  <path d="M 407,53 L 407,68 L 454,68 L 454,148 L 468,148" fill="none" stroke="#cf142b" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />

  <!-- Wire 2: D_P (orange) to TP2 -->
  <path d="M 449,53 L 449,86 L 472,86 L 472,126" fill="none" stroke="#e67e22" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />

  <!-- Wire 3: D_N (green) to TP3 -->
  <path d="M 491,53 L 491,96 L 488,96 L 488,126" fill="none" stroke="#27ae60" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />

  <!-- Wire 4: NC/GPIO4 (cyan) to TP4 -->
  <path d="M 545,53 L 545,68 L 506,68 L 506,166 L 494,166" fill="none" stroke="#00bcd4" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />

  <!-- Wire 5: GPIO8 (lime) to TP5 -->
  <path d="M 605,53 L 605,78 L 516,78 L 516,184 L 494,184" fill="none" stroke="#9ec33a" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />

  <!-- Wire 6: GPIO9 (purple) to TP6 -->
  <path d="M 659,53 L 659,88 L 526,88 L 526,202 L 494,202" fill="none" stroke="#9c27b0" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />


  <!-- ============================================================= -->
  <!-- 2. ESP32-C6-PICO BOARD GRAPHIC                                -->
  <!-- ============================================================= -->
  <g id="board">
    <!-- PCB Substrate -->
    <rect x="417" y="92" width="126" height="376" rx="8" fill="#1c1e22" stroke="#33373e" stroke-width="1.5" />

    <!-- Corner mounting holes -->
    <circle cx="425" cy="102" r="3" fill="#141414" stroke="#d4af37" stroke-width="1.2" />
    <circle cx="535" cy="102" r="3" fill="#141414" stroke="#d4af37" stroke-width="1.2" />
    <circle cx="425" cy="458" r="3" fill="#141414" stroke="#d4af37" stroke-width="1.2" />
    <circle cx="535" cy="458" r="3" fill="#141414" stroke="#d4af37" stroke-width="1.2" />

    <!-- USB Type-C Receptacle -->
    <rect x="462" y="80" width="36" height="26" rx="4" fill="#d0d4da" stroke="#8a9098" stroke-width="1" />
    <rect x="466" y="80" width="28" height="10" rx="3" fill="#141414" />
    <rect x="472" y="83" width="16" height="3" rx="1" fill="#c69a30" />

    <!-- Test Points TP1 - TP6 -->
    <circle cx="472" cy="126" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <text x="468" y="133" font-family="Arial, sans-serif" font-size="4.5" font-weight="bold" fill="#ffffff" text-anchor="middle">TP2</text>

    <circle cx="488" cy="126" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <text x="492" y="133" font-family="Arial, sans-serif" font-size="4.5" font-weight="bold" fill="#ffffff" text-anchor="middle">TP3</text>

    <circle cx="472" cy="148" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <text x="480" y="148.5" font-family="Arial, sans-serif" font-size="4.5" font-weight="bold" fill="#ffffff" dominant-baseline="central">TP1</text>

    <circle cx="488" cy="166" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <text x="480" y="166.5" font-family="Arial, sans-serif" font-size="4.5" font-weight="bold" fill="#ffffff" dominant-baseline="central">TP4</text>

    <circle cx="488" cy="184" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <text x="480" y="184.5" font-family="Arial, sans-serif" font-size="4.5" font-weight="bold" fill="#ffffff" dominant-baseline="central">TP5</text>

    <circle cx="488" cy="202" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <text x="480" y="202.5" font-family="Arial, sans-serif" font-size="4.5" font-weight="bold" fill="#ffffff" dominant-baseline="central">TP6</text>

    <!-- Waveshare Logo & Vertical Silkscreen -->
    <g transform="translate(472, 255) rotate(-90)">
      <path d="M -10,-4 C -6,-1 -4,1 0,0 C 4,-1 6,1 10,-2 C 7,4 2,4 0,1 C -3,0 -6,3 -10,-4 Z" fill="#ffffff" />
      <path d="M -8,2 C -5,5 -2,6 1,4 C 4,2 6,5 9,1 C 6,7 1,6 -1,4 C -4,3 -6,5 -8,2 Z" fill="#ffffff" />
      <circle cx="8" cy="-3.5" r="1" fill="#ffffff" />
    </g>
    <text x="459" y="254" font-family="Arial, Helvetica, sans-serif" font-size="6" font-weight="bold" fill="#e6e6e6" dominant-baseline="central" transform="rotate(-90 459 254)">Waveshare</text>
    <text x="481" y="358" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" fill="#ffffff" letter-spacing="0.4px" transform="rotate(90 481 358)">ESP32-C6-Pico</text>

    <!-- ESP32-C6-MINI-1 Bottom Module -->
    <rect x="428" y="424" width="104" height="40" rx="3" fill="#24272c" stroke="#40454d" stroke-width="1" />
    <path d="M 440,460 L 440,448 L 472,448 M 448,448 L 448,456 M 460,448 L 460,456" fill="none" stroke="#cca033" stroke-width="0.8" />
    <text x="480" y="434" font-family="monospace" font-size="4" fill="#7d848e" text-anchor="middle" transform="rotate(180 480 434)">ESP32-C6-MINI-1 V1.0</text>

    <!-- 20 Pins Contacts & Silk Text: Left & Right -->
    <!-- LEFT SIDE PINS -->
    <!-- Pin 1 -->
    <circle cx="422" cy="120" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="120" r="1.2" fill="#1c1e22" />
    <text x="429" y="120.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">Vbus</text>

    <!-- Pin 2 -->
    <circle cx="422" cy="136.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="136.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="137" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">Vsys</text>

    <!-- Pin 3 -->
    <circle cx="422" cy="153" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="153" r="1.2" fill="#1c1e22" />
    <text x="429" y="153.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GND</text>

    <!-- Pin 4 -->
    <circle cx="422" cy="169.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="169.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="170" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">3V3_EN</text>

    <!-- Pin 5 -->
    <circle cx="422" cy="186" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="186" r="1.2" fill="#1c1e22" />
    <text x="429" y="186.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">3V3</text>

    <!-- Pin 6 -->
    <circle cx="422" cy="202.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="202.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="203" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GP0</text>

    <!-- Pin 7 -->
    <circle cx="422" cy="219" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="219" r="1.2" fill="#1c1e22" />
    <text x="429" y="219.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GP1</text>

    <!-- Pin 8 -->
    <circle cx="422" cy="235.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="235.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="236" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GND</text>

    <!-- Pin 9 -->
    <circle cx="422" cy="252" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="252" r="1.2" fill="#1c1e22" />
    <text x="429" y="252.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GP2</text>

    <!-- Pin 10 -->
    <circle cx="422" cy="268.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="268.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="269" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GP3</text>

    <!-- Pin 11 -->
    <circle cx="422" cy="285" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="285" r="1.2" fill="#1c1e22" />
    <text x="429" y="285.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">RUN</text>

    <!-- Pin 12 -->
    <circle cx="422" cy="301.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="301.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="302" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">EXIO1</text>

    <!-- Pin 13 -->
    <circle cx="422" cy="318" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="318" r="1.2" fill="#1c1e22" />
    <text x="429" y="318.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GND</text>

    <!-- Pin 14 -->
    <circle cx="422" cy="334.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="334.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="335" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GP23</text>

    <!-- Pin 15 -->
    <circle cx="422" cy="351" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="351" r="1.2" fill="#1c1e22" />
    <text x="429" y="351.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GP22</text>

    <!-- Pin 16 -->
    <circle cx="422" cy="367.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="367.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="368" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">EXIO2</text>

    <!-- Pin 17 -->
    <circle cx="422" cy="384" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="384" r="1.2" fill="#1c1e22" />
    <text x="429" y="384.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">EXIO3</text>

    <!-- Pin 18 -->
    <circle cx="422" cy="400.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="400.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="401" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">GND</text>

    <!-- Pin 19 -->
    <circle cx="422" cy="417" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="417" r="1.2" fill="#1c1e22" />
    <text x="429" y="417.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">EXIO4</text>

    <!-- Pin 20 -->
    <circle cx="422" cy="433.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="422" cy="433.5" r="1.2" fill="#1c1e22" />
    <text x="429" y="434" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" dominant-baseline="central">EXIO5</text>

    <!-- RIGHT SIDE PINS -->
    <!-- Pin 40 -->
    <circle cx="538" cy="120" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="120" r="1.2" fill="#1c1e22" />
    <text x="531" y="120.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">TXD</text>

    <!-- Pin 39 -->
    <circle cx="538" cy="136.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="136.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="137" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">RXD</text>

    <!-- Pin 38 -->
    <circle cx="538" cy="153" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="153" r="1.2" fill="#1c1e22" />
    <text x="531" y="153.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GND</text>

    <!-- Pin 37 -->
    <circle cx="538" cy="169.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="169.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="170" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP4</text>

    <!-- Pin 36 -->
    <circle cx="538" cy="186" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="186" r="1.2" fill="#1c1e22" />
    <text x="531" y="186.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP5</text>

    <!-- Pin 35 -->
    <circle cx="538" cy="202.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="202.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="203" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP6</text>

    <!-- Pin 34 -->
    <circle cx="538" cy="219" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="219" r="1.2" fill="#1c1e22" />
    <text x="531" y="219.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP7</text>

    <!-- Pin 33 -->
    <circle cx="538" cy="235.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="235.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="236" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GND</text>

    <!-- Pin 32 -->
    <circle cx="538" cy="252" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="252" r="1.2" fill="#1c1e22" />
    <text x="531" y="252.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP14</text>

    <!-- Pin 31 -->
    <circle cx="538" cy="268.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="268.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="269" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP15</text>

    <!-- Pin 30 -->
    <circle cx="538" cy="285" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="285" r="1.2" fill="#1c1e22" />
    <text x="531" y="285.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP8</text>

    <!-- Pin 29 -->
    <circle cx="538" cy="301.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="301.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="302" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP9</text>

    <!-- Pin 28 -->
    <circle cx="538" cy="318" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="318" r="1.2" fill="#1c1e22" />
    <text x="531" y="318.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GND</text>

    <!-- Pin 27 -->
    <circle cx="538" cy="334.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="334.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="335" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP18</text>

    <!-- Pin 26 -->
    <circle cx="538" cy="351" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="351" r="1.2" fill="#1c1e22" />
    <text x="531" y="351.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP19</text>

    <!-- Pin 25 -->
    <circle cx="538" cy="367.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="367.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="368" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP20</text>

    <!-- Pin 24 -->
    <circle cx="538" cy="384" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="384" r="1.2" fill="#1c1e22" />
    <text x="531" y="384.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GP21</text>

    <!-- Pin 23 -->
    <circle cx="538" cy="400.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="400.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="401" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">GND</text>

    <!-- Pin 22 -->
    <circle cx="538" cy="417" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="417" r="1.2" fill="#1c1e22" />
    <text x="531" y="417.5" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">EXIO6</text>

    <!-- Pin 21 -->
    <circle cx="538" cy="433.5" r="2.5" fill="#d4af37" stroke="#8c6c20" stroke-width="0.5" />
    <circle cx="538" cy="433.5" r="1.2" fill="#1c1e22" />
    <text x="531" y="434" font-family="monospace, sans-serif" font-size="5.8" font-weight="bold" fill="#e6e6e6" text-anchor="end" dominant-baseline="central">EXIO7</text>
  </g>


  <!-- ============================================================= -->
  <!-- 3. PINOUT LABELS & TRACES (LEFT SIDE)                         -->
  <!-- ============================================================= -->

  <!-- Row 1: Pin 1 - VBUS -->
  <path d="M 422,120 L 394,120" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="356" y="113.25" width="38" height="13.5" rx="4" fill="#cf142b" />
  <text x="375" y="120" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">VBUS</text>

  <!-- Row 2: Pin 2 - VSYS -->
  <path d="M 422,136.5 L 394,136.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="356" y="129.75" width="38" height="13.5" rx="4" fill="#cf142b" />
  <text x="375" y="136.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">VSYS</text>

  <!-- Row 3: Pin 3 - GND -->
  <path d="M 422,153 L 394,153" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="362" y="146.25" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="378" y="153" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 4: Pin 4 - 3V3_EN -->
  <path d="M 422,169.5 L 394,169.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="344" y="162.75" width="50" height="13.5" rx="4" fill="#cf142b" />
  <text x="369" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">3V3_EN</text>

  <!-- Row 5: Pin 5 - 3V3 -->
  <path d="M 422,186 L 394,186" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="364" y="179.25" width="30" height="13.5" rx="4" fill="#cf142b" />
  <text x="379" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">3V3</text>

  <!-- Row 6: Pin 6 - GP0 (PWM Wave: explicit fill="none") -->
  <path d="M 422,202.5 L 413,202.5 L 409,198.5 L 405,206.5 L 401,202.5 L 394,202.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- Badges: ADC1_CH0 | LP_UART_DTRN | LP_GPIO0 | XTAL_32K_P | GPIO0 -->
  <rect x="40" y="195.75" width="64" height="13.5" rx="4" fill="#862688" />
  <text x="72" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADC1_CH0</text>
  <rect x="106.5" y="195.75" width="94" height="13.5" rx="4" fill="#7ecca5" />
  <text x="153.5" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_UART_DTRN</text>
  <rect x="203" y="195.75" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="235" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO0</text>
  <rect x="269.5" y="195.75" width="78" height="13.5" rx="4" fill="#d8707a" />
  <text x="308.5" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">XTAL_32K_P</text>
  <rect x="350" y="195.75" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="372" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO0</text>

  <!-- Row 7: Pin 7 - GP1 (PWM) -->
  <path d="M 422,219 L 413,219 L 409,215 L 405,223 L 401,219 L 394,219" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- Badges: ADC1_CH1 | LP_UART_DSRN | LP_GPIO1 | XTAL_32K_N | GPIO1 -->
  <rect x="40" y="212.25" width="64" height="13.5" rx="4" fill="#862688" />
  <text x="72" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADC1_CH1</text>
  <rect x="106.5" y="212.25" width="94" height="13.5" rx="4" fill="#7ecca5" />
  <text x="153.5" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_UART_DSRN</text>
  <rect x="203" y="212.25" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="235" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO1</text>
  <rect x="269.5" y="212.25" width="78" height="13.5" rx="4" fill="#d8707a" />
  <text x="308.5" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">XTAL_32K_N</text>
  <rect x="350" y="212.25" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="372" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO1</text>

  <!-- Row 8: Pin 8 - GND -->
  <path d="M 422,235.5 L 394,235.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="362" y="228.75" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="378" y="235.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 9: Pin 9 - GP2 (PWM) -->
  <path d="M 422,252 L 413,252 L 409,248 L 405,256 L 401,252 L 394,252" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- Badges: FSPIQ | ADC1_CH2 | LP_UART_RTSN | LP_GPIO2 | GPIO2 -->
  <rect x="74" y="245.25" width="44" height="13.5" rx="4" fill="#a2c836" />
  <text x="96" y="252" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPIQ</text>
  <rect x="120.5" y="245.25" width="64" height="13.5" rx="4" fill="#862688" />
  <text x="152.5" y="252" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADC1_CH2</text>
  <rect x="187" y="245.25" width="94" height="13.5" rx="4" fill="#7ecca5" />
  <text x="234" y="252" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_UART_RTSN</text>
  <rect x="283.5" y="245.25" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="315.5" y="252" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO2</text>
  <rect x="350" y="245.25" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="372" y="252" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO2</text>

  <!-- Row 10: Pin 10 - GP3 (PWM) -->
  <path d="M 422,268.5 L 413,268.5 L 409,264.5 L 405,272.5 L 401,268.5 L 394,268.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- Badges: ADC1_CH3 | LP_UART_CTSN | LP_GPIO3 | GPIO3 -->
  <rect x="120.5" y="261.75" width="64" height="13.5" rx="4" fill="#862688" />
  <text x="152.5" y="268.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADC1_CH3</text>
  <rect x="187" y="261.75" width="94" height="13.5" rx="4" fill="#7ecca5" />
  <text x="234" y="268.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_UART_CTSN</text>
  <rect x="283.5" y="261.75" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="315.5" y="268.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO3</text>
  <rect x="350" y="261.75" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="372" y="268.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO3</text>

  <!-- Row 11: Pin 11 - RUN -->
  <path d="M 422,285 L 394,285" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="362" y="278.25" width="32" height="13.5" rx="4" fill="#d8707a" />
  <text x="378" y="285" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">RUN</text>

  <!-- Row 12: Pin 12 - EXIO1 (PWM) -->
  <path d="M 422,301.5 L 413,301.5 L 409,297.5 L 405,305.5 L 401,301.5 L 394,301.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="350" y="294.75" width="44" height="13.5" rx="4" fill="#1c0ca3" />
  <text x="372" y="301.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO1</text>

  <!-- Row 13: Pin 13 - GND -->
  <path d="M 422,318 L 394,318" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="362" y="311.25" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="378" y="318" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 14: Pin 14 - GP23 (PWM) -->
  <path d="M 422,334.5 L 413,334.5 L 409,330.5 L 405,338.5 L 401,334.5 L 394,334.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="261.5" y="327.75" width="80" height="13.5" rx="4" fill="#cb6b38" />
  <text x="301.5" y="334.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO_DATA3</text>
  <rect x="344" y="327.75" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="369" y="334.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO23</text>

  <!-- Row 15: Pin 15 - GP22 (PWM) -->
  <path d="M 422,351 L 413,351 L 409,347 L 405,355 L 401,351 L 394,351" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="261.5" y="344.25" width="80" height="13.5" rx="4" fill="#cb6b38" />
  <text x="301.5" y="351" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO_DATA2</text>
  <rect x="344" y="344.25" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="369" y="351" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO22</text>

  <!-- Row 16: Pin 16 - EXIO2 (PWM) -->
  <path d="M 422,367.5 L 413,367.5 L 409,363.5 L 405,371.5 L 401,367.5 L 394,367.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="350" y="360.75" width="44" height="13.5" rx="4" fill="#1c0ca3" />
  <text x="372" y="367.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO2</text>

  <!-- Row 17: Pin 17 - EXIO3 (PWM) -->
  <path d="M 422,384 L 413,384 L 409,380 L 405,388 L 401,384 L 394,384" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="350" y="377.25" width="44" height="13.5" rx="4" fill="#1c0ca3" />
  <text x="372" y="384" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO3</text>

  <!-- Row 18: Pin 18 - GND -->
  <path d="M 422,400.5 L 394,400.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="362" y="393.75" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="378" y="400.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 19: Pin 19 - EXIO4 (PWM) -->
  <path d="M 422,417 L 413,417 L 409,413 L 405,421 L 401,417 L 394,417" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="350" y="410.25" width="44" height="13.5" rx="4" fill="#1c0ca3" />
  <text x="372" y="417" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO4</text>

  <!-- Row 20: Pin 20 - EXIO5 (PWM) -->
  <path d="M 422,433.5 L 413,433.5 L 409,429.5 L 405,437.5 L 401,433.5 L 394,433.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="350" y="426.75" width="44" height="13.5" rx="4" fill="#1c0ca3" />
  <text x="372" y="433.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO5</text>


  <!-- ============================================================= -->
  <!-- 4. PINOUT LABELS & TRACES (RIGHT SIDE)                        -->
  <!-- ============================================================= -->

  <!-- Row 1: Pin 40 - TXD (PWM) -->
  <path d="M 538,120 L 545,120 L 549,116 L 553,124 L 557,120 L 566,120" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="113.25" width="44" height="13.5" rx="4" fill="#808488" />
  <text x="588" y="120" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">U0TXD</text>
  <rect x="612.5" y="113.25" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="637.5" y="120" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO16</text>
  <rect x="665" y="113.25" width="56" height="13.5" rx="4" fill="#a2c836" />
  <text x="693" y="120" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPICS0</text>

  <!-- Row 2: Pin 39 - RXD (PWM) -->
  <path d="M 538,136.5 L 545,136.5 L 549,132.5 L 553,140.5 L 557,136.5 L 566,136.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="129.75" width="44" height="13.5" rx="4" fill="#808488" />
  <text x="588" y="136.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">U0RXD</text>
  <rect x="612.5" y="129.75" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="637.5" y="136.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO17</text>
  <rect x="665" y="129.75" width="56" height="13.5" rx="4" fill="#a2c836" />
  <text x="693" y="136.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPICS1</text>

  <!-- Row 3: Pin 38 - GND -->
  <path d="M 538,153 L 566,153" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="566" y="146.25" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="582" y="153" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 4: Pin 37 - GP4 (PWM) -->
  <path d="M 538,169.5 L 545,169.5 L 549,165.5 L 553,173.5 L 557,169.5 L 566,169.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- MTMS | GPIO4 | LP_GPIO4 | LP_UART_RXD | ADC1_CH4 | FSPIHD | SDIO -->
  <rect x="566" y="162.75" width="38" height="13.5" rx="4" fill="#4ec3e6" />
  <text x="585" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">MTMS</text>
  <rect x="606.5" y="162.75" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="628.5" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO4</text>
  <rect x="653" y="162.75" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="685" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO4</text>
  <rect x="719.5" y="162.75" width="84" height="13.5" rx="4" fill="#7ecca5" />
  <text x="761.5" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_UART_RXD</text>
  <rect x="806" y="162.75" width="64" height="13.5" rx="4" fill="#862688" />
  <text x="838" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADC1_CH4</text>
  <rect x="872.5" y="162.75" width="50" height="13.5" rx="4" fill="#a2c836" />
  <text x="897.5" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPIHD</text>
  <rect x="925" y="162.75" width="38" height="13.5" rx="4" fill="#df0f66" />
  <text x="944" y="169.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO</text>

  <!-- Row 5: Pin 36 - GP5 (PWM) -->
  <path d="M 538,186 L 545,186 L 549,182 L 553,190 L 557,186 L 566,186" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- MTDI | GPIO5 | LP_GPIO5 | LP_UART_TXD | ADC1_CH5 | FSPIWP | SDIO -->
  <rect x="566" y="179.25" width="38" height="13.5" rx="4" fill="#4ec3e6" />
  <text x="585" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">MTDI</text>
  <rect x="606.5" y="179.25" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="628.5" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO5</text>
  <rect x="653" y="179.25" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="685" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO5</text>
  <rect x="719.5" y="179.25" width="84" height="13.5" rx="4" fill="#7ecca5" />
  <text x="761.5" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_UART_TXD</text>
  <rect x="806" y="179.25" width="64" height="13.5" rx="4" fill="#862688" />
  <text x="838" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADC1_CH5</text>
  <rect x="872.5" y="179.25" width="50" height="13.5" rx="4" fill="#a2c836" />
  <text x="897.5" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPIWP</text>
  <rect x="925" y="179.25" width="38" height="13.5" rx="4" fill="#df0f66" />
  <text x="944" y="186" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO</text>

  <!-- Row 6: Pin 35 - GP6 (PWM) -->
  <path d="M 538,202.5 L 545,202.5 L 549,198.5 L 553,206.5 L 557,202.5 L 566,202.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- MTCK | GPIO6 | LP_GPIO6 | LP_I2C_SDA | ADC1_CH6 | FSPICLK -->
  <rect x="566" y="195.75" width="38" height="13.5" rx="4" fill="#4ec3e6" />
  <text x="585" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">MTCK</text>
  <rect x="606.5" y="195.75" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="628.5" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO6</text>
  <rect x="653" y="195.75" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="685" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO6</text>
  <rect x="719.5" y="195.75" width="76" height="13.5" rx="4" fill="#cca033" />
  <text x="757.5" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_I2C_SDA</text>
  <rect x="798" y="195.75" width="64" height="13.5" rx="4" fill="#862688" />
  <text x="830" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADC1_CH6</text>
  <rect x="864.5" y="195.75" width="54" height="13.5" rx="4" fill="#a2c836" />
  <text x="891.5" y="202.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPICLK</text>

  <!-- Row 7: Pin 34 - GP7 (PWM) -->
  <path d="M 538,219 L 545,219 L 549,215 L 553,223 L 557,219 L 566,219" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- MTDO | GPIO7 | LP_GPIO7 | LP_I2C_SCL | FSPID -->
  <rect x="566" y="212.25" width="38" height="13.5" rx="4" fill="#4ec3e6" />
  <text x="585" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">MTDO</text>
  <rect x="606.5" y="212.25" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="628.5" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO7</text>
  <rect x="653" y="212.25" width="64" height="13.5" rx="4" fill="#248acc" />
  <text x="685" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO7</text>
  <rect x="719.5" y="212.25" width="76" height="13.5" rx="4" fill="#cca033" />
  <text x="757.5" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_I2C_SCL</text>
  <rect x="798" y="212.25" width="44" height="13.5" rx="4" fill="#a2c836" />
  <text x="820" y="219" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPID</text>

  <!-- Row 8: Pin 33 - GND -->
  <path d="M 538,235.5 L 566,235.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="566" y="228.75" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="582" y="235.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 9: Pin 32 - GP14 (PWM) -->
  <path d="M 538,252 L 545,252 L 549,248 L 553,256 L 557,252 L 566,252" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="245.25" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="591" y="252" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO14</text>

  <!-- Row 10: Pin 31 - GP15 (PWM) -->
  <path d="M 538,268.5 L 545,268.5 L 549,264.5 L 553,272.5 L 557,268.5 L 566,268.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="261.75" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="591" y="268.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO15</text>
  <rect x="618.5" y="261.75" width="38" height="13.5" rx="4" fill="#df0f66" />
  <text x="637.5" y="268.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">JTAG</text>

  <!-- Row 11: Pin 30 - GP8 (PWM) -->
  <path d="M 538,285 L 545,285 L 549,281 L 553,289 L 557,285 L 566,285" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <!-- GPIO8 | BOOT | ROM | RGB LED -->
  <rect x="566" y="278.25" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="588" y="285" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO8</text>
  <rect x="612.5" y="278.25" width="38" height="13.5" rx="4" fill="#df0f66" />
  <text x="631.5" y="285" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">BOOT</text>
  <rect x="653" y="278.25" width="34" height="13.5" rx="4" fill="#b6005d" />
  <text x="670" y="285" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ROM</text>
  <rect x="689.5" y="278.25" width="54" height="13.5" rx="4" fill="#d8707a" />
  <text x="716.5" y="285" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">RGB LED</text>

  <!-- Row 12: Pin 29 - GP9 (PWM) -->
  <path d="M 538,301.5 L 545,301.5 L 549,297.5 L 553,305.5 L 557,301.5 L 566,301.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="294.75" width="44" height="13.5" rx="4" fill="#3ea234" />
  <text x="588" y="301.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO9</text>
  <rect x="612.5" y="294.75" width="38" height="13.5" rx="4" fill="#df0f66" />
  <text x="631.5" y="301.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">BOOT</text>

  <!-- Row 13: Pin 28 - GND -->
  <path d="M 538,318 L 566,318" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="566" y="311.25" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="582" y="318" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 14: Pin 27 - GP18 (PWM) -->
  <path d="M 538,334.5 L 545,334.5 L 549,330.5 L 553,338.5 L 557,334.5 L 566,334.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="327.75" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="591" y="334.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO18</text>
  <rect x="618.5" y="327.75" width="66" height="13.5" rx="4" fill="#cb6b38" />
  <text x="651.5" y="334.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO_CMD</text>
  <rect x="687" y="327.75" width="56" height="13.5" rx="4" fill="#a2c836" />
  <text x="715" y="334.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPICS2</text>

  <!-- Row 15: Pin 26 - GP19 (PWM) -->
  <path d="M 538,351 L 545,351 L 549,347 L 553,355 L 557,351 L 566,351" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="344.25" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="591" y="351" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO19</text>
  <rect x="618.5" y="344.25" width="66" height="13.5" rx="4" fill="#cb6b38" />
  <text x="651.5" y="351" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO_CLK</text>
  <rect x="687" y="344.25" width="56" height="13.5" rx="4" fill="#a2c836" />
  <text x="715" y="351" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPICS3</text>

  <!-- Row 16: Pin 25 - GP20 (PWM) -->
  <path d="M 538,367.5 L 545,367.5 L 549,363.5 L 553,371.5 L 557,367.5 L 566,367.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="360.75" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="591" y="367.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO20</text>
  <rect x="618.5" y="360.75" width="80" height="13.5" rx="4" fill="#cb6b38" />
  <text x="658.5" y="367.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO_DATA0</text>
  <rect x="701" y="360.75" width="56" height="13.5" rx="4" fill="#a2c836" />
  <text x="729" y="367.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPICS4</text>

  <!-- Row 17: Pin 24 - GP21 (PWM) -->
  <path d="M 538,384 L 545,384 L 549,380 L 553,388 L 557,384 L 566,384" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="377.25" width="50" height="13.5" rx="4" fill="#3ea234" />
  <text x="591" y="384" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIO21</text>
  <rect x="618.5" y="377.25" width="80" height="13.5" rx="4" fill="#cb6b38" />
  <text x="658.5" y="384" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO_DATA1</text>
  <rect x="701" y="377.25" width="56" height="13.5" rx="4" fill="#a2c836" />
  <text x="729" y="384" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPICS5</text>

  <!-- Row 18: Pin 23 - GND -->
  <path d="M 538,400.5 L 566,400.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" />
  <rect x="566" y="393.75" width="32" height="13.5" rx="4" fill="#141414" />
  <text x="582" y="400.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>

  <!-- Row 19: Pin 22 - EXIO6 (PWM) -->
  <path d="M 538,417 L 545,417 L 549,413 L 553,421 L 557,417 L 566,417" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="410.25" width="44" height="13.5" rx="4" fill="#1c0ca3" />
  <text x="588" y="417" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO6</text>

  <!-- Row 20: Pin 21 - EXIO7 (PWM) -->
  <path d="M 538,433.5 L 545,433.5 L 549,429.5 L 553,437.5 L 557,433.5 L 566,433.5" fill="none" stroke="#000000" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
  <rect x="566" y="426.75" width="44" height="13.5" rx="4" fill="#1c0ca3" />
  <text x="588" y="433.5" font-family="Arial, Helvetica, sans-serif" font-size="8" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO7</text>


  <!-- ============================================================= -->
  <!-- 5. BOTTOM LEGEND BOX                                         -->
  <!-- ============================================================= -->
  <g id="legend">
    <!-- Legend Outer Box -->
    <rect x="40" y="485" width="915" height="485" rx="14" fill="#ffffff" stroke="#c8cdd4" stroke-width="1.8" />

    <!-- ROW 0 -->
    <!-- Left: PWM Capable Pin -->
    <path d="M 75,536 L 98,536 L 115,516 L 126,554 L 138,536 L 160,536" fill="none" stroke="#000000" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" />
    <text x="180" y="536" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">PWM Capable Pin</text>

    <!-- Right: OTHER -->
    <rect x="530" y="523" width="110" height="26" rx="13" fill="#d8707a" />
    <text x="585" y="536" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">OTHER</text>
    <text x="655" y="536" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Other Related Functions</text>

    <!-- ROW 1 -->
    <!-- Left: LP_UART -->
    <rect x="65" y="574" width="110" height="26" rx="13" fill="#7ecca5" />
    <text x="120" y="587" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_UART</text>
    <text x="185" y="587" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Low-Power UART Functions</text>

    <!-- Right: ADCX_CH -->
    <rect x="530" y="574" width="110" height="26" rx="13" fill="#862688" />
    <text x="585" y="587" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">ADCX_CH</text>
    <text x="655" y="587" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Analog-to-Digital Converter</text>

    <!-- ROW 2 -->
    <!-- Left: FSPI -->
    <rect x="65" y="632" width="110" height="26" rx="13" fill="#a2c836" />
    <text x="120" y="645" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">FSPI</text>
    <text x="185" y="645" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Fast SPI Functions</text>

    <!-- Right: STRAP -->
    <rect x="530" y="632" width="110" height="26" rx="13" fill="#df0f66" />
    <text x="585" y="645" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">STRAP</text>
    <text x="655" y="645" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Strapping Pin Functions</text>

    <!-- ROW 3 -->
    <!-- Left: GPIOX -->
    <rect x="65" y="690" width="110" height="26" rx="13" fill="#3ea234" />
    <text x="120" y="703" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GPIOX</text>
    <text x="185" y="703" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">General Purpose Input and Output</text>

    <!-- Right: SDIO -->
    <rect x="530" y="690" width="110" height="26" rx="13" fill="#cb6b38" />
    <text x="585" y="703" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SDIO</text>
    <text x="655" y="703" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">SDIO Functions</text>

    <!-- ROW 4 -->
    <!-- Left: LP_I2C -->
    <rect x="65" y="748" width="110" height="26" rx="13" fill="#cca033" />
    <text x="120" y="761" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_I2C</text>
    <text x="185" y="761" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Low-Power I2C Functions</text>

    <!-- Right: LP_GPIO -->
    <rect x="530" y="748" width="110" height="26" rx="13" fill="#248acc" />
    <text x="585" y="761" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">LP_GPIO</text>
    <text x="655" y="761" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Low-Power GPIO Functions</text>

    <!-- ROW 5 -->
    <!-- Left: JTAG -->
    <rect x="65" y="806" width="110" height="26" rx="13" fill="#4ec3e6" />
    <text x="120" y="819" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">JTAG</text>
    <text x="185" y="819" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">JTAG for Debugging</text>

    <!-- Right: PWR -->
    <rect x="530" y="806" width="110" height="26" rx="13" fill="#cf142b" />
    <text x="585" y="819" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">PWR</text>
    <text x="655" y="819" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Power Rails (3V3, 5V, Battery)</text>

    <!-- ROW 6 -->
    <!-- Left: SERIAL -->
    <rect x="65" y="864" width="110" height="26" rx="13" fill="#808488" />
    <text x="120" y="877" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">SERIAL</text>
    <text x="185" y="877" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Serial for Debug/Programming</text>

    <!-- Right: GND -->
    <rect x="530" y="864" width="110" height="26" rx="13" fill="#141414" />
    <text x="585" y="877" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">GND</text>
    <text x="655" y="877" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Ground Plane</text>

    <!-- ROW 7 -->
    <!-- Left: EXIO -->
    <rect x="65" y="922" width="110" height="26" rx="13" fill="#1c0ca3" />
    <text x="120" y="935" font-family="Arial, Helvetica, sans-serif" font-size="12.5" font-weight="bold" text-anchor="middle" dominant-baseline="central" fill="#ffffff">EXIO</text>
    <text x="185" y="935" font-family="Arial, Helvetica, sans-serif" font-size="15" fill="#1f1f1f" dominant-baseline="central">Expand the GPIO pin</text>
  </g>
</svg>


* **Subsystem Name:** Wireless Multi-Protocol Sniffer Engine
* **Related RTOS Task:** `Wireless_Engine_Task` (`TSK_WIRELESS`)
* **Purpose:** Intercept raw 802.11 and 802.15.4 packets and extract RF physical metadata.
* **Reason a Statechart is Needed:** Manages radio baseband transitions and provides graceful degradation during high-throughput packet bursts.
**Figure 6: Subsystem 3 — Wireless Multi-Protocol Sniffer Statechart**
![Figure 6: Subsystem 3 — Wireless Multi-Protocol Sniffer Statechart](diagrams/subsystem_wireless.png)

#### States, Events, and Fault Handling:
* `RADIO_IDLE`: Radio disabled; power consumption minimized.
* `PROMISC_TUNE`: Configures channel filter and engages promiscuous mode.
* `CAPTURE_RX`: Drains hardware baseband FIFO upon packet arrival.
* `PARSE_METADATA`: Extracts RSSI, channel ID, and microsecond timestamps into standard PCAP frame format.
* **Fault Handling:** If the fixed memory pool reaches $> 85\%$ capacity, non-critical broadcast data frames are dropped while management/control frames are preserved, logging an overrun warning to telemetry.

---

## 7. Assumptions

| ID     | Assumption                                                                                                                                                      | Risk if Wrong                                                                    | Validation Method                                                                                                                              | Owner       | Status |
| ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | ----------- | :----: |
| **A1** | Probe inputs resolve clean CMOS logic thresholds ($V_{IL} \le 0.8\text{ V}, V_{IH} \ge 2.0\text{ V}$) without excessive ringing on $15\text{ cm}$ jumper lines. | Edge noise triggers spurious interrupts, causing false baud rate detection.      | Capture 100 kHz I2C transitions on an oscilloscope with 15 cm leads; verify rise time $t_r \le 300\text{ ns}$ and no false trigger interrupts. | Tan Yan Qi  |  Open  |
| **A2** | ESP32-C6 GPIO pins can switch direction from High-Z Input to Driven Output in $< 1\,\mu\text{s}$ for targeted bit glitching.                                    | Delayed output drive misses the target clock pulse, failing to inject the fault. | Program a GPIO toggle triggered by an external pulse; measure drive delay on an oscilloscope with $< 50\text{ ns}$ resolution.                 | Annie Goh   |  Open  |
| **A3** | The ESP32-C6 promiscuous packet filter captures raw 802.11 management frames without dropping frames at burst rates up to 50–100 pkts/s.                        | Dropped beacon frames cause incomplete Wireshark trace exports.                  | Blast the sniffer with a 100 pkt/s beacon generator (Node 3); compare transmitted vs. logged frame sequence counters.                          | Zayne Lee   |  Open  |
| **A4** | SPI FAT32 MicroSD write latency spikes ($\le 40\text{ ms}$) can be fully absorbed by an 8 KB pre-allocated packet ring buffer.                                  | Buffer overflow during SD write stalls drops incoming physical/wireless packets. | Simulate a 50 ms SD write stall while streaming UART traffic at 115200 baud; verify zero dropped bytes in memory logs.                         | Ryan Ke     |  Open  |
| **A5** | The WebSocket telemetry server can stream 1–2 Hz packet updates over Wi-Fi without starving the $\mu\text{T-Kernel}$ scheduler.                                 | Network stack CPU load introduces jitter into physical bus sampling.             | Benchmark system timer jitter while serving 3 concurrent WebSocket clients running telemetry streams at 1 Hz.                                  | Joseph Poon |  Open  |
| **A6** | ESP-IDF on the ESP32-C6 permits raw 802.11 frame transmission (`esp_wifi_80211_tx`) of deauthentication frames against the testbed victim node, without being silently filtered by the stack. | Deauth stress injection (R-3.2) cannot be demonstrated; feature must drop to stretch scope. | Transmit a crafted deauth frame at the testbed SoftAP (Node 3) and confirm via the sniffer that the frame is emitted and the victim re-associates; if filtered, fall back to 802.15.4-only stress or beacon-flood. | Zayne Lee / Annie Goh |  Open  |

---

## 8. AI-Supported Design Evidence

### 8.1 Pre-Submission AI Ideation

| Buddy       | AI Prompt or Discussion Used                                                                          | Useful AI Ideas                                                                                                           | Weak or Unrealistic AI Ideas                                                         | Team Decision                                                                                                                        |
| ----------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------ |
| **Buddy 1** | Prompted AI on multi-pin signal triage heuristics for mixed UART and I2C lines.                       | AI proposed the "Global Pin Exclusivity Filter" (counting active lines before calculating timings).                       | AI suggested using a neural network classifier on the Pico to identify protocols.    | Adopted the deterministic pulse-width heuristic; rejected neural networks as an unnecessary compute and memory burden.               |
| **Buddy 2** | Asked AI how to safely inject in-flight faults on open-drain I2C buses without frying master drivers. | AI suggested driving lines strictly during slave ACK clock pulses and adding an immediate High-Z release timeout.         | AI suggested cutting the physical VCC line with a MOSFET to simulate device failure. | Implemented software-timed ACK suppression and physical E-stop High-Z override; rejected cutting VCC to prevent back-powering chips. |
| **Buddy 3** | Inquired whether ESP32-C6 can sniff connected BLE data channels promiscuously.                        | AI identified that ESP-IDF only supports BLE GAP advertisement scanning, not raw data channel sniffing.                   | AI initially claimed BLE promiscuous mode existed via standard APIs.                 | Critiqued and dropped Bluetooth link-layer sniffing; scoped wireless strictly to native Wi-Fi 6 + 802.15.4 promiscuous capture.      |
| **Buddy 4** | Asked AI how to log high-rate packet streams to MicroSD without missing real-time deadlines.          | AI suggested using pre-allocated fixed memory pools (`MPF`) and message buffers to decouple capture from SD block writes. | AI suggested writing raw text JSON strings to the SD card for each captured packet.  | Adopted zero-copy fixed memory pools; rejected JSON in favor of compact binary `libpcap` format.                                     |
| **Buddy 5** | Asked AI to evaluate whether a 0.96" OLED display should be included alongside the Wi-Fi dashboard.   | AI highlighted that the OLED consumes 1 KB RAM, bus bandwidth, and cannot render complex hex packet trees.                | AI suggested keeping both OLED and Web dashboard for redundancy.                     | Completely dropped the OLED screen; allocated resources to the WebSocket web dashboard and status NeoPixel.                          |

### 8.2 Team-Reviewed Architecture Freeze

| AI or Initial Idea                                                                  | Team Decision | Engineering Reason                                                                                                                           | Affected Artefact                                         |
| ----------------------------------------------------------------------------------- | ------------- | -------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------- |
| Use a 3-node closed-loop testbed (Tool + Wired Victim + Wireless Victim).           | Accepted      | Proves technical scope with dedicated, repeatable hardware; eliminates reliance on noisy campus Wi-Fi or uncontrolled targets.               | Testbed topology, System overview, Hardware pinout        |
| Mount ESP32-C6 on Cytron Maker Pi Pico Base carrier board.                          | Accepted      | Eliminates external MicroSD breakout module by utilizing onboard SD slot; provides per-pin blue LED diagnostics and clean benchtop mounting. | Hardware pinout table, Schematic, Bill of Materials       |
| Drop 0.96" OLED display in favor of Wi-Fi WebSocket dashboard.                      | Accepted      | Eliminates I2C bus contention, saves 1 KB RAM, and provides far superior visual clarity during zoom tribunals.                               | Bill of Materials, Pinout table, Task table               |
| Mandate bidirectional 3.3V $\leftrightarrow$ 5V logic level shifter on probe lines. | Accepted      | RP2040 and ESP32-C6 pads have a 3.6V maximum limit; connecting 5V targets directly causes over-voltage destruction.                          | Hardware integration table, Schematic, NFR-4               |
| Omit Bluetooth (BLE) promiscuous mode from the project scope.                       | Accepted      | Standard ESP-IDF lacks raw link-layer BLE sniffing APIs; initializing NimBLE consumes 45 KB SRAM and 200 KB flash.                           | Protocol matrix, Wireless requirements, Task architecture |

### 8.3 Final AI-Vetted Gap Check

* **AI Reviewer Prompt Used:** *"Act as a strict embedded systems reviewer for whattheflip. Check requirements, pinout, RTOS task table, IPC architecture, statecharts, assumptions, and consistency. Identify missing requirements, unclear interfaces, untested assumptions, timing risks, task conflicts, and shared-resource problems."*

| AI-Identified Gap | Team Response | Action Taken | Evidence or Reason |
|---|---|---|---|
| Maker Pi Base buttons on GP20/GP21/GP22 tie to the TCA9554 expander I2C domain. | **Fixed** | Mandated the native BOOT switch on Pin 12 (`GPIO9`) for Emergency Stop, and left all three Maker Pi base user buttons unused. | Hardware integration Section 2.2 (pin table + safety invariant note). |
| Unclear probe line ownership between Auto-Discovery and Fuzzing tasks. | **Fixed** | Defined dynamic probe sharing arbitrated via Mutex `MTX_PROBE_ACCESS`. | Task table Section 3 and IPC diagram Section 4 updated. |
| MicroSD card write latency spikes ($\le 40\text{ ms}$) may cause buffer overrun during burst captures. | **Needs Validation** | Sized fixed memory pool (`MPF_PACKETS`) to 8 KB to buffer up to 32 consecutive frames; validated via Assumption A4. | Assumption A4 documented with simulated 50 ms stall benchmark. |
| Single 2.4 GHz RF front-end cannot capture Wi-Fi and 802.15.4 simultaneously. | **Accepted as Risk** | Documented time-multiplexed capture constraint; system operates in distinct Wi-Fi or Zigbee listening modes. | Documented in Section 2 and Subsystem Statechart 3. |

---

## 9. Design Consistency

* **Which task owns each hardware device?**
  * `AutoDiscovery_Task` and `Chaos_Fuzzer_Task` dynamically share the 4 target probe lines on Maker Pi Pins 1, 2, 4, 5 (ESP32-C6 `GPIO16`, `GPIO17`, `GPIO4`, `GPIO5`), arbitrated via Mutex `MTX_PROBE_ACCESS`.
  * `Wireless_Engine_Task` owns the 2.4 GHz radio transceiver baseband.
  * `Storage_Task` owns the Maker Pi onboard MicroSD card slot hardwired to physical pins 14–16 and 20 (ESP32-C6 `GPIO18`, `GPIO19`, `GPIO20`, and `EXIO7`).
  * `Safety_Supervisor_Task` owns the emergency stop button on Maker Pi Pin 12 (`GPIO9` BOOT switch), status NeoPixel (`GPIO8`), and alert buzzer (`EXIO3`).
  * `Web_Dashboard_Task` owns the Wi-Fi AP network socket interface.
* **Which task has the highest priority, and why?**
  * `Safety_Supervisor_Task` has the highest priority (Priority 1) because target hardware safety is the primary system invariant. If an electrical short occurs or the user presses the emergency stop, the system must immediately preempt all other tasks and drop probe pins to High-Z in $\le 5\text{ ms}$.
* **Which subsystem statechart corresponds to which RTOS task or subsystem?**
  * Subsystem Statechart 1 corresponds to `AutoDiscovery_Task` (`TSK_DISCOVERY`).
  * Subsystem Statechart 2 corresponds to `Chaos_Fuzzer_Task` (`TSK_FUZZER`).
  * Subsystem Statechart 3 corresponds to `Wireless_Engine_Task` (`TSK_WIRELESS`).
* **How are functional requirements reflected in the task and statechart design?**
  * R-1.1–R-1.3 (Probing, Auto-Discovery, Pin Routing) map to `AutoDiscovery_Task` (`D4`) and the `AUTO_DISCOVERING` state.
  * R-2.1 (Wireless Capture) maps to `Wireless_Engine_Task` (`D2`) and the `SNIFFING_ACTIVE` state.
  * R-3.1–R-3.2 (Chaos Fault Injection, Wireless Stress) map to `Chaos_Fuzzer_Task` (`D3`) and the `GLITCH_INJECTION` state.
  * ER-1 (Emergency Stop) maps to `Safety_Supervisor_Task` (`D1`) and the `FAULT_ESTOP` state.
  * R-4.1 and R-5.1 (Capture Storage and Dashboard Streaming) map to `Storage_Task` (`D5`) and `Web_Dashboard_Task` (`D6`).
* **What changes are still expected after the Week 6 review?**
  * Confirmation of empirical rise-time tolerances during Week 8 hardware wiring tests.
  * Fine-tuning of storage block transfer sizes and ring buffer depths based on measured write-latency benchmarks.
  * Validation of assumptions A1 through A6 using physical oscilloscope and network traffic generator measurements before Week 10 Subsystems Check-in.

---

## 10. Appendix: Lecture 6.1 Traceability Matrix (V-Model Verification)

$$\text{Requirement ID} \longrightarrow \text{Use Case / Sequence ID} \longrightarrow \text{LLD Function Name} \longrightarrow \text{Test ID} \longrightarrow \text{Expected Result (Pass/Fail)}$$

| Requirement ID | Use Case / Sequence ID | LLD Function Name | Test ID | Expected Result | Verification Status |
|---|---|---|---|---|:---:|
| **R-1.1** (Passive Probing) | `UC-1.1` (Sample Bus Lines) | `probe_sample_edges()` | **TC-1.1** | Edge timestamps resolved to $\le 1\,\mu\text{s}$; leakage $\le 1\,\mu\text{A}$ | Planned |
| **R-1.2** (Auto-Discovery) | `UC-1.2` (Classify Protocol) | `analyze_for_i2c()`, `analyze_for_uart()`, `analyze_for_spi()` | **TC-1.2** | Correct protocol identified within $\le 500\text{ ms}$ for all 3 bus types | Planned |
| **R-1.3** (Dynamic Reconfig) | `UC-1.3` (Map Peripheral Pins) | `esp_rom_gpio_connect_in_signal()` / `..._out_signal()` (GPIO matrix) | **TC-1.3** | Any probe pin routed to any decode channel; ready $\le 10\text{ ms}$, no rewiring | Planned |
| **R-2.1** (Wi-Fi Capture) | `UC-2.1` (Capture 802.11 Frames)| `wifi_promiscuous_rx_cb()` | **TC-2.1** | $\ge 99\%$ of injected mgmt frames logged with RSSI/channel/timestamp | Planned |
| **R-2.1** (Zigbee Capture) | `UC-2.2` (Capture 802.15.4) | `ieee802154_rx_cb()` | **TC-2.2** | $\ge 99\%$ of injected 802.15.4 frames logged on channels 11–26 | Planned |
| **R-3.1** (I2C Fault Injection)| `UC-3.1` (Suppress I2C ACK) | `fuzzer_inject_i2c_nack()` | **TC-3.1** | Target ACK converted to NACK; line released $\le 50\,\mu\text{s}$ | Planned |
| **R-3.1** (UART Bit Flip) | `UC-3.2` (Invert UART Bit) | `fuzzer_invert_uart_bit()` | **TC-3.2** | Target bit inverted in-flight; no residual bus drive | Planned |
| **R-3.2** (Wireless Stress) | `UC-3.3` (Inject Deauth Frame)| `wifi_80211_tx_deauth()` | **TC-3.3** | Deauth emitted $\le 100\text{ ms}$ after arm; victim re-associates (see A6) | Planned |
| **ER-1** (Emergency Stop) | `UC-4.1` (E-Stop Abort) | `safety_abort_to_high_z()` | **TC-4.1** | All probe lines High-Z $\le 5\text{ ms}$ after switch contact | Planned |
| **R-4.1** (Capture Logging) | `UC-5.1` (Write Capture Record) | `capture_write_frame()` | **TC-4.2** | Written file opens in Wireshark; byte-exact vs. source | Planned |
| **R-5.1** (Telemetry Stream) | `UC-5.2` (Stream Telemetry) | `ws_broadcast_telemetry()` | **TC-5.1** | Dashboard refresh $500$–$1000\text{ ms}$; fault alert $\le 200\text{ ms}$ | Planned |
| **NFR-1** (E-Stop $\le 5\text{ ms}$)| `UC-4.1` (E-Stop Latency) | `safety_abort_to_high_z()` | **TC-4.1** | Measured preemption $\le 5\text{ ms}$ on oscilloscope | Planned |
| **NFR-2** (Zero Dynamic Heap)| `UC-1.1` (Memory Allocation)| `tk_get_mpf()` / `tk_rel_mpf()` | **TC-4.3** | 0 heap allocations over $\ge 10\text{ min}$ run | Planned |
| **NFR-3** (Burst Reliability) | `UC-2.1` (Queue Reliability) | `ring_buffer_push()` | **TC-2.1** | $\le 0.1\%$ drop at 500 kbps; $\ge 32$-frame burst buffered | Planned |
| **NFR-4** (Voltage Clamp) | `UC-1.1` (Electrical Clamping)| Level Shifter (`D7` probe driver HW) | **TC-1.4** | $\le 3.6\text{ V}$ at MCU pad with $5.5\text{ V}$ applied input | Planned |
| **NFR-5** (Jitter $< 500\text{ ns}$) | `UC-3.1` (Glitch Precision) | `timer_sync_glitch()` | **TC-3.1** | Edge-to-pulse jitter $< 500\text{ ns}$ on oscilloscope | Planned |
