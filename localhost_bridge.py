#!/usr/bin/env python3
# /// script
# dependencies = [
#   "pyserial",
#   "websockets",
# ]
# ///
"""
whattheflip // Localhost Bridge & Hardware Telemetry Relay
Course: INF2004 Embedded Systems Programming (Group SE06)

Runs locally on macOS:
1. Connects to live microcontrollers:
   - whattheflip: /dev/cu.usbmodem101 (ESP32-C6 µT-Kernel 3.0)
   - daphne:      /dev/cu.usbmodem5 (RP2040 Core 0/1 UART RX + I2C Master)
   - hestia:      /dev/cu.usbmodemMTK3PICO1 (RP2040 Core 0 UART TX)
2. Serves the static dashboard bundle at http://localhost:8888
3. Streams live 2 Hz telemetry and <200ms alerts over ws://localhost:8888/ws
4. Relays attack commands (POST /cmd?c=U, A, S, W, E, R) directly to whattheflip silicon

Usage:
  uv run whattheflip/localhost_bridge.py
"""

import asyncio
import http.server
import json
import os
import re
import socketserver
import struct
import sys
import threading
import time
from typing import Set

try:
    import serial
except ImportError:
    serial = None

try:
    import websockets
except ImportError:
    websockets = None

HTTP_PORT = 8888
WS_PORT = 8889
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WWW_DIR = os.path.join(BASE_DIR, "frontend", "dist", "WWW")

PORT_WTF = "/dev/cu.usbmodem101"
PORT_DAPHNE = "/dev/cu.usbmodemMTK3PICO1"
PORT_HESTIA = "/dev/cu.usbmodem5"
PORT_IRIS = "/dev/cu.usbmodem11301"

# Shared Global Hardware Telemetry State
state = {
    "uart_pkts": 0,
    "i2c_pkts": 0,
    "wifi_pkts": 0,
    "ieee154_pkts": 0,
    "fuzz_cycles": 0,
    "corrupted_frames": 0,
    "wifi_ch": 6,
    "wifi_mode": 6,
    "zigbee_ch": 15,
    "tool_mode": "AUTO",
    "disc_locked": False,
    "safety": "ARMED",
    "uart_locked": True,
    "i2c_locked": True,
    "pcap_bytes": 24,
    "accel": None,
    "mag": None,
    "uptime_s": 0,
    "ber": "0.000%",
    "iris_rssi": None,
    "iris_verified": False
}
ser_wtf = None
ser_daphne = None
ser_hestia = None
ser_iris = None
ws_clients: Set = set()
last_uart_ctr = 100
pcap_records = bytearray()

# Initialize PCAP 2.4 Header (24 bytes)
pcap_records.extend(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 147))

def append_pcap_frame(proto_id, probe_id, payload_bytes):
    global pcap_records
    t = time.time()
    t_sec = int(t)
    t_usec = int((t - t_sec) * 1000000)
    user0_hdr = bytes([proto_id, probe_id])
    full_payload = user0_hdr + payload_bytes
    rec_hdr = struct.pack("<IIII", t_sec, t_usec, len(full_payload), len(full_payload))
    pcap_records.extend(rec_hdr + full_payload)
    state["pcap_bytes"] = len(pcap_records)

# --------------------------------------------------------------------------
# Background Serial Reader for whattheflip (ESP32-C6)
# --------------------------------------------------------------------------
def wtf_serial_reader():
    global ser_wtf
    t_boot = time.time()
    while True:
        try:
            if ser_wtf is None:
                ser_wtf = serial.Serial(PORT_WTF, 115200, timeout=0.2)
                print(f"[BRIDGE] Connected to whattheflip on {PORT_WTF}")
            
            line = ser_wtf.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            state["uptime_s"] = int(time.time() - t_boot)

            # Match 2 Hz telemetry
            # [DASHBOARD:2Hz] Telemetry: UART_pkts=512 | I2C_pkts=3246 | WiFi_pkts=119 | 802154_pkts=20 | Alerts=0 | Safety=ARMED
            m = re.search(r"UART_pkts=(\d+)\s*\|\s*I2C_pkts=(\d+)\s*\|\s*WiFi_pkts=(\d+)\s*\|\s*802154_pkts=(\d+)\s*\|\s*Alerts=(\d+)\s*\|\s*Safety=(\w+)", line)
            if m:
                state["uart_pkts"] = int(m.group(1))
                state["i2c_pkts"] = int(m.group(2))
                state["wifi_pkts"] = int(m.group(3))
                # Take max: ESP32 may report 0 if 802.15.4 RX not yet enabled,
                # while Iris serial reader counts actual TX frames
                esp32_154 = int(m.group(4))
                state["ieee154_pkts"] = max(esp32_154, state["ieee154_pkts"])
                state["corrupted_frames"] = int(m.group(5))
                state["safety"] = m.group(6)

            # Match Iris RSSI/verified from extended telemetry line
            # [DASHBOARD:2Hz] Iris: RSSI=-42 dBm | Verified=true
            m_iris = re.search(r"Iris:\s*RSSI=(-?\d+)\s*dBm\s*\|\s*Verified=(\w+)", line)
            if m_iris:
                state["iris_rssi"] = int(m_iris.group(1))
                state["iris_verified"] = m_iris.group(2) == "true"

            # Match SNIFFER Wi-Fi capture lines for live WiFi packet count & PCAP
            m_wifi = re.search(r"\[SNIFFER:WIFI\] CAPTURE: 802\.11 Frame on Ch (\d+) \(RSSI=(-?\d+) dBm, len=(\d+)(.*?)\)", line)
            if m_wifi:
                w_ch = int(m_wifi.group(1))
                w_rssi = int(m_wifi.group(2))
                w_len = int(m_wifi.group(3))
                is_iris = "IRIS" in m_wifi.group(4)
                state["wifi_pkts"] = state.get("wifi_pkts", 0) + 1
                mac_bytes = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x03]) if is_iris else bytes([0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF])
                append_pcap_frame(4, w_ch, mac_bytes + f" CH{w_ch} RSSI={w_rssi}dBm LEN={w_len}".encode("utf-8"))

            # Match SNIFFER Zigbee capture lines
            m_154 = re.search(r"\[SNIFFER:802154\] CAPTURE: 802\.15\.4 Frame #(\d+) on Ch (\d+)", line)
            if m_154:
                fnum = int(m_154.group(1))
                zch = int(m_154.group(2))
                state["ieee154_pkts"] = max(fnum, state["ieee154_pkts"])
                append_pcap_frame(5, zch, bytes([0x41, 0x88, fnum & 0xFF]) + f"ZIGBEE:PAN0x1234:SEQ{fnum}:CH{zch}".encode("utf-8"))
            # Match Alert — forward ESP32 hardware-confirmed alerts directly to dashboard
            if "[TELEMETRY:ALERT]" in line or "FAST FAULT ALERT" in line or "[FUZZER]" in line or "[DASHBOARD:ALERT]" in line:
                broadcast_ws({"type": "alert", "msg": "[ESP32 HW] " + line})

        except Exception as e:
            ser_wtf = None
            time.sleep(1.0)

# --------------------------------------------------------------------------
# Background Serial Reader for daphne (RP2040)
# --------------------------------------------------------------------------
def daphne_serial_reader():
    global ser_daphne, last_uart_ctr
    while True:
        try:
            if ser_daphne is None:
                ser_daphne = serial.Serial(PORT_DAPHNE, 115200, timeout=0.2)
                print(f"[BRIDGE] Connected to daphne on {PORT_DAPHNE}")

            line = ser_daphne.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            # Match [DAPHNE:I2C] ACCEL mg: X=-218 Y=-155 Z=1009
            m_accel = re.search(r"ACCEL mg:\s*X=(-?\d+)\s*Y=(-?\d+)\s*Z=(-?\d+)", line)
            if m_accel:
                ax = int(m_accel.group(1))
                ay = int(m_accel.group(2))
                az = int(m_accel.group(3))
                state["accel"] = {"x": ax, "y": ay, "z": az}
                # Also log synthesized I2C packet to PCAP
                append_pcap_frame(2, 2, bytes([0x33, (ax & 0xFF), ((ax >> 8) & 0xFF), (ay & 0xFF), ((ay >> 8) & 0xFF), (az & 0xFF), ((az >> 8) & 0xFF)]))

            # Match [DAPHNE:I2C] ... MAG: X=-307 Y=54 Z=61
            m_mag = re.search(r"MAG:\s*X=(-?\d+)\s*Y=(-?\d+)\s*Z=(-?\d+)", line)
            if m_mag:
                state["mag"] = {
                    "x": int(m_mag.group(1)),
                    "y": int(m_mag.group(2)),
                    "z": int(m_mag.group(3))
                }

            # Match [DAPHNE:BER] ... BER=31.06%
            m_ber = re.search(r"BER=([0-9\.]+)%", line)
            if m_ber:
                state["ber"] = m_ber.group(1) + "%"

            # Match I2C Bus Fault / 9-Clock Recovery from Daphne
            if "[DAPHNE:I2C] Bus fault detected" in line or "[DAPHNE:I2C] FAULT:" in line:
                state["corrupted_frames"] = state.get("corrupted_frames", 0) + 1
                append_pcap_frame(3, 2, b"I2C_FAULT:NACK_0x19_9CLK_RECOVERY")

            # Match UART Corrupt / Valid frames with dynamic counter and real corrupted bytes
            if "[DAPHNE:UART] CORRUPT" in line:
                m_raw = re.search(r"raw=\[(.*?)\]", line)
                if m_raw and m_raw.group(1):
                    raw_txt = m_raw.group(1)
                else:
                    last_uart_ctr += 1
                    corrupt_csum = ((~last_uart_ctr) ^ 0x0800) & 0xFFFF
                    raw_txt = f"HESTIA:{last_uart_ctr}:{corrupt_csum:04X}"
                append_pcap_frame(1, 1, (raw_txt + "\n").encode("utf-8"))
            elif "[DAPHNE:UART] PKT #" in line:
                m_pkt = re.search(r"PKT #(\d+)", line)
                pkt_num = int(m_pkt.group(1)) if m_pkt else (last_uart_ctr + 1)
                last_uart_ctr = pkt_num
                exp_csum = (~pkt_num) & 0xFFFF
                append_pcap_frame(1, 1, f"HESTIA:{pkt_num}:{exp_csum:04X}\n".encode("utf-8"))
        except Exception as e:
            ser_daphne = None
            time.sleep(1.0)


# --------------------------------------------------------------------------
# Background Serial Reader for iris (ESP32-C6 wireless victim)
# --------------------------------------------------------------------------
def iris_serial_reader():
    global ser_iris
    while True:
        try:
            if ser_iris is None:
                ser_iris = serial.Serial(PORT_IRIS, 115200, timeout=0.2)
                print(f"[BRIDGE] Connected to iris on {PORT_IRIS}")

            line = ser_iris.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            # Match [IRIS:802154] TX Frame #N on Ch 15 (len=N)
            m_tx = re.search(r"\[IRIS:802154\] TX Frame #(\d+) on Ch (\d+) \(len=(\d+)\)", line)
            if m_tx:
                frame_num = int(m_tx.group(1))
                ch = int(m_tx.group(2))
                flen = int(m_tx.group(3))
                state["ieee154_pkts"] = frame_num
                state["zigbee_ch"] = ch
                payload = bytes([0x41, 0x88, frame_num & 0xFF]) + f"IRIS:PKT:{frame_num}:C{ch}:2H".encode("utf-8")
                append_pcap_frame(5, ch, payload)
            # Match [IRIS:BEACON] SoftAP started: SSID='iris-victim' Ch=6 BSSID=...
            m_beacon = re.search(r"\[IRIS:BEACON\] SoftAP started: SSID='(\S+)' Ch=(\d+)", line)
            if m_beacon:
                state["wifi_ch"] = int(m_beacon.group(2))

            # Match [IRIS:OTA] command acknowledgments
            if "[IRIS:OTA]" in line:
                broadcast_ws({"type": "alert", "msg": f"IRIS HW CONFIRMED: {line}"})

            # Match [IRIS:JAM] jam detection
            if "[IRIS:JAM]" in line:
                broadcast_ws({"type": "alert", "msg": f"IRIS HW CONFIRMED: {line}"})

        except Exception as e:
            ser_iris = None
            time.sleep(1.0)
# --------------------------------------------------------------------------
# WebSocket Server & Broadcaster
# --------------------------------------------------------------------------
ws_event_loop = None

def broadcast_ws(obj):
    if not ws_clients or ws_event_loop is None:
        return
    msg = json.dumps(obj)
    for ws in list(ws_clients):
        try:
            asyncio.run_coroutine_threadsafe(ws.send(msg), ws_event_loop)
        except Exception:
            pass

async def ws_handler(websocket):
    ws_clients.add(websocket)
    try:
        async for message in websocket:
            pass
    finally:
        ws_clients.discard(websocket)

async def telemetry_broadcaster():
    while True:
        await asyncio.sleep(0.5)  # 2 Hz broadcast
        if ws_clients:
            msg_obj = {
                "type": "telemetry",
                "uart_pkts": state["uart_pkts"],
                "i2c_pkts": state["i2c_pkts"],
                "wifi_pkts": state["wifi_pkts"],
                "ieee154_pkts": state["ieee154_pkts"],
                "fuzz_cycles": state["fuzz_cycles"],
                "corrupted_frames": state["corrupted_frames"],
                "wifi_ch": state["wifi_ch"],
                "wifi_mode": state["wifi_mode"],
                "zigbee_ch": state["zigbee_ch"],
                "tool_mode": state["tool_mode"],
                "disc_locked": state["disc_locked"],
                "safety": state["safety"],
                "uart_locked": state["uart_locked"],
                "i2c_locked": state["i2c_locked"],
                "pcap_bytes": state["pcap_bytes"],
                "accel": state["accel"],
                "mag": state["mag"],
                "uptime_s": state["uptime_s"],
                "ber": state["ber"],
                "iris_rssi": state["iris_rssi"],
                "iris_verified": state["iris_verified"]
            }
            broadcast_ws(msg_obj)

# --------------------------------------------------------------------------
# HTTP Server (Serves /WWW/ assets + /api/pcap + /cmd)
# --------------------------------------------------------------------------
class BridgeHttpHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WWW_DIR, **kwargs)

    def do_GET(self):
        url = self.path.split("?")[0]
        if url == "/" or url == "/INDEX.HTM" or url == "/index.html":
            self.path = "/INDEX.HTM"
            return super().do_GET()
        elif url == "/app.js" or url == "/APP.JS":
            self.send_response(200)
            self.send_header("Content-Type", "application/javascript")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            with open(os.path.join(WWW_DIR, "APP.JS"), "rb") as f:
                self.wfile.write(f.read())
            return
        elif url == "/style.css" or url == "/STYLE.CSS":
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            with open(os.path.join(WWW_DIR, "STYLE.CSS"), "rb") as f:
                self.wfile.write(f.read())
            return
        elif url == "/api/pcap":
            req_offset = 0
            req_limit = 4096
            if "?" in self.path:
                q = self.path.split("?")[1]
                parts = dict(p.split("=") for p in q.split("&") if "=" in p)
                if "offset" in parts:
                    try: req_offset = int(parts["offset"])
                    except ValueError: pass
                if "limit" in parts:
                    try: req_limit = int(parts["limit"])
                    except ValueError: pass

            total_sz = len(pcap_records)
            chunk = pcap_records[req_offset:req_offset+req_limit]

            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Expose-Headers", "X-Pcap-Total-Bytes, X-Pcap-Offset")
            self.send_header("X-Pcap-Total-Bytes", str(total_sz))
            self.send_header("X-Pcap-Offset", str(req_offset))
            self.end_headers()
            self.wfile.write(chunk)
            return
        elif url == "/capture.pcap":
            total_sz = len(pcap_records)
            range_hdr = self.headers.get("Range")
            if range_hdr and range_hdr.startswith("bytes="):
                try:
                    parts = range_hdr[6:].split("-")
                    r_start = int(parts[0]) if parts[0] else 0
                    r_end = int(parts[1]) if len(parts) > 1 and parts[1] else (total_sz - 1)
                    if r_end >= total_sz:
                        r_end = total_sz - 1
                    chunk = pcap_records[r_start:r_end + 1]
                    self.send_response(206)
                    self.send_header("Content-Type", "application/vnd.tcpdump.pcap")
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Content-Range", f"bytes {r_start}-{r_end}/{total_sz}")
                    self.send_header("Content-Length", str(len(chunk)))
                    self.end_headers()
                    self.wfile.write(chunk)
                    return
                except Exception:
                    pass

            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.tcpdump.pcap")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Disposition", 'attachment; filename="capture.pcap"')
            self.send_header("Content-Length", str(total_sz))
            self.end_headers()
            self.wfile.write(pcap_records)
            return
        elif url == "/telemetry":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(state).encode("utf-8"))
            return
        return super().do_GET()

    def do_POST(self):
        url = self.path.split("?")[0]
        if url == "/cmd":
            q = self.path.split("?")[1] if "?" in self.path else ""
            parts = dict(p.split("=") for p in q.split("&") if "=" in p)
            cmd_c = parts.get("c", "")
            cmd_ch = parts.get("ch", "")
            cmd_iris = parts.get("iris_cmd", "")
            cmd_pin = parts.get("pin", "")
            cmd_dec = parts.get("dec", "")
            cmd_mode = parts.get("mode", "")
            cmd_lock = parts.get("lock_disc", "")
            cmd_zch = parts.get("zch", "")


            # Relay to whattheflip silicon if serial is connected
            hw_connected = ser_wtf is not None and ser_wtf.is_open
            if hw_connected:
                if cmd_c:
                    ser_wtf.write(cmd_c.encode("utf-8"))
                if cmd_ch:
                    ser_wtf.write(f"CH{cmd_ch}\n".encode("utf-8"))
                if cmd_zch:
                    ser_wtf.write(f"ZCH{cmd_zch}\n".encode("utf-8"))
                if cmd_mode:
                    ser_wtf.write(f"MODE:{cmd_mode}\n".encode("utf-8"))
                if cmd_lock:
                    ser_wtf.write(f"DISCLOCK:{cmd_lock}\n".encode("utf-8"))
                if cmd_iris:
                    ser_wtf.write(f"IRIS:{cmd_iris}\n".encode("utf-8"))

            hw_tag = "[HW RELAYED] " if hw_connected else "[LOCAL ONLY — ESP32 DISCONNECTED] "

            if cmd_iris == "20hz":
                broadcast_ws({"type": "alert", "msg": hw_tag + "IRIS OTA: Commanded 20 Hz Stress Burst Rate (50 ms)"})
                append_pcap_frame(5, 15, b"\x41\x88\x01IRIS:PKT:CMD_ACK:20HZ:BURST")
            elif cmd_iris == "2hz":
                broadcast_ws({"type": "alert", "msg": hw_tag + "IRIS OTA: Restored 2 Hz Nominal Beacon Rate (500 ms)"})
                append_pcap_frame(5, 15, b"\x41\x88\x02IRIS:PKT:CMD_ACK:2HZ:NOMINAL")
            elif cmd_iris == "ch11":
                broadcast_ws({"type": "alert", "msg": hw_tag + "IRIS OTA: Commanded SoftAP Hop to Wi-Fi Channel 11"})
                append_pcap_frame(4, 11, b"IRIS:BEACON:CH11:02:00:00:00:00:03")
            elif cmd_iris == "ch6":
                broadcast_ws({"type": "alert", "msg": hw_tag + "IRIS OTA: Commanded SoftAP Hop back to Wi-Fi Channel 6"})
                append_pcap_frame(4, 6, b"IRIS:BEACON:CH6:02:00:00:00:00:03")

            if cmd_mode:
                state["tool_mode"] = cmd_mode.upper()
                if cmd_mode.upper() != "AUTO":
                    state["disc_locked"] = True
                broadcast_ws({"type": "alert", "msg": hw_tag + f"CAPTURE ENGINE: Switched to {state['tool_mode']} Tool Mode"})
            if cmd_lock:
                state["disc_locked"] = (cmd_lock in ["1", "true", "True"])
                lock_str = "LOCKED (Manual Override Active)" if state["disc_locked"] else "UNLOCKED (Auto-Discovery Active)"
                broadcast_ws({"type": "alert", "msg": hw_tag + f"DISCOVERY GATE: {lock_str}"})
            if cmd_zch:
                try:
                    zch_int = int(cmd_zch)
                    state["zigbee_ch"] = zch_int
                    freq_mhz = 2405 + 5 * (zch_int - 11)
                    broadcast_ws({"type": "alert", "msg": hw_tag + f"802.15.4 RADIO: Tuned to Channel {zch_int} ({freq_mhz} MHz)"})
                except ValueError:
                    pass

            # Local state reflection
            if cmd_c in ["U", "u"]:
                global last_uart_ctr
                last_uart_ctr += 1
                state["corrupted_frames"] = state.get("corrupted_frames", 0) + 1
                corrupt_csum = ((~last_uart_ctr) ^ 0x0800) & 0xFFFF
                corrupt_frame = f"HESTIA:{last_uart_ctr}:{corrupt_csum:04X}\n".encode("utf-8")
                append_pcap_frame(3, 0, b"10us UART1 TX Bit-Flip on Row 13")
                append_pcap_frame(1, 1, corrupt_frame)
                broadcast_ws({"type": "alert", "msg": hw_tag + f"UART GLITCH: Row 13 bit D3 flipped -> HESTIA:{last_uart_ctr}:{corrupt_csum:04X}"})
            elif cmd_c in ["A", "a"]:
                state["corrupted_frames"] = state.get("corrupted_frames", 0) + 1
                append_pcap_frame(3, 2, b"50us I2C0 ACK-Kill on Row 58 (0x32 -> NACK -> 9-Clk Recovery)")
                broadcast_ws({"type": "alert", "msg": hw_tag + "I2C ACK-KILL: 50 µs SDA pull on Row 58 (Probe 2)"})
            elif cmd_c in ["S", "s"]:
                state["corrupted_frames"] = state.get("corrupted_frames", 0) + 1
                append_pcap_frame(3, 3, b"50us I2C0 Clock Stretch on Row 55 (SCL Held LOW -> Recovery)")
                broadcast_ws({"type": "alert", "msg": hw_tag + "I2C STRETCH: 50 µs SCL hold on Row 55 (Probe 3)"})
            elif cmd_c in ["W", "w"]:
                state["corrupted_frames"] = state.get("corrupted_frames", 0) + 1
                append_pcap_frame(3, 15, b"Targeted Zigbee Jam to Iris (02:00:00:00:00:03 Ch 15)")
                broadcast_ws({"type": "alert", "msg": hw_tag + "ZIGBEE JAM: Targeted burst to Iris on Ch 15"})
            elif cmd_c in ["E", "e"]:
                state["safety"] = "ABORTED"
                broadcast_ws({"type": "alert", "msg": hw_tag + "HARDWARE E-STOP: All probes floated in < 200 ns (FR-7)"})
            elif cmd_c in ["R", "r"]:
                state["safety"] = "ARMED"
                state["corrupted_frames"] = 0
                state["ber"] = "0.000%"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", "2")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"OK")
            return

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Content-Length", "0")
        self.end_headers()
def start_http_server():
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", HTTP_PORT), BridgeHttpHandler) as httpd:
        httpd.serve_forever()

async def main_async():
    global ws_event_loop
    ws_event_loop = asyncio.get_running_loop()
    # Start WS server
    async with websockets.serve(ws_handler, "localhost", WS_PORT):
        print(f"\n==================================================================")
        print(f"  WHATTHEFLIP // LOCALHOST BRIDGE ACTIVE")
        print(f"  Open in Browser: http://localhost:{HTTP_PORT}")
        print(f"  WebSocket Relay: ws://localhost:{WS_PORT}/ws")
        print(f"  Live Endpoints : WTF={PORT_WTF} | DAPHNE={PORT_DAPHNE} | IRIS={PORT_IRIS}")
        print(f"  (MacBook keeps 100% normal Wi-Fi / Internet connection)")
        print(f"==================================================================\n")
        await telemetry_broadcaster()

def main():
    # Spawn background serial readers
    threading.Thread(target=wtf_serial_reader, daemon=True).start()
    threading.Thread(target=daphne_serial_reader, daemon=True).start()
    threading.Thread(target=iris_serial_reader, daemon=True).start()
    
    # Spawn HTTP server
    threading.Thread(target=start_http_server, daemon=True).start()

    # Run WebSocket loop
    asyncio.run(main_async())

if __name__ == "__main__":
    main()
