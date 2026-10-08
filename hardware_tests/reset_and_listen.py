import time
import sys
import serial

ser = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=0.2)

# ESP32 USB-JTAG Normal Boot Reset Sequence:
# 1. Ensure BOOT (DTR) is HIGH (not pulled down)
ser.dtr = False
time.sleep(0.1)

# 2. Pulse RESET (RTS) LOW (active) then HIGH (release)
ser.rts = True
time.sleep(0.15)
ser.rts = False
time.sleep(0.2)
ser.dtr = False

print("Reset pulse sent. Listening for firmware output...")
start = time.time()
while time.time() - start < 10.0:
    data = ser.read(512)
    if data:
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()

ser.close()
print("\n[Done listening]")
