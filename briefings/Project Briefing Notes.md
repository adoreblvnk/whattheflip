# INF2004 Autonomous Robotic Car Challenge: Briefing Intel

---

## 1. Executive Snapshot & Hard Rules

* **The Mission:** A fully autonomous, line-tracking robotic car that navigates an unannounced track, decodes navigation barcodes, measures single-wheel speed humps, avoids static obstacles, and streams real-time MQTT telemetry over Wi-Fi.
* **Target Runtime:** Must complete the full track in **$< 5$ minutes** without manual intervention.
* **Demo Scoring Rule:** You are given **up to 3 trials**, but **ONLY the most recent run’s score is recorded** (not the highest). Stop once you achieve a stable run.
* **Firmware Cap:** 
  * Using **FreeRTOS** $\rightarrow$ Grade capped at **A-**.
  * Using **$\mu\text{T-Kernel}$ (MicroT-Kernel)** $\rightarrow$ Eligible for **A / A+**.
* **Code Ownership:** Zero tolerance for unexplained AI code. Any member unable to explain a line of code within their assigned subsystem will receive immediate grade penalties. Individual grades are moderated via peer evaluation and **GitHub commit history**.

---

## 2. Subsystem & "Buddy" Breakdown

```
                       +------------------------------------+
                       |         Vehicle Controller         |
                       |    (State Machine Coordination)    |
                       +-----------------+------------------+
                                         |
         +---------------+---------------+---------------+---------------+
         |               |               |               |               |
         v               v               v               v               v
    [Buddy 1]       [Buddy 2]       [Buddy 3]       [Buddy 4]       [Buddy 5]
    Wi-Fi/MQTT        Motion        Line & Barcode  IMU & Terrain   Ultrasonic
    Telemetry        Control          (3x IR)         (GY-511)       + Servo
```

### Buddy 1: Wi-Fi, Command & Telemetry
* **Hardware:** RP2040 on-board Wi-Fi.
* **Protocol:** Two-way MQTT (Client on Pico $\leftrightarrow$ Evaluator's Broker on PC).
* **Requirements:**
  * Stream continuous telemetry (speed, hump count, hump height, scanned barcodes, decoded actions, vehicle state).
  * Handle dynamic command requests (e.g., send snapshot of telemetry data upon request).
  * **Critical:** Receive and update runtime barcode-to-action mappings dynamically over MQTT (e.g., mapping character `D` to `LEFT`). Recompiling firmware on demo day is strictly banned.

### Buddy 2: Motion Control & Odometry
* **Hardware:** Dual DC motors + optical wheel encoders.
* **Requirements:**
  * Implement **PID closed-loop speed control** to synchronize left and right wheel speeds (compensating for physical motor asymmetries).
  * Execute precise turn angles ($45^\circ$, $90^\circ$, and $180^\circ$ U-turns) and accurate dead-reckoning straight lines.
  * Maintain line-following stability when one side climbs an asymmetric hump.

### Buddy 3: Line Tracking & Barcode Decoding
* **Hardware:** $3\times$ IR Sensors ($2\times$ for line detection, $1\times$ for outer barcode scanning).
* **Requirements:**
  * **Line Following:** 2 IR sensors bracket the black line edges for proportional steering corrections.
  * **Barcode Reading:** Decode directional 1D barcodes located on the side of the track.
  * **Bidirectional Decoding:** Algorithm must correctly decode barcode characters (A–Z) regardless of whether the car scans them forward or backward.
  * **Execution:** Action (Forward, Left, Right, U-Turn) must be held and **executed only when the car reaches the cross-junction**.

### Buddy 4: IMU & Terrain Profiling
* **Hardware:** GY-511 Module (3-Axis Accelerometer + 3-Axis Magnetometer).
* **Requirements:**
  * **Hump Detection & Height:** Detect single-wheel hump traversal via pitch/roll tilt and vertical acceleration impulses ($h \approx \frac{v^2}{2g}$ or calibrated tilt-to-height lookup).
  * **Heading & Orientation:** Digital compass / yaw rate to verify turn execution.
  * **Grade Booster:** Perform **Sensor Fusion** (combining IMU data with Buddy 2's wheel encoders) to improve heading accuracy. Measuring while moving earns higher marks than stopping over the hump.

### Buddy 5: Adaptive Ultrasonic Scanning & Obstacle Avoidance
* **Hardware:** HC-SR04 Ultrasonic sensor mounted on a positional Servo motor.
* **Requirements:**
  * **Scan & Profile:** Detect static monolith obstacles on straight track sections; sweep servo left/right to measure obstacle width and identify the clearance path with the largest gap.
  * **Avoidance & Re-acquisition:** Steer off-line around the obstacle (accounting for the car’s chassis width), search for the line behind the obstacle, and smoothly re-align to resume tracking.

---

## 3. Critical Failure Modes & Engineering Tips

| Subsystem | Common Mistake / Trap | Recommended Engineering Fix |
| :--- | :--- | :--- |
| **Motion (PID)** | Tuning PID parameters before the mechanical chassis is finalized. | **Lock all hardware down first.** Any change in wheel alignment, weight, or battery voltage invalidates PID constants ($K_p, K_i, K_d$). |
| **Barcode** | Hardcoding `A = Straight, B = Left, C = Right, D = U-Turn`. | Store full A–Z barcode patterns in a lookup table. Update active navigation mappings dynamically via an incoming MQTT payload. |
| **Line Tracking** | Sensor saturation due to ambient room lighting. | Calibrate IR threshold values dynamically at startup over both black track tape and white floor surfaces. |
| **Ultrasonic** | Clipping the obstacle chassis corner during bypass. | Add a safety margin to the measured obstacle width $W$ that accounts for half the vehicle's axle width plus turning radius. |
| **IMU / Terrain** | High-frequency motor vibrations corrupting accelerometer readings. | Implement a moving average or low-pass digital filter on vertical acceleration ($Z$-axis) and pitch angles. |
| **System-wide** | Freezing execution inside blocking delay loops (`sleep_ms`). | Use non-blocking RTOS task delays (`dly_tsk` in $\mu\text{T-Kernel}$) and periodic timers so concurrent sensor polling never halts. |

---

## 4. Milestone Roadmap

```
Week 06 ──> [HURDLE] Requirements & Architecture Document (-5% / day if late)
Week 08 ──> [TRIBUNAL 1] Live Zoom Breakout + Unannounced Requirement Change (15%)
Week 10 ──> [HURDLE] Subsystems In-Lab Demonstration & Health Check
Week 13 ──> [TRIBUNAL 2] Theoretical Architecture Adaptation Review (15%)
Week 13 ──> [FINAL DEMO] 3-Trial Track Run + Public GitHub Repo & Barr C Review (15%)
```