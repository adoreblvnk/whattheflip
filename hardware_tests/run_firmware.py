import time
import sys
import serial
import esptool

# 1. Connect via esptool and jump to flashed user code
print("[1] Connecting to ESP32-C6...")
esp = esptool.cmds.detect_chip(port='/dev/cu.usbmodem101', baud=115200)
stub = esp.run_stub()
print("[2] Jumping into flashed firmware...")
stub.run()
stub._port.close()

# 2. Wait for USB CDC re-enumeration
print("[3] Waiting 1.5s for USB CDC re-enumeration...")
time.sleep(1.5)

# 3. Open fresh serial connection to the running firmware
print("[4] Opening live serial monitor on /dev/cu.usbmodem101...")
for attempt in range(5):
    try:
        ser = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=0.2)
        print("[5] Connected! Streaming live test output:\n")
        start = time.time()
        while time.time() - start < 15.0:
            data = ser.read(512)
            if data:
                sys.stdout.write(data.decode('utf-8', errors='replace'))
                sys.stdout.flush()
        ser.close()
        break
    except Exception as e:
        print(f"Waiting for port (attempt {attempt+1}/5)...")
        time.sleep(0.5)
