#!/usr/bin/env python3
"""
Test I2C bus scanning to detect WM8960 at 0x1A
"""
import serial
import time

port = serial.Serial('COM10', 115200, timeout=2)
time.sleep(0.5)

print("="*60)
print("I2C Bus Scan Test")
print("="*60)
print("Scanning for WM8960 at address 0x1A...")
print()

port.write(b'codec scan\n')
time.sleep(2)

response = port.read(port.in_waiting or 1024).decode('utf-8', errors='ignore')
print(response)

if "0x1A" in response or "0x1a" in response:
    print("\n✓ SUCCESS: WM8960 detected at 0x1A")
    print("  The HAT is powered and I2C is working!")
elif "No I2C devices found" in response:
    print("\n✗ FAILURE: No I2C devices detected")
    print("  Possible issues:")
    print("  - HAT not powered (check 3.3V connection)")
    print("  - I2C pins not connected (GP4/GP5)")
    print("  - Ground not connected")
    print("  - HAT defective")
elif "Error: No codec configured" in response:
    print("\n✗ ERROR: Codec not initialized in firmware")
else:
    print("\n? UNKNOWN: Check output above")

port.close()
