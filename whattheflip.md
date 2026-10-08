# whattheflip: Embedded Multi-Bus Sniffer, Wireless Monitor & Chaos Fuzzer

**Module:** INF2004 Embedded Systems Programming  
**Group:** SE06 (Lab P1)  
**Target Platform:** ESP32-C6 RISC-V + Raspberry Pi Pico  
**Operating System:** $\mu\text{T-Kernel}$ 3.0 (`esp32c6-mtk3`)  
**Document Purpose:** Unified Master Knowledge Base & Week 6 Architecture Freeze Specification  

---

## 1. Executive Summary: What is `whattheflip`?

`whattheflip` is an **Embedded Hardware-in-the-Loop (HIL) Resilience & Diagnostic Testbed**—combining physical bus sniffing, multi-protocol over-the-air monitoring, and non-destructive chaos fault injection into a portable, benchtop platform running **$\mu\text{T-Kernel}$ 3.0**.

To eliminate reliance on external uncontrolled signals and definitively substantiate technical scope to course evaluators, the project is architected as a **3-node closed-loop testbed**:

**Figure 1: Hardware Interconnect & Maker Pi Base Pinout**
![Figure 1: Hardware Interconnect & Maker Pi Base Pinout](diagrams/hardware_interconnect.png)

### The Three Operational Pillars:
1. **Node 1: The Diagnostic Tool (ESP32-C6):** Runs $\mu\text{T-Kernel}$ 3.0 on a 32-bit RISC-V core @ 160 MHz. It performs passive physical bus sniffing (UART, I2C, SPI), promiscuous wireless monitoring (Wi-Fi 6, 802.15.4 Zigbee), and active non-destructive chaos fault injection.
2. **Node 2: The Wired Victim (Raspberry Pi Pico):** Dedicated target hardware running operational victim firmware. It drives active UART transmissions, I2C sensor transactions reading a physical GY-511 accelerometer, and SPI peripheral exchanges. It provides the physical target for auto-discovery and in-flight glitch testing, outputting its error detection and recovery statistics directly to a serial monitor.
3. **Node 3: The Wireless Victim (ESP32-C6 Node B):** Dedicated over-the-air target device running an isolated Wi-Fi SoftAP and an IEEE 802.15.4 Zigbee transmitter. It acts as an uncorrupted RF signal source for promiscuous packet capture and wireless stress testing.

---

## 2. Supported Protocol Scope (3 Wired + 2 Wireless)

| Wired Protocols (via Level Shifter) | Wireless Protocols (2.4 GHz RF) |
|---|---|
| **1. UART** (9600 to 115200 baud) | **1. Wi-Fi 6** (IEEE 802.11ax/b/g/n) |
| **2. I2C** (100 kHz & 400 kHz) | **2. IEEE 802.15.4** (Zigbee 3.0 / Thread) |
| **3. SPI** (Mode 0, up to 10 MHz) | *(BLE explicitly excluded)* |

### Protocol Scoping Decision: Bluetooth (BLE) Omission
* **Architectural Justification:** ESP-IDF provides no native link-layer promiscuous sniffing API for connected Bluetooth Low Energy (`esp_ble_set_promiscuous` does not exist). BLE uses 37 pseudo-randomly hopping data channels that standard ESP32-C6 baseband hardware cannot intercept without specialized sniffing silicon. Furthermore, initializing the NimBLE stack consumes $\sim 200\text{ KB}$ Flash and $\sim 45\text{ KB}$ SRAM, degrading memory buffers for $\mu\text{T-Kernel}$ tasks.
* **Decision:** Bluetooth is **explicitly excluded** from the capture scope. Wireless monitoring focuses exclusively on **Wi-Fi 6 (802.11ax)** and **IEEE 802.15.4 (Zigbee)** where native hardware promiscuous frame capture (`esp_wifi_set_promiscuous` and `esp_ieee802154_enable_promiscuous`) provides complete, raw link-layer frame access compatible with standard Wireshark PCAP encapsulation.

---

## 3. Team Subsystem Ownership (5 Buddies)

| Buddy | Name | Subsystem Responsibility | Deliverable / Hardware Boundary |
|---|---|---|---|
| **Buddy 1** | Tan Yan Qi | **Wired Protocol Auto-Discovery Engine** | Passive 4-pin edge timing analysis, baud rate estimation ($9600–115200\text{ baud} \pm 2\%$), open-drain vs. push-pull classification, and dynamic pin mapping on the Wired Victim. |
| **Buddy 2** | Ryan Ke | **Physical Chaos & Fault Injection Engine** | Frame-synchronized glitch injection against the Wired Victim (single bit-inversion, I2C ACK suppression, clock stretching up to $50\,\mu\text{s}$) with instantaneous hardware E-stop release. |
| **Buddy 3** | Zayne Lee | **Wireless Multi-Protocol Sniffer Engine** | Promiscuous RF capture of 802.11ax management frames and IEEE 802.15.4 raw packets from the Wireless Victim, extracting RSSI, channel, and microsecond timestamps. |
| **Buddy 4** | Annie Goh | **Telemetry Ingestion & PCAP Storage Engine** | Zero-copy fixed memory ring buffers, packet serialization into standard `libpcap` format (`.pcap`), and high-speed FAT32 MicroSD card logging. |
| **Buddy 5** | Joseph Poon | **System Supervisor, Web UI & Victim Control** | $\mu\text{T-Kernel}$ supervisory task coordination, WebSocket live streaming dashboard over Wi-Fi, hardware E-stop interrupt handling, NeoPixel/buzzer status indicators, and victim stimulus coordination. |

---

## 4. System Requirements

### 4.1 Functional Requirements (FR)
*Requirements specify purely external observable behavior ("what") and tolerances without restricting internal implementation:*
* **FR1 (Passive Signal Acquisition):** The system shall passively acquire digital signals across 4 probe pins with an input leakage current $\le 1\,\mu\text{A}$ ($R_{in} \ge 1\text{ M}\Omega$) and a timing resolution $\le 1\,\mu\text{s}$ (backed by $\mu\text{T-Kernel}$ 1 MHz physical timer `tk_*ptmr`).
* **FR2 (Automated Protocol Classification):** The auto-discovery engine shall classify standard UART ($9600\text{ to }115200\text{ baud} \pm 2\%$), standard I2C ($100\text{ kHz and }400\text{ kHz}$), and SPI Mode 0 (up to $1\text{ MHz}$) signals within $\le 500\text{ ms}$ of continuous bus activity.
* **FR3 (Dynamic Signal Routing):** The system shall programmatically map any of the 4 probe pins to internal protocol decoding channels without requiring physical jumper wire alterations.
* **FR4 (Over-the-Air Wireless Capture):** The wireless engine shall capture raw 802.11 management frames and raw IEEE 802.15.4 frames across 2.4 GHz channels 11–26, extracting received signal strength (RSSI), channel ID, and microsecond timestamps.
* **FR5 (Physical Bus Fault Injection):** When armed by the user, the chaos engine shall inject non-destructive protocol faults into the target bus, specifically single bit-inversions, I2C ACK $\to$ NACK suppressions, and clock stretching up to $50\,\mu\text{s} \pm 10\%$.
* **FR6 (Wireless Fault Injection):** The system shall transmit targeted over-the-air stress frames (including 802.11 deauthentication frames) to trigger reconnection routines on the target wireless node.
* **FR7 (Emergency Isolation):** Actuation of the physical emergency stop button shall disarm all active output drivers and return all probe lines to high-impedance mode within $\le 5\text{ ms}$.
* **FR8 (Standard Capture Export):** The storage engine shall write captured packet records containing microsecond timestamps and raw packet bytes to non-volatile removable media in a format parseable by standard network analysis software.
* **FR9 (Real-Time Telemetry Stream):** The system shall broadcast decoded packet summaries and bus error statistics over a wireless interface to connected client dashboards at a periodic refresh rate of $1\text{ Hz to }2\text{ Hz}$ ($500\text{ ms to }1000\text{ ms}$), with event-driven immediate alerts for fault states.

### 4.2 Non-Functional Requirements (NFR)
* **NFR1 (Real-Time Preemption):** Emergency stop and safety disarm routines shall preempt all background processing tasks within $\le 5\text{ ms}$ of physical switch contact.
* **NFR2 (Deterministic Memory Operation):** The firmware shall perform zero dynamic heap allocations (`malloc`/`free`) during active monitoring and fault injection phases.
* **NFR3 (Packet Ingestion Reliability):** The capture pipeline shall achieve $0\%$ packet drop rate for continuous wired streams up to $500\text{ kbps}$ (covering UART 115200 and I2C Fast Mode), and buffer wireless bursts of up to 32 consecutive frames without data loss.
* **NFR4 (Over-Voltage Protection):** The physical probe interface shall tolerate continuous input voltages up to $5.5\text{ V}$ without exceeding the microcontroller pad maximum rating of $3.6\text{ V}$.
* **NFR5 (Fault Timing Precision):** Fault injection pulses shall synchronize to target bus trigger edges with a timing jitter of $< 500\text{ ns}$ (well within the $8.68\,\mu\text{s}$ bit window of 115200 baud UART).

---

## 5. Hardware Integration & Pinout Layout

### 5.1 Node 1: Diagnostic Tool (ESP32-C6 on Cytron Maker Pi Base)

The diagnostic tool consists of the **Waveshare ESP32-C6-Pico-M** mounted directly onto the **Cytron Maker Pi Pico Base** carrier board:
* **Integrated MicroSD Slot:** Utilizes the onboard spring-push MicroSD socket hardwired to physical pins 14–16, eliminating external breakout boards.
* **Per-Pin Real-Time LED Diagnostics:** Every GPIO header pin features an onboard blue LED indicator, providing immediate physical feedback when probe lines and SPI buses toggle.
* **I2C Bus Safety Guard:** On the ESP32-C6, physical pins 26 and 27 connect to native `GPIO22` (SDA) and `GPIO23` (SCL), which drive the onboard TCA9554PWR expander. The team mandates using the **native `BOOT` pushbutton on Pin 12 (`GPIO9`)** for Emergency Stop, strictly banning Maker Pi Base Buttons 1 & 2 (GP20/GP21) which would short the expander's I2C bus to ground.

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
| **Pin 24 (GP18)** | `BUZZER_OUT` | **EXIO3 / GPIO15**| PWM / Audio | `Safety_Supervisor_Task` | Maker Pi Base onboard piezo buzzer (Pin 24) or external resonant buzzer on GPIO15. |
| **Pins 26 / 27** | `EXP_I2C` | **GPIO22 / 23** | I2C Master Bus | System Init | Dedicated to onboard TCA9554PWR expander. **Baseboard Buttons 1 & 2 must NOT be pressed**. |
| **Pin 36 (3V3)** | `VCC_LV` | **3V3 OUT** | Power Output | Hardware Interconnect | 3.3V reference power to low-voltage side of TXS0104E level shifter. |
| **Pin 38 (GND)** | `GND` | **GND** | Ground Plane | Hardware Interconnect | Common reference ground unified across Maker Pi Base, Level Shifter, and Pico Victim. |

### 5.2 Node 2: Wired Victim (Raspberry Pi Pico)
* **UART Channel:** GP0 (TX) and GP1 (RX) connected to Shifter HV0 and HV1 $\to$ `PROBE_0` and `PROBE_1`.
* **I2C Channel:** GP4 (SDA) and GP5 (SCL) connected to physical GY-511 sensor and Shifter HV2 and HV3 $\to$ `PROBE_2` and `PROBE_3`.
* **SPI Channel:** GP16 (MISO), GP19 (MOSI), GP18 (SCK), GP17 (CS) wired for multi-wire SPI tests.
* **Telemetry Output:** USB CDC serial port reporting packet error counts, CRC failures, and recovery latencies to the workstation.

### 5.3 Node 3: Wireless Victim (ESP32-C6 Node B)
* Independent wireless target node operating on 2.4 GHz RF.
* Transmits isolated 802.11ax SoftAP beacon frames and periodic IEEE 802.15.4 data bursts on Channel 15.

---

## 6. $\mu\text{T-Kernel}$ 3.0 Task Architecture

| Task Identifier | Task Name | Priority | Trigger Condition | Execution Budget | Purpose |
|---|---|:---:|---|---|---|
| `TSK_SAFETY` | `Safety_Supervisor_Task` | **1 (Highest)** | E-Stop GPIO Interrupt / Fault | $< 2\text{ ms}$ | Immediate hardware disarm, High-Z isolation, audio-visual alarm. |
| `TSK_WIRELESS` | `Wireless_Engine_Task` | **2** | Radio Promiscuous RX Event | $< 5\text{ ms}$ | Drains hardware RF FIFOs (Wi-Fi/Zigbee), parses metadata. |
| `TSK_FUZZER` | `Chaos_Fuzzer_Task` | **3** | Frame Sync Trigger Event | $< 1\text{ ms}$ (Critical) | Injects synchronized bit-flips, ACK suppression, clock stretching. |
| `TSK_DISCOVERY`| `AutoDiscovery_Task` | **4** | Periodic (50 ms) / Edge Event | $< 10\text{ ms}$ | Samples edge transitions, computes pulse statistics, maps pins. |
| `TSK_STORAGE` | `Storage_Task` | **5** | Memory Pool Queue Event | $< 25\text{ ms}$ | Flushes packet ring buffers to FAT32 `.pcap` files on MicroSD. |
| `TSK_DASHBOARD`| `Web_Dashboard_Task` | **6 (Lowest)** | Periodic (500–1000 ms) | $< 30\text{ ms}$ | Broadcasts live WebSocket telemetry and handles UI commands. |
### Priority Justification:
* `TSK_SAFETY` has Priority 1 (highest) to enforce the invariant that target protection overrides all processing: the E-stop interrupt must instantly drop outputs to High-Z within $\le 5\text{ ms}$.
* `TSK_WIRELESS` has Priority 2 to prevent hardware radio FIFO overruns during dense 802.11 beacon bursts.
* `TSK_FUZZER` has Priority 3 to ensure sub-microsecond glitch injection timing when a target trigger pattern is matched.
* `TSK_DISCOVERY` has Priority 4 to perform edge timing calculations without interrupting active packet reception.
* `TSK_STORAGE` and `TSK_DASHBOARD` occupy lowest priorities (5 and 6) because disk and network operations are fully decoupled via pre-allocated memory buffers, ensuring slow MicroSD writes never stall real-time bus acquisition.

---

## 7. Task Interaction & IPC Architecture

Inter-task communication is strictly decoupled using native $\mu\text{T-Kernel}$ 3.0 synchronization primitives:

**Figure 2: µT-Kernel 3.0 Task Interaction & IPC Architecture**
![Figure 2: µT-Kernel 3.0 Task Interaction & IPC Architecture](diagrams/task_interaction.png)

---

## 8. Statechart Architecture

### 8.1 Unified System-Level Statechart

*Addressing the evaluator feedback to the senior: one single unified formal model integrating nominal and exception flows:*

**Figure 3: Unified System-Level Statechart**
![Figure 3: Unified System-Level Statechart](diagrams/system_statechart.png)

#### Main Operational States:
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

### 8.2 Subsystem Statecharts

#### Subsystem Statechart 1: Wired Auto-Discovery Engine
* **Related Task:** `AutoDiscovery_Task` (`TSK_DISCOVERY`)
* **Purpose:** Passively classify unknown bus protocols and resolve pin roles.
* **Why a Statechart is Needed:** Prevents invalid state progression (e.g. attempting to decode baud rates before verifying bus activity) and ensures noisy lines timeout cleanly.

**Figure 4: Subsystem 1 — Wired Auto-Discovery Statechart**
![Figure 4: Subsystem 1 — Wired Auto-Discovery Statechart](diagrams/subsystem_autodiscovery.png)
* **Fault Handling:** If edge activity does not match standard protocol timings within $2\text{ s}$, transitions to `DISC_TIMEOUT`, notifies the dashboard, and returns to `DISC_LISTEN`.

---

#### Subsystem Statechart 2: Chaos Fuzzing Engine
* **Related Task:** `Chaos_Fuzzer_Task` (`TSK_FUZZER`)
* **Purpose:** Safely inject synchronized faults into active target transmissions.
* **Why a Statechart is Needed:** Enforces strict hardware safety guards so outputs are never held driven longer than a single bit period.

**Figure 5: Subsystem 2 — Chaos Fuzzing Engine Statechart**
![Figure 5: Subsystem 2 — Chaos Fuzzing Engine Statechart](diagrams/subsystem_fuzzer.png)
* **Fault Handling:** If a line remains driven or held LOW for $> 100\,\mu\text{s}$, a hardware timeout triggers `FAULT_CONTENTION`, immediately floating all probe pins to protect target hardware.

---

#### Subsystem Statechart 3: Wireless Multi-Protocol Sniffer
* **Related Task:** `Wireless_Engine_Task` (`TSK_WIRELESS`)
* **Purpose:** Ingest raw RF packets and extract physical radio metadata.
* **Why a Statechart is Needed:** Manages radio baseband state transitions and prevents buffer overruns during high-rate 802.11 beacon bursts.

**Figure 6: Subsystem 3 — Wireless Multi-Protocol Sniffer Statechart**
![Figure 6: Subsystem 3 — Wireless Multi-Protocol Sniffer Statechart](diagrams/subsystem_wireless.png)
* **Fault Handling:** If the internal packet pool reaches $> 85\%$ capacity, selectively drops non-essential broadcast data frames while preserving management and control frames, logging an overrun counter to the telemetry stream.

---

## 9. Test Plan with Explicit Expected Results and Tolerances

*Addressing evaluator feedback: every test case contains objective binary pass/fail criteria with explicit tolerances:*

| Test ID | Test Category | Setup / Stimulus | Expected Result | Pass/Fail Criteria |
|---|---|---|---|---|
| **TC-1.1** | UART Passive Auto-Discovery | Wired Victim sends 115200 baud UART on GP0. Probes connected to GP0–GP3. | Sniffer classifies UART, identifies TX pin, and locks baud rate to $115200 \pm 2\%$. | Pass if detected within $\le 500\text{ ms}$; Fail if timeout or wrong protocol. |
| **TC-1.2** | I2C Passive Auto-Discovery | Wired Victim reads GY-511 sensor on GP4/GP5 at 100 kHz. Probes connected to GP0–GP3. | Sniffer classifies I2C, identifies SCL and SDA pins, confirms clock period $= 10.0\,\mu\text{s} \pm 25\%$. | Pass if SCL/SDA identified correctly in $\le 500\text{ ms}$; Fail if misclassified. |
| **TC-1.3** | Global Pin Filter Exclusivity | Probes connected to 3 simultaneously toggling lines (UART + I2C). | Sniffer classifies protocol as `UNKNOWN` and aborts classification. | Pass if system rejects ambiguous mixed-signal input; Fail if false positive returned. |
| **TC-2.1** | I2C ACK Suppression Fuzzing | Victim actively reading GY-511. Fuzzer configured to force SDA HIGH on byte 2 ACK. | Victim logs `[I2C NACK]` on serial monitor, executes bus recovery, and resumes sampling. | Pass if victim reports NACK and retries; Fail if victim hangs or fuzzer misses clock. |
| **TC-2.2** | UART In-Flight Bit Flip | Victim sends telemetry packets with CRC-16. Fuzzer inverts bit 4 of byte 8. | Victim logs `[CHECKSUM MISMATCH]` and drops corrupted frame without system crash. | Pass if exactly 1 frame dropped per trigger; Fail if line locked LOW. |
| **TC-3.1** | Wi-Fi 6 Promiscuous RX | Wireless Victim broadcasts "VICTIM_AP" beacons. Sniffer listens on Channel 6. | Sniffer captures $\ge 98\%$ of transmitted beacons, logging RSSI, channel, and microsecond timestamps. | Pass if packet capture rate $\ge 98\%$; Fail if packet loss $> 2\%$. |
| **TC-3.2** | 802.15.4 Zigbee Capture | Wireless Victim transmits raw 802.15.4 data frames on Channel 15. | Sniffer captures frame, validates 2-byte FCS, extracts LQI. | Pass if FCS valid and payload matches; Fail if CRC error. |
| **TC-4.1** | Hardware E-Stop Safety | Fuzzer actively driving probe pins during chaos mode. Physical E-stop pressed. | Probe pins switch to High-Z (leakage current $< 1\,\mu\text{A}$) within $\le 5\text{ ms}$. | Pass if pin floats in $\le 5.0\text{ ms}$; Fail if line remains driven $> 5.0\text{ ms}$. |
| **TC-4.2** | PCAP Binary Integrity | Sniffer captures 100 physical and wireless frames. MicroSD card ejected and read on PC. | Wireshark opens `.pcap` file without parsing errors, displaying valid 802.11 / UART dissections. | Pass if Wireshark reports 0 packet header decode errors; Fail if file corrupt. |
| **TC-5.1** | WebSocket Stream Rate | Sniffer connected to browser dashboard over local Wi-Fi. Stream active. | Dashboard receives JSON telemetry updates at a sustained rate of 1–2 Hz (500–1000 ms). | Pass if telemetry update rate is 1–2 Hz; Fail if rate drops below 1 Hz. |

---

## 10. Lecture 6.1 Traceability Matrix (V-Model Verification)

*Formatted strictly to the 5-column standard from Lecture 6.1 Slide #5:*

$$\text{Requirement ID} \longrightarrow \text{Use Case / Sequence ID} \longrightarrow \text{LLD Function Name} \longrightarrow \text{Test ID} \longrightarrow \text{Verification Status}$$

| Requirement ID | Use Case / Sequence ID | LLD Function Name | Test ID | Verification Status |
|---|---|---|---|:---:|
| **FR1** (Passive Probing) | `UC-1.1` (Sample Bus Lines) | `probe_sample_edges()` | **TC-1.1** | Planned (Architecture Freeze) |
| **FR2** (Auto-Discovery) | `UC-1.2` (Classify Protocol) | `analyze_for_i2c()`, `analyze_for_uart()` | **TC-1.2** | Planned (Architecture Freeze) |
| **FR3** (Dynamic Reconfig) | `UC-1.3` (Map Peripheral Pins) | `gpio_matrix_route_pins()` | **TC-1.1** | Planned (Architecture Freeze) |
| **FR4** (Wireless Capture) | `UC-2.1` (Capture 802.11 Frames)| `wifi_promiscuous_rx_cb()` | **TC-3.1** | Planned (Architecture Freeze) |
| **FR4** (Zigbee Capture) | `UC-2.2` (Capture 802.15.4) | `ieee802154_rx_cb()` | **TC-3.2** | Planned (Architecture Freeze) |
| **FR5** (I2C Fault Injection)| `UC-3.1` (Suppress I2C ACK) | `fuzzer_inject_i2c_nack()` | **TC-2.1** | Planned (Architecture Freeze) |
| **FR5** (UART Bit Flip) | `UC-3.2` (Invert UART Bit) | `fuzzer_invert_uart_bit()` | **TC-2.2** | Planned (Architecture Freeze) |
| **FR6** (Wireless Stress) | `UC-3.3` (Inject Deauth Frame)| `wifi_inject_deauth_frame()` | **TC-3.1** | Planned (Architecture Freeze) |
| **FR7** (Emergency Stop) | `UC-4.1` (E-Stop Abort) | `safety_abort_to_high_z()` | **TC-4.1** | Planned (Architecture Freeze) |
| **FR8** (PCAP SD Logging) | `UC-5.1` (Write PCAP Record) | `pcap_write_frame()` | **TC-4.2** | Planned (Architecture Freeze) |
| **FR9** (WebSocket Stream) | `UC-5.2` (Stream Telemetry) | `ws_broadcast_telemetry()` | **TC-5.1** | Planned (Architecture Freeze) |
| **NFR1** (E-Stop $\le 5\text{ ms}$)| `UC-4.1` (E-Stop Latency) | `safety_abort_to_high_z()` | **TC-4.1** | Planned (Architecture Freeze) |
| **NFR2** (Zero Dynamic Heap)| `UC-1.1` (Memory Allocation)| `tk_get_mpf()` | **TC-4.2** | Planned (Architecture Freeze) |
| **NFR3** (Burst Reliability) | `UC-2.1` (Queue Reliability) | `ring_buffer_push()` | **TC-3.1** | Planned (Architecture Freeze) |
| **NFR4** (Voltage Clamp) | `UC-1.1` (Electrical Clamping)| Hardware Level Shifter | **TC-1.1** | Planned (Architecture Freeze) |
| **NFR5** (Jitter $< 500\text{ ns}$) | `UC-3.1` (Glitch Precision) | `timer_sync_glitch()` | **TC-2.1** | Planned (Architecture Freeze) |

---

## 11. Assumptions & Pre-Integration Validation Matrix

| ID | Technical Assumption | Operational Risk if Wrong | Empirical Validation Method | Owner | Status |
|---|---|---|---|---|---|
| **A1** | Probe inputs resolve clean CMOS logic thresholds ($V_{IL} \le 0.8\text{ V}, V_{IH} \ge 2.0\text{ V}$) without excessive ringing on $15\text{ cm}$ jumper lines. | Edge noise triggers spurious interrupts, causing false baud rate detection. | Capture 100 kHz I2C transitions on an oscilloscope with 15 cm leads; verify rise time $t_r \le 300\text{ ns}$ and no false trigger interrupts. | Tan Yan Qi | Open |
| **A2** | ESP32-C6 GPIO pins can switch direction from High-Z Input to Driven Output in $< 1\,\mu\text{s}$ for targeted bit glitching. | Delayed output drive misses the target clock pulse, failing to inject the fault. | Program a GPIO toggle triggered by an external pulse; measure drive delay on an oscilloscope with $< 50\text{ ns}$ resolution. | Ryan Ke | Open |
| **A3** | The ESP32-C6 promiscuous packet filter captures raw 802.11 management frames without dropping frames at burst rates up to 50–100 pkts/s. | Dropped beacon frames cause incomplete Wireshark trace exports. | Blast the sniffer with a 100 pkt/s beacon generator (Node 3); compare transmitted vs. logged frame sequence counters. | Zayne Lee | Open |
| **A4** | SPI FAT32 MicroSD write latency spikes ($\le 40\text{ ms}$) can be fully absorbed by an 8 KB pre-allocated packet ring buffer. | Buffer overflow during SD write stalls drops incoming physical/wireless packets. | Simulate a 50 ms SD write stall while streaming UART traffic at 115200 baud; verify zero dropped bytes in memory logs. | Annie Goh | Open |
| **A5** | The WebSocket telemetry server can stream 1–2 Hz packet updates over Wi-Fi without starving the $\mu\text{T-Kernel}$ scheduler. | Network stack CPU load introduces jitter into physical bus sampling. | Benchmark system timer jitter while serving 3 concurrent WebSocket clients running telemetry streams at 1 Hz. | Joseph Poon | Open |

---

## 12. AI-Supported Design Evidence (Section 8 Template Compliance)

### 12.1 Pre-Submission AI Ideation
* **Prompt Used:** *"Propose an RTOS architecture for a combined physical bus sniffer and wireless monitor on an ESP32-C6, considering task preemption and storage bottlenecks."*
* **Useful Ideas Adopted:** Decoupling high-frequency radio and probe ISRs from the SD card logger using pre-allocated fixed memory pools (`MPF`) and message buffers rather than writing directly from interrupts.
* **Unrealistic Ideas Rejected:** AI suggested implementing full on-chip protocol decoding for USB 2.0 High-Speed. This was rejected because USB 480 Mbps signaling far exceeds the ESP32-C6 GPIO sampling bandwidth ($40\text{ MHz}$ Nyquist limit).

### 12.2 Team-Reviewed Architecture Freeze Decisions
1. **Selection of 3-Node Closed-Loop Testbed:** Rather than testing against random uncontrolled devices, building dedicated Wired (Pico) and Wireless (ESP32-C6) victims provides reproducible ground truth to rigorously prove technical scope to evaluators.
2. **Mounting on Cytron Maker Pi Pico Base:** Eliminates external MicroSD breakout module by utilizing the baseboard's onboard spring-push SD slot; provides per-pin blue LED diagnostics and clean benchtop mounting.
3. **Exclusion of Physical OLED Screen:** A 0.96" I2C display was dropped to conserve 1 KB of RAM, prevent I2C bus contention, and eliminate display-related task blocking. Telemetry is streamed over Wi-Fi to a responsive laptop web dashboard.
4. **Mandatory 3.3V/5V Bidirectional Level Shifter:** External target buses may operate at 5V logic (e.g., standard Arduino boards). Direct connection would destroy the ESP32-C6 3.6V-tolerant pads; a level shifter was mandated as an architectural invariant.
5. **Omission of Bluetooth (BLE) Promiscuous Mode:** Avoids unfulfillable claims about raw link-layer sniffing, saves 45 KB SRAM, and keeps wireless capture focused on native Wi-Fi 6 + 802.15.4 Zigbee.

### 12.3 Final AI-Vetted Gap Check
* **Gap Identified by AI:** Simultaneous continuous capture across Wi-Fi and 802.15.4 is physically impossible on a single RF front-end.
  * **Team Response:** *Accepted as Risk / Documented Design Constraint.* The ESP32-C6 features one shared 2.4 GHz radio. The system implements time-multiplexed capture modes rather than true simultaneous multi-protocol RF reception.
* **Gap Identified by AI:** Maker Pi Base buttons 1 & 2 (GP20/21) short the TCA9554 expander I2C bus to GND.
  * **Team Response:** *Fixed.* Mandated using the native BOOT switch on Pin 12 (`GPIO9`) for Emergency Stop, strictly banning baseboard buttons 1 & 2.
* **Gap Identified by AI:** Active fault injection can induce bus lockup if the target holds lines low.
  * **Team Response:** *Fixed.* Added a hardware timeout to the Chaos Fuzzer state machine that forces High-Z if the bus is held low for $> 100\,\mu\text{s}$, paired with the physical E-stop interrupt.

---

## 13. Design Consistency Audit (Section 9 Template Compliance)

* **1. Which task owns each hardware device?**
  * `AutoDiscovery_Task` and `Chaos_Fuzzer_Task` dynamically share the 4 target probe lines on Maker Pi Pins 1, 2, 4, 5 (ESP32-C6 `GPIO16`, `GPIO17`, `GPIO4`, `GPIO5`), arbitrated via `MTX_PROBE_ACCESS`.
  * `Wireless_Engine_Task` owns the 2.4 GHz radio transceiver baseband.
  * `Storage_Task` owns the Maker Pi onboard MicroSD card slot hardwired to physical pins 14–16 and 20 (ESP32-C6 `GPIO18`, `GPIO19`, `GPIO20`, and `EXIO7`).
  * `Safety_Supervisor_Task` owns the emergency stop button on Maker Pi Pin 12 (`GPIO9` BOOT switch), status NeoPixel (`GPIO8`), and alert buzzer (`EXIO3` / `GPIO15`).
  * `Web_Dashboard_Task` owns the Wi-Fi AP network socket interface.
* **2. Which task has the highest priority, and why?**
  * `Safety_Supervisor_Task` has the highest priority (Priority 1) because target hardware safety is the primary system invariant. If an electrical short occurs or the user presses the emergency stop, the system must immediately preempt all other tasks and drop probe pins to High-Z in $\le 5\text{ ms}$.
* **3. Which subsystem statechart corresponds to which RTOS task or subsystem?**
  * Subsystem Statechart 1 corresponds to `AutoDiscovery_Task` (`TSK_DISCOVERY`).
  * Subsystem Statechart 2 corresponds to `Chaos_Fuzzer_Task` (`TSK_FUZZER`).
  * Subsystem Statechart 3 corresponds to `Wireless_Engine_Task` (`TSK_WIRELESS`).
* **4. How are functional requirements reflected in the task and statechart design?**
  * FR1–FR3 (Probing, Auto-Discovery, Pin Routing) map to `AutoDiscovery_Task` and the `AUTO_DISCOVERY` state.
  * FR4 (Wireless Capture) maps to `Wireless_Engine_Task` and the `PASSIVE_SNIFF` state.
  * FR5–FR6 (Chaos Fault Injection) map to `Chaos_Fuzzer_Task` and the `CHAOS_FUZZING` state.
  * FR7 (Emergency Stop) maps to `Safety_Supervisor_Task` and the `FAULT_ESTOP` state.
  * FR8–FR9 (PCAP Storage and Web Streaming) map to `Storage_Task` and `Web_Dashboard_Task`.
* **5. What changes are still expected after the Week 6 review?**
  * Confirmation of empirical rise-time tolerances during Week 8 hardware wiring tests.
  * Fine-tuning of SPI DMA block transfer sizes and ring buffer depths based on measured SD write latency benchmarks.
  * Validation of assumptions A1 through A5 using physical oscilloscope and network traffic generator measurements before Week 10 Subsystems Check-in.
