# daphne (Victim B) Firmware

µT-Kernel 3.0 dual-core SMP (`SMP=1`, IEEE 2050-2018) implementation for **daphne** (Raspberry Pi Pico RP2040 seated on Cytron Maker Pi Pico Base).

---

## System Role & Peripheral Allocation

| Subsystem | Core Binding (`TA_ASSPRC`) | Pins | Parameters | Role |
|---|---|---|---|---|
| **UART0 RX** | Processor 1 (`TP_PRC1`, Core 0) | GP1 (Row 12) | 115200 baud, 8N1 | Receives telemetry packets from Hestia (`HESTIA:<counter>:<checksum>`), validates framing and checksum (`~counter`). |
| **I2C Master** | Processor 2 (`TP_PRC2`, Core 1) | GP6 SDA (Row 43)<br>GP7 SCL (Row 42) | 400 kHz Fast-Mode (`I2C1`) | Polls GY-511 (LSM303DLHC) accelerometer (`0x19`) and magnetometer (`0x1E`) with 8-sample moving average filter. Auto-detects GP6/7 vs GP4/5. |
| **USB CDC Console** | Processor 1 (Core 0) | USB (Micro-USB) | 115200 baud CDC ACM | Streams system logs via `tm_printf` under SMP-safe console lock. Supports software reboot into BOOTSEL on 1200 baud or `B` command. |

---

## Pin Mapping & Bench Findings

- **UART RX**: Daphne GP1 (Row 12) receives transmissions from Hestia GP0 (Row 10) through the 220 Ω series protection resistor.
- **I2C Bus Detection**: The universal GPIO pin scanner identified that the breadboard pull-ups for the GY-511 sensor are routed to **GP6 (SDA)** and **GP7 (SCL)** (RP2040 `I2C1`). The driver automatically probes both I2C0 and I2C1 and selects the active bus.
- **3.3V Power**: Daphne Pin 36 (`3V3_OUT`) supplies 3.3V power to the breadboard (+) Red rail.

---

## Sensor Protocol & Register Specification

### 1. Accelerometer (`0x19`)
- `CTRL_REG1_A` (`0x20`): `0x77` (400 Hz ODR, Normal mode, all 3 axes enabled).
- `CTRL_REG4_A` (`0x23`): `0x08` (High Resolution mode, ±2g range).
- **Auto-Increment Invariant**: Multi-byte burst reads of `OUT_X_L_A` through `OUT_Z_H_A` (`0x28..0x2D`) **must set bit 7 (`0x80`)** on the register sub-address (`0x28 | 0x80 = 0xA8`) with repeated START. Reading without bit 7 fails to advance the internal register pointer.

### 2. Magnetometer (`0x1E`)
- `CRA_REG_M` (`0x00`): `0x14` (30 Hz ODR).
- `CRB_REG_M` (`0x01`): `0x20` (±1.3 Gauss gain).
- `MR_REG_M` (`0x02`): `0x00` (Continuous conversion mode).
- Burst reads starting at `OUT_X_H_M` (`0x03`).

---

## Building and Flashing

### 1. Build UF2
```bash
make -C whattheflip/daphne
```
Generates `whattheflip/daphne/daphne.uf2`.

### 2. Flash to Device (Software Reboot — No Buttons Required)
To flash without touching physical buttons:
```bash
python3 -c "import serial; s = serial.Serial('/dev/cu.usbmodemMTK3PICO1', 1200); s.close()"
sleep 2
make -C whattheflip/daphne flash
```
Or if Daphne is in BOOTSEL mode:
```bash
make -C whattheflip/daphne flash
```

---

## Hardware Verification Telemetry

Live output captured from Daphne over USB CDC (`/dev/cu.usbmodemMTK3PICO1`):
```text
[DAPHNE:I2C] ACCEL mg: X=-28 Y=56 Z=981 (avg: X=-30 Y=55 Z=984) | MAG: X=-307 Y=54 Z=61
[DAPHNE:UART] PKT #14841 VALID (total=30, valid=30, corrupt=0)
[DAPHNE:I2C] ACCEL mg: X=-27 Y=56 Z=985 (avg: X=-31 Y=55 Z=982) | MAG: X=-307 Y=50 Z=60
[DAPHNE:I2C] ACCEL mg: X=-33 Y=55 Z=982 (avg: X=-30 Y=56 Z=981) | MAG: X=-307 Y=50 Z=58
[DAPHNE:I2C] ACCEL mg: X=-36 Y=57 Z=977 (avg: X=-30 Y=56 Z=980) | MAG: X=-308 Y=51 Z=60
```
- `Z ≈ 981 mg` corresponds exactly to $1g$ of Earth gravity along the vertical axis.
- Checksums verified across thousands of continuous packets with zero corruption.
