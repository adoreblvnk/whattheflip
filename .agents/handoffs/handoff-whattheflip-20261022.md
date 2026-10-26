# Handoff: whattheflip (INF2004 SE06) — 2026-10-22

Working dir: /Users/adoreblvnk/Documents/school/INF2004/whattheflip
Read first: /Users/adoreblvnk/Documents/school/INF2004/AGENTS.md (prof profiles + "How the human wants work done") and whattheflip/AGENTS.md (canon names, layout, tone). Follow them strictly.

## Where things stand

- `whattheflip.md` (living source of truth): fully reconciled against ground truth. Boards renamed: whattheflip (tool), hestia = old victim A (uart tx only), daphne = old victim B (uart rx + gy-511 i2c master 400 kHz, sensor wired to daphne exclusively), iris = wireless victim (softap ch 6, bssid 02:00:00:00:00:03, 802.15.4 ch 15). Requirement IDs normalized to FR-1..FR-10 (house style: hyphenated, matching frozen tribunal docs).
- Iris (wireless victim) was NOT on bench this session. No detection, no firmware, no test. Untested.
- `whattheflip/AGENTS.md` updated with Maker Pi Base socket remap rule: ESP32-C6 pins do not map 1-to-1 to Maker Pi Base labels. SD SCK=GPIO18, MOSI=GPIO19, MISO=GPIO20, CS=EXIO7 (TCA9554 I2C expander at 0x20 on GPIO22/GPIO23, NOT a native GPIO). Maker Pi buttons 1 & 2 (GP20/GP21) tie to GPIO22/GPIO23 and short the TCA9554 bus.
- Root `AGENTS.md` updated with one-line pin mapping rule.

## Bench session 2026-10-22: hardware detection + first fuzz

### Devices on bench
- whattheflip: Waveshare ESP32-C6-Pico on Cytron Maker Pi Base, USB-C to dock, `/dev/cu.usbmodem101`.
- Hestia: Raspberry Pi Pico (RP2040) on Cytron Maker Pi Base, Micro-USB to dock, `/dev/cu.usbmodem11201`. Serial `E66430A64B586C38`.
- Daphne: Raspberry Pi Pico (RP2040) on Cytron Maker Pi Base, Micro-USB to dock, `/dev/cu.usbmodem11101`. Serial `E66480454F73A231`.
- GY-511 (LSM303DLHC) on breadboard, powered by Daphne 3.3V (W4). Accel at 0x19, mag at 0x1E.
- Delkin USDCOEM-32GB microSD in Maker Pi Base slot.
- MacBook Air (USB-C only) → USB dock → all devices. No direct USB-A ports available.

### ESP32-C6 detection
- Chip: ESP32-C6FH4 (QFN32, revision v0.2), 4 MB embedded flash, Wi-Fi 6 / 802.15.4 / BLE 5.
- Base MAC: `8c:fd:49:0d:18:40`. 128-bit UUID: `42:4f:4c:04:b2:b0:4b:89:25:e4:71:cd:ea:0f:87:55`.
- Secure Boot: Disabled. Flash Encryption: Disabled. JTAG: Enabled.
- Flash partition table at 0x8000: nvs, otadata, app0 (0x10000), app1 (0x150000).
- Pre-existing firmware: `WHATTHEFLIP_PIN_VERIFY_V2` (compiled Oct 8 2026, Arduino 3.3.11 / ESP-IDF 5.5.5). Backed up to `/tmp/esp32c6_backup_4mb.bin`.
- Boot mode note: ESP32-C6 enters ROM download mode (`boot:0x74`) when GPIO9 (BOOT) is held low. Must release BOOT and press RUN to boot into SPI flash mode (`boot:0x6c`).

### microSD detection — SOLVED
- **Hardware SPI (`SPI.begin()`) does NOT work** on this board. The Maker Pi Base solders an indicator LED on the GP12 (MISO) line to ground. When no card drives MISO, the LED pulls the line to 0V. The ESP32-C6 hardware SPI peripheral samples too fast for the card's output driver to overcome the LED load, reading 0x00 on every byte.
- **Software bitbang SPI works perfectly.** Manual clock toggling with `delayMicroseconds(4)` between edges gives the SD card enough time to drive MISO high past the LED.
- **CS problem**: Maker Pi Base routes SD CS (socket pin 20 / GP15) to the Waveshare board's TCA9554 I2C expander (EXIO7), not a native ESP32 GPIO. The TCA9554 bus (GPIO22 SDA / GPIO23 SCL) is corrupted by Maker Pi Base buttons 1 & 2 (GP20/GP21 tie to GPIO22/GPIO23 with pull-downs). I2C scan shows 119 phantom devices. Writes to TCA9554 do not latch.
- **Fix**: Jumper wire from Maker Pi Base GP15 (pin 20, SD CS) to GP13 (pin 17, routes to ESP32-C6 GPIO21). This gives the C6 direct native GPIO control over CS, bypassing the TCA9554 expander entirely. **This jumper must stay in place.**
- Card detected via bitbang: CMD0→0x01, CMD8 echo `01 AA`, ACMD41 ready in 2 loops.
- Card metadata: Manufacturer 0x27 (Delkin), OEM "PH", Product "SD32G", Rev 6.0, Serial 0x6C5C52DF, SDHC/SDXC, 31,016,878,080 bytes (~28.89 GB).
- Firmware constraint for TSK_STORAGE: must use bitbang SPI, not hardware SPI. This is a real limitation for Ryan's storage task.

### GY-511 detection
- Detected from whattheflip's probes on shared breadboard I2C bus (GPIO6 SDA / GPIO7 SCL).
- Accel at 0x19, mag at 0x1E. Both responded. Idle pins read SDA=1, SCL=1 (3.3V from Daphne confirmed).
- Daphne's I2C polling reads ACCEL/MAG but returns -1 on all axes. The sensor init sequence (CTRL_REG1_A, CTRL_REG4_A, CRA_REG_M, CRB_REG_M, MR_REG_M) may need register address debugging — the bus is alive but data registers read invalid.

### Firmware flashed this session
All sketches are in `/tmp/` (throwaway, not committed to repo):

1. **Hestia** (`/tmp/hestia_uart/hestia_uart.ino`): UART TX burst on GP0 at 115200. Sends `HESTIA:<counter>:<checksum>\n` every 500ms. Also prints to USB CDC for cross-check. FQBN: `rp2040:rp2040:rpipico`. Flash via UF2 copy to `/Volumes/RPI-RP2/`.
2. **Daphne** (`/tmp/daphne_uart_i2c/daphne_uart_i2c.ino`): UART RX on GP1 at 115200 + GY-511 I2C polling on GP4/GP5 at 400 kHz. Prints received UART chars and ACCEL/MAG readings to USB CDC. Wire API: `Wire.setSDA(4); Wire.setSCL(5); Wire.begin();` (RP2040 core uses setSDA/setSCL, not begin(sda,scl,freq)). FQBN: `rp2040:rp2040:rpipico`.
3. **whattheflip** (`/tmp/wtf_sniffer/wtf_sniffer.ino`): Combined UART + I2C sniffer/fuzzer. Probe 1 (GPIO5) bitbang UART RX. Probe 0 (GPIO4) glitch inject. Probe 2 (GPIO6) I2C SDA sniff. Probe 3 (GPIO7) I2C SCL sniff. Modes: 0=sniff only, 1=UART fuzz, 2=I2C fuzz, 3=combined. FQBN: `esp32:esp32:esp32c6:CDCOnBoot=cdc,JTAGAdapter=builtin`.

### S8 Combined Fuzz — PASSED
- whattheflip in mode 3 (combined fuzz), injecting every 2 seconds.
- UART bit flip: 10 µs glitch on GPIO4 (Row 12). Daphne received corrupted byte (`\x00h\x00`).
- I2C ACK kill: drove SDA (GPIO6) high for 50 µs during ACK slot. I2C transaction aborted (START STOP with no data).
- Both faults landed within 5% duty cycle budget.
- I2C bus showed START/STOP activity — Daphne is polling GY-511 and whattheflip sees the transactions.
- Hestia UART messages (`HESTIA:1268:FB0B\n`) received correctly by Daphne in normal mode, corrupted when fuzz fires.

### USB dock issues
- MacBook Air has USB-C only, no USB-A. All devices go through a USB-C dock with GenesysLogic hub.
- Pico UF2 flashing via `dd` to `/Volumes/RPI-RP2/` is slow (~21 kB/s) but works. Hestia (56 KB UF2) took 3s; Daphne (158 KB UF2) took 7.5s.
- Earlier attempts to copy UF2 via `cp` or `shutil` hung indefinitely (macOS I/O layer locked up on the RPI-RP2 drive). Solution: use `dd` with `bs=512` and a timeout.
- ESP32-C6 esptool flashing works fine over the dock at 921600 baud.
- The RPI-RP2 drive can lock up if a write is interrupted. Fix: unplug Pico, hold BOOTSEL, replug.
- Max 2-3 devices on the dock simultaneously. More devices caused enumeration issues.

## Open items
1. **GY-511 reads -1**: Daphne's I2C init sequence for LSM303DLHC needs debugging. Bus is alive (devices respond to scan), but data registers return invalid values. Check: register auto-increment bit (0x80 on OUT_X_L_A), correct register addresses for this specific GY-511 variant.
2. **SD bitbang SPI constraint**: TSK_STORAGE firmware must use bitbang SPI, not hardware SPI, due to Maker Pi MISO LED. Document in `whattheflip.md`.
3. **CS jumper permanent**: GP15→GP13 jumper must stay on the Maker Pi Base. Note in `whattheflip.md` wiring schedule.
4. **TCA9554 unusable for CS**: Maker Pi Base buttons corrupt the I2C bus. If any other EXIO pin is needed, the buttons must be physically disconnected or the pins avoided.
5. **Buzzer EXIO2 vs EXIO3**: handoff from 2026-10-20 item 1 still open. Bench check needed.
6. **A8: RMT RX channel limit**: still open for week 10 check-in.
7. **Firmware not in repo**: all sketches are in `/tmp/`. Need to move into `whattheflip/hestia/`, `whattheflip/daphne/`, `whattheflip/whattheflip/` when starting real firmware.
8. **RP2040 core installed**: `rp2040:rp2040@6.3.0` (earlephilhower) added to arduino-cli. FQBN for Pico: `rp2040:rp2040:rpipico`.
9. **Git housekeeping**: old diagram deletions staged, everything new untracked; commit baseline (user's call).

## Next moves
- Fix GY-511 I2C init so Daphne reads real accel/mag values.
- Move throwaway sketches into the repo folder structure with proper µT-Kernel skeletons.
- Daphne UART RX + I2C combined sniff scenario (S1-S4) once GY-511 reads valid.
- Week 10 check-in: resolve A8 (RMT RX channel limit) and SD bitbang constraint.
- Flash iris firmware when iris board is on bench.

## Suggested skills
- writing-for-agents (any doc writing/editing here)
- karpathy-guidelines (when code starts)
- md-to-pdf (rebuild whattheflip.pdf)
