#!/usr/bin/env python3
"""Test flash file removal and config upload"""

import serial
import time

PORT = 'COM10'
BAUD = 115200

def send_command(ser, cmd, wait=0.5):
    """Send command and print response"""
    print(f"\n> {cmd}")
    ser.write(cmd.encode() + b'\n')
    time.sleep(wait)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
    print(response)
    return response

def main():
    print("=== Flash Cleanup Test ===\n")
    
    with serial.Serial(PORT, BAUD, timeout=2) as ser:
        time.sleep(2)  # Wait for device
        
        # Initialize flash
        send_command(ser, 'flash init')
        
        # List files before
        print("\n--- Files BEFORE cleanup ---")
        send_command(ser, 'flash ls')
        
        # Remove test files
        print("\n--- Removing test files ---")
        send_command(ser, 'flash rm /tiny.txt')
        send_command(ser, 'flash rm /test.txt')
        send_command(ser, 'flash rm /config.yaml.bak')
        send_command(ser, 'flash rm /config_deadlock_test.yaml')
        send_command(ser, 'flash rm /config_test.yaml')
        
        # List files after
        print("\n--- Files AFTER cleanup ---")
        send_command(ser, 'flash ls')
        
        # Show config file
        print("\n--- Config file contents ---")
        send_command(ser, 'flash cat /config.yaml', wait=1.0)
        
    print("\n✓ Test complete!")

if __name__ == '__main__':
    main()
