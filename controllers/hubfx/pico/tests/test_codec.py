#!/usr/bin/env python3
import serial
import time

port = serial.Serial('COM10', 115200, timeout=2)
time.sleep(0.5)

commands = [
    "codec scan",
    "codec test",
    "codec status"
]

for cmd in commands:
    print(f"\n{'='*60}")
    print(f"Sending: {cmd}")
    print('='*60)
    port.write((cmd + '\n').encode())
    time.sleep(2)
    response = port.read(port.in_waiting or 2048).decode('utf-8', errors='ignore')
    print(response)

port.close()
