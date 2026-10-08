import serial
import time
import sys

# Configure port without asserting DTR/RTS
ser = serial.Serial()
ser.port = '/dev/cu.usbmodem101'
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

# Send a soft reset command or monitor
print("Opened port. Listening for 5 seconds...")
start = time.time()
while time.time() - start < 5.0:
    data = ser.read(512)
    if data:
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()

ser.close()
