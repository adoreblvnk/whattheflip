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

Auto-detects connected boards over USB serial:
  - whattheflip (ESP32-C6)
  - hestia (RP2040)
  - daphne (RP2040)
  - iris (ESP32-C6)
Mirrors [DASHBOARD:JSON] and [PCAP:PKT] directly from whattheflip silicon.
"""

import asyncio
import glob
import http.server
import json
import os
import re
import socketserver
import struct
import threading
import time
from typing import Set

import serial
import websockets

HTTP_PORT = 8888
WS_PORT = 8889
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WWW_DIR = os.path.join(BASE_DIR, "frontend", "dist", "WWW")

PORT_WTF = "/dev/cu.usbmodem101"
PORT_IRIS = "/dev/cu.usbmodem11301"

# Shared Global Hardware Telemetry State (mirrors whattheflip silicon JSON)
state = {
    "type": "telemetry",
    "uart_pkts": 0,
    "i2c_pkts": 0,
    "wifi_pkts": 0,
    "ieee154_pkts": 0,
    "fuzz_cycles": 0,
    "corrupted_frames": 0,
    "wifi_ch": 6,
    "wifi_mode": 0,
    "zigbee_ch": 0,
    "tool_mode": "OFF",
    "safety": "Active",
    "uart_locked": False,
    "i2c_locked": False,
    "pcap_bytes": 24,
    "accel": {"x": 0, "y": 0, "z": 0},
    "mag": {"x": 0, "y": 0, "z": 0},
    "iris_rssi": -70,
    "iris_verified": False,
    "uart_clear": False,
    "uart_sample": "",
    "wifi_clear": False,
    "wifi_sample_ch": 0,
    "z154_clear": False,
    "z154_sample": "",
    "ctf": {"uart": "", "i2c": "", "wifi": "", "zigbee": "", "bonus": "", "imask": 0, "emask": 0},
    "uptime_s": 0,
}

ser_wtf = None
ser_daphne = None
ser_iris = None
ser_lock = threading.Lock()
ws_clients: Set = set()
ws_event_loop = None

pcap_records = bytearray()
pcap_lock = threading.Lock()
pcap_records.extend(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 147))


def append_pcap_frame(proto_id: int, probe_id: int, payload_bytes: bytes):
    t = time.time()
    t_sec = int(t)
    t_usec = int((t - t_sec) * 1000000)
    full_payload = bytes([proto_id & 0xFF, probe_id & 0xFF]) + payload_bytes
    rec_hdr = struct.pack("<IIII", t_sec, t_usec, len(full_payload), len(full_payload))
    with pcap_lock:
        pcap_records.extend(rec_hdr + full_payload)
        state["pcap_bytes"] = len(pcap_records)


def reset_pcap_buffer():
    with pcap_lock:
        pcap_records.clear()
        pcap_records.extend(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 147))
        state["pcap_bytes"] = len(pcap_records)


def broadcast_ws(obj):
    if not ws_clients or ws_event_loop is None:
        return
    msg = json.dumps(obj)
    for ws in list(ws_clients):
        try:
            asyncio.run_coroutine_threadsafe(ws.send(msg), ws_event_loop)
        except Exception:
            pass


def wtf_write_serial(line_str: str) -> bool:
    with ser_lock:
        if ser_wtf is not None and ser_wtf.is_open:
            try:
                ser_wtf.write(line_str.encode("utf-8"))
                ser_wtf.flush()
                return True
            except Exception:
                return False
    return False


def wtf_serial_reader():
    global ser_wtf
    while True:
        try:
            if ser_wtf is None:
                ser_wtf = serial.Serial(PORT_WTF, 115200, timeout=0.2)
                print(f"[BRIDGE] Connected to whattheflip on {PORT_WTF}")
                ser_wtf.write(b"MODE:off\nCLEAR\n")
                ser_wtf.flush()
            line = ser_wtf.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            if line.startswith("[DASHBOARD:JSON] "):
                try:
                    j = json.loads(line[len("[DASHBOARD:JSON] "):])
                    for k, v in j.items():
                        if k != "pcap_bytes":
                            state[k] = v
                except Exception:
                    pass
            elif line.startswith("[PCAP:PKT] "):
                try:
                    parts = line[len("[PCAP:PKT] "):].split(",", 2)
                    if len(parts) == 3:
                        proto_id = int(parts[0])
                        probe_id = int(parts[1])
                        payload = bytes.fromhex(parts[2]) if parts[2] else b""
                        append_pcap_frame(proto_id, probe_id, payload)
                except Exception:
                    pass
            elif "[CTF] BONUS SECRET UNLOCKED" in line:
                broadcast_ws({"type": "alert", "msg": "All 4 protocols verified — Bonus Secret revealed"})
            elif "[SAFETY:ALERT]" in line:
                broadcast_ws({"type": "alert", "msg": "E-Stop active: whattheflip stopped"})
            elif "[SNIFFER:UART] AUTO-DETECT LOCKED:" in line:
                broadcast_ws({"type": "alert", "msg": line.replace("[SNIFFER:UART] ", "")})

        except Exception:
            ser_wtf = None
            time.sleep(1.0)


def find_daphne_port():
    for p in sorted(glob.glob("/dev/cu.usbmodem*")):
        if p in (PORT_WTF, PORT_IRIS):
            continue
        try:
            s = serial.Serial(p, 115200, timeout=0.3)
            time.sleep(0.8)
            txt = s.read(1024).decode("utf-8", errors="replace")
            s.close()
            if "DAPHNE" in txt:
                return p
        except Exception:
            pass
    return None


def daphne_serial_reader():
    global ser_daphne
    port = None
    while True:
        try:
            if ser_daphne is None:
                if port is None:
                    port = find_daphne_port()
                if port is None:
                    time.sleep(2.0)
                    continue
                ser_daphne = serial.Serial(port, 115200, timeout=0.2)
                print(f"[BRIDGE] Connected to daphne on {port}")

            line = ser_daphne.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            m_accel = re.search(r"ACCEL mg:\s*X=(-?\d+)\s*Y=(-?\d+)\s*Z=(-?\d+)", line)
            if m_accel:
                state["accel"] = {"x": int(m_accel.group(1)), "y": int(m_accel.group(2)), "z": int(m_accel.group(3))}

            m_mag = re.search(r"MAG:\s*X=(-?\d+)\s*Y=(-?\d+)\s*Z=(-?\d+)", line)
            if m_mag:
                state["mag"] = {"x": int(m_mag.group(1)), "y": int(m_mag.group(2)), "z": int(m_mag.group(3))}

        except Exception:
            ser_daphne = None
            port = None
            time.sleep(1.5)


def iris_serial_reader():
    global ser_iris
    while True:
        try:
            if ser_iris is None:
                ser_iris = serial.Serial()
                ser_iris.port = PORT_IRIS
                ser_iris.baudrate = 115200
                ser_iris.timeout = 0.2
                ser_iris.dtr = False
                ser_iris.rts = False
                ser_iris.open()
                print(f"[BRIDGE] Connected to iris on {PORT_IRIS}")

            line = ser_iris.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if "[IRIS:WIFI] Jam detected" in line or "[IRIS:802154] Jam detected" in line:
                broadcast_ws({"type": "alert", "msg": line})
        except Exception:
            ser_iris = None
            time.sleep(1.5)


async def ws_handler(websocket):
    ws_clients.add(websocket)
    try:
        await websocket.send(json.dumps(state))
        async for _ in websocket:
            pass
    finally:
        ws_clients.discard(websocket)


async def telemetry_broadcaster():
    while True:
        await asyncio.sleep(0.5)
        if ws_clients:
            broadcast_ws(state)


class BridgeHttpHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WWW_DIR, **kwargs)

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        url = self.path.split("?")[0]
        if url in ("/", "/INDEX.HTM", "/index.html"):
            self.path = "/INDEX.HTM"
            return super().do_GET()
        elif url in ("/app.js", "/APP.JS"):
            data = open(os.path.join(WWW_DIR, "APP.JS"), "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/javascript")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        elif url in ("/style.css", "/STYLE.CSS"):
            data = open(os.path.join(WWW_DIR, "STYLE.CSS"), "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "text/css")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        elif url == "/api/pcap":
            req_offset = 0
            req_limit = 4096
            if "?" in self.path:
                q = self.path.split("?")[1]
                parts = dict(p.split("=") for p in q.split("&") if "=" in p)
                try: req_offset = int(parts.get("offset", 0))
                except ValueError: pass
                try: req_limit = int(parts.get("limit", 4096))
                except ValueError: pass

            with pcap_lock:
                total_sz = len(pcap_records)
                chunk = bytes(pcap_records[req_offset:req_offset + req_limit])

            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Expose-Headers", "X-Pcap-Total-Bytes, X-Pcap-Offset")
            self.send_header("X-Pcap-Total-Bytes", str(total_sz))
            self.send_header("X-Pcap-Offset", str(req_offset))
            self.send_header("Content-Length", str(len(chunk)))
            self.end_headers()
            self.wfile.write(chunk)
            return
        elif url == "/capture.pcap":
            with pcap_lock:
                data = bytes(pcap_records)
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.tcpdump.pcap")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Disposition", 'attachment; filename="capture.pcap"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        elif url == "/telemetry":
            data = json.dumps(state).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        return super().do_GET()

    def do_POST(self):
        url = self.path.split("?")[0]
        if url == "/cmd":
            q = self.path.split("?")[1] if "?" in self.path else ""
            parts = dict(p.split("=") for p in q.split("&") if "=" in p)

            if "c" in parts:
                c = parts["c"].upper()
                wtf_write_serial(f"{c}\n")
                if c == "C":
                    reset_pcap_buffer()
                    state["uart_pkts"] = 0
                    state["i2c_pkts"] = 0
                    state["wifi_pkts"] = 0
                    state["ieee154_pkts"] = 0
                    state["fuzz_cycles"] = 0
                    state["corrupted_frames"] = 0
                    state["uart_clear"] = False
                    state["uart_sample"] = ""
                    state["wifi_clear"] = False
                    state["wifi_sample_ch"] = 0
                    state["wifi_sample"] = ""
                    state["z154_clear"] = False
                    state["z154_sample"] = ""
                    state["ctf"] = {"uart": "", "i2c": "", "wifi": "", "zigbee": "", "bonus": "", "imask": 0, "emask": 0}
                elif c == "E":
                    state["safety"] = "Stopped"
                elif c == "R":
                    state["safety"] = "Active"
            if "mode" in parts:
                m = parts["mode"].upper()
                state["tool_mode"] = m
                if m == "WIFI":
                    state["wifi_mode"] = 0
                    state["wifi_sample_ch"] = 0
                    state["wifi_clear"] = False
                    state["wifi_sample"] = ""
                elif m == "ZIGBEE":
                    state["zigbee_ch"] = 0
                    state["z154_clear"] = False
                    state["z154_sample"] = ""
                wtf_write_serial(f"MODE:{m.lower()}\n")
            if "baud" in parts:
                b = parts["baud"]
                state["uart_baud"] = int(b)
                state["uart_clear"] = False
                state["uart_sample"] = ""
                wtf_write_serial(f"BAUD:{b}\n")
            if "uart_auto" in parts:
                wtf_write_serial("BAUD:AUTO\n")
            if "ch" in parts:
                ch = int(parts["ch"])
                state["wifi_mode"] = ch
                state["wifi_sample_ch"] = ch
                state["wifi_clear"] = False
                state["wifi_sample"] = ""
                wtf_write_serial(f"CH{ch}\n")
            if "wifi_auto" in parts:
                state["wifi_clear"] = False
                state["wifi_sample"] = ""
                wtf_write_serial("WIFI:AUTO\n")
            if "wifi_jam" in parts:
                ch = int(parts["wifi_jam"])
                state["wifi_clear"] = False
                state["wifi_sample"] = ""
                wtf_write_serial(f"JAMWIFI:{ch}\n")
            if "zch" in parts:
                zch = int(parts["zch"])
                state["zigbee_ch"] = zch
                state["z154_clear"] = False
                state["z154_sample"] = ""
                wtf_write_serial(f"ZCH{zch}\n")
            if "z_auto" in parts:
                state["z154_clear"] = False
                state["z154_sample"] = ""
                wtf_write_serial("ZIGBEE:AUTO\n")

            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", "2")
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
    async with websockets.serve(ws_handler, "localhost", WS_PORT):
        print(f"WHATTHEFLIP // LOCALHOST BRIDGE ACTIVE on http://localhost:{HTTP_PORT}")
        await telemetry_broadcaster()


def main():
    threading.Thread(target=wtf_serial_reader, daemon=True).start()
    threading.Thread(target=daphne_serial_reader, daemon=True).start()
    threading.Thread(target=iris_serial_reader, daemon=True).start()
    threading.Thread(target=start_http_server, daemon=True).start()
    asyncio.run(main_async())


if __name__ == "__main__":
    main()
