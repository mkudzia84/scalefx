#!/usr/bin/env python3
import serial
import time

port = serial.Serial('COM10', 115200, timeout=2)
port.write(b'reboot\n')
port.close()

time.sleep(1.5)

port = serial.Serial('COM10', 115200, timeout=3)
print("=== Waiting for boot ===")
time.sleep(3.5)

try:
    if port.in_waiting > 0:
        boot_log = port.read(port.in_waiting).decode('utf-8', errors='ignore')
        print(boot_log)
    else:
        print("No boot data received")
except Exception as e:
    print(f"Error: {e}")

port.close()
