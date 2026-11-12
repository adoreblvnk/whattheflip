#!/usr/bin/env bash
# ==============================================================================
# monitor_gy511.sh // Live GY-511 (LSM303DLHC) Sensor Telemetry Monitor
# Course: INF2004 Embedded Systems Programming (Group SE06 - whattheflip)
#
# Detects and tracks live GY-511 motion (3-axis Accel + Mag) from Daphne (RP2040).
# - Primary source : ws://localhost:8889 (via running localhost_bridge.py)
# - Fallback source: Direct serial port /dev/cu.usbmodemMTK3PICO1 (115200 baud)
#
# Usage:
#   ./whattheflip/monitor_gy511.sh            # Continuous real-time stream
#   ./whattheflip/monitor_gy511.sh --count 10 # Capture 10 samples then exit
#   ./whattheflip/monitor_gy511.sh --raw      # Raw JSON output
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COUNT=0
RAW_MODE=false
while [ $# -gt 0 ]; do
  case "$1" in
    --count)
      shift
      COUNT="${1:-10}"
      shift
      ;;
    --count=*)
      COUNT="${1#*=}"
      shift
      ;;
    --raw)
      RAW_MODE=true
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--count N] [--raw]"
      echo "  --count N   Stop after N readings (default: infinite)"
      echo "  --raw       Output raw JSON telemetry lines"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

exec uv run --quiet --with websockets --with pyserial python3 - "$COUNT" "$RAW_MODE" << 'EOF'
import asyncio
import json
import math
import os
import re
import sys
import time

count_limit = int(sys.argv[1]) if len(sys.argv) > 1 else 0
raw_mode = (sys.argv[2].lower() == "true") if len(sys.argv) > 2 else False

PORT_DAPHNE = "/dev/cu.usbmodemMTK3PICO1"
WS_URL = "ws://localhost:8889"

def format_bar(val_mg, max_mg=2000, width=12):
    clamped = max(-max_mg, min(max_mg, val_mg))
    half = width // 2
    pos = int((clamped / max_mg) * half)
    bar = [" "] * width
    bar[half] = "|"
    if pos > 0:
        for i in range(half + 1, min(width, half + 1 + pos)):
            bar[i] = "#"
    elif pos < 0:
        for i in range(max(0, half + pos), half):
            bar[i] = "#"
    return "".join(bar)

def render_sample(idx, t_elapsed, ax, ay, az, mx=None, my=None, mz=None):
    if raw_mode:
        data = {
            "sample": idx,
            "elapsed_s": round(t_elapsed, 2),
            "accel_mg": {"x": ax, "y": ay, "z": az},
            "mag": {"x": mx, "y": my, "z": mz} if mx is not None else None
        }
        print(json.dumps(data))
        return

    net_g = math.sqrt(ax*ax + ay*ay + az*az) / 1000.0

    if abs(net_g - 1.0) < 0.12:
        motion = "\033[32mSTATIC (1.0g)\033[0m"
    elif net_g > 1.35 or net_g < 0.65:
        motion = "\033[1;31mDYNAMIC JERK\033[0m"
    else:
        motion = "\033[33mMOTION / TILT\033[0m"

    bx = format_bar(ax)
    by = format_bar(ay)
    bz = format_bar(az)

    mag_str = f"M:({mx:>5},{my:>5},{mz:>5})" if mx is not None else "M:(  n/a )"

    print(
        f"\r\033[2K[{t_elapsed:>5.1f}s] #{idx:<4} "
        f"X: {ax:>5} mg [{bx}]  "
        f"Y: {ay:>5} mg [{by}]  "
        f"Z: {az:>5} mg [{bz}]  "
        f"|G|: {net_g:>4.2f}g  {motion:<18}  {mag_str}"
    )
    sys.stdout.flush()

async def read_from_ws():
    import websockets
    async with websockets.connect(WS_URL) as ws:
        t0 = time.time()
        idx = 0
        if not raw_mode:
            print("\033[1;36m=== WHATTHEFLIP // GY-511 (LSM303DLHC) LIVE MOTION TRACKER ===\033[0m")
            print("Source: Localhost Bridge WebSocket (ws://localhost:8889)")
            print("Press Ctrl+C to stop.\n")
        while True:
            msg = await ws.recv()
            data = json.loads(msg)
            if data.get("type") == "telemetry":
                idx += 1
                t_elapsed = time.time() - t0
                acc = data.get("accel", {})
                mag = data.get("mag", {})
                render_sample(
                    idx,
                    t_elapsed,
                    acc.get("x", 0),
                    acc.get("y", 0),
                    acc.get("z", 0),
                    mag.get("x"),
                    mag.get("y"),
                    mag.get("z")
                )
                if count_limit > 0 and idx >= count_limit:
                    break

def read_from_serial():
    import serial
    if not os.path.exists(PORT_DAPHNE):
        print(f"Error: Neither bridge WebSocket nor serial port {PORT_DAPHNE} found.", file=sys.stderr)
        sys.exit(1)

    s = serial.Serial(PORT_DAPHNE, 115200, timeout=1.0)
    t0 = time.time()
    idx = 0
    if not raw_mode:
        print("\033[1;36m=== WHATTHEFLIP // GY-511 (LSM303DLHC) LIVE MOTION TRACKER ===\033[0m")
        print(f"Source: Direct Serial on {PORT_DAPHNE} (115200 baud)")
        print("Press Ctrl+C to stop.\n")
    try:
        while True:
            line = s.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            m = re.search(r"ACCEL mg:\s*X=(-?\d+)\s*Y=(-?\d+)\s*Z=(-?\d+)", line)
            if m:
                idx += 1
                t_elapsed = time.time() - t0
                ax = int(m.group(1))
                ay = int(m.group(2))
                az = int(m.group(3))
                mx, my, mz = None, None, None
                mm = re.search(r"MAG:\s*X=(-?\d+)\s*Y=(-?\d+)\s*Z=(-?\d+)", line)
                if mm:
                    mx = int(mm.group(1))
                    my = int(mm.group(2))
                    mz = int(mm.group(3))
                render_sample(idx, t_elapsed, ax, ay, az, mx, my, mz)
                if count_limit > 0 and idx >= count_limit:
                    break
    finally:
        s.close()

try:
    asyncio.run(read_from_ws())
except (ConnectionRefusedError, OSError):
    read_from_serial()
except KeyboardInterrupt:
    if not raw_mode:
        print("\n\033[33m[Stopped GY-511 monitor]\033[0m")
EOF
