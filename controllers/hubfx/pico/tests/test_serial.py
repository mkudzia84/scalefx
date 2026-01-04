#!/usr/bin/env python3
"""Quick serial test for HubFX Pico"""
import serial
import time
import sys

def send_command(ser, cmd, delay=0.5):
    """Send command and read response"""
    print(f"\n> {cmd}")
    ser.write(f"{cmd}\n".encode())
    time.sleep(delay)
    
    # Read all available data
    response = ""
    while ser.in_waiting:
        data = ser.read(ser.in_waiting)
        response += data.decode(errors='ignore')
        time.sleep(0.1)
    
    if response:
        print(response.rstrip())
    return response

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
    
    print(f"=== HubFX Serial Test ===")
    print(f"Connecting to {port}...")
    
    try:
        ser = serial.Serial(port, 115200, timeout=2)
        time.sleep(2)  # Wait for connection
        
        # Clear any startup messages
        if ser.in_waiting:
            startup = ser.read(ser.in_waiting).decode(errors='ignore')
            if startup:
                print("\nStartup messages:")
                print(startup.rstrip())
        
        # Run basic tests
        print("\n" + "="*50)
        print("BASIC CHECKS")
        print("="*50)
        
        send_command(ser, "help", 1)
        send_command(ser, "status", 0.5)
        send_command(ser, "config", 0.5)
        send_command(ser, "sdinfo", 0.5)
        
        print("\n" + "="*50)
        print("Tests complete!")
        print("="*50)
        
        ser.close()
        return 0
        
    except serial.SerialException as e:
        print(f"Error: {e}")
        print("\nTroubleshooting:")
        print("  - Close any open serial monitors (Ctrl+C)")
        print("  - Verify Pico is connected")
        print("  - Check port with: python -m serial.tools.list_ports")
        return 1
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        return 0

if __name__ == '__main__':
    sys.exit(main())
