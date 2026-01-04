#!/usr/bin/env python3
"""Extended serial test for HubFX Pico"""
import serial
import time
import sys

def send_command(ser, cmd, delay=0.5):
    """Send command and read response"""
    print(f"\n> {cmd}")
    ser.write(f"{cmd}\n".encode())
    time.sleep(delay)
    
    response = ""
    while ser.in_waiting:
        data = ser.read(ser.in_waiting)
        response += data.decode(errors='ignore')
        time.sleep(0.1)
    
    if response:
        print(response.rstrip())
    else:
        print("(no response)")
    return response

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
    
    print(f"=== HubFX Extended Test ===")
    print(f"Connecting to {port}...\n")
    
    try:
        ser = serial.Serial(port, 115200, timeout=2)
        time.sleep(2)
        
        # Clear startup
        if ser.in_waiting:
            startup = ser.read(ser.in_waiting).decode(errors='ignore')
            if startup.strip():
                print("Startup messages:")
                print(startup.rstrip())
                print()
        
        tests = [
            ("System Info", [
                "help",
                "status",
            ]),
            ("Config Tests", [
                "config",
            ]),
            ("Storage Tests", [
                "sdinfo",
            ]),
            ("Engine Tests", [
                "engine",
            ]),
            ("Volume Test", [
                "volume 0.5",
                "volume 0.8",
            ]),
        ]
        
        for category, commands in tests:
            print("="*50)
            print(f"  {category}")
            print("="*50)
            for cmd in commands:
                send_command(ser, cmd, 0.5)
        
        print("\n" + "="*50)
        print("  All tests complete!")
        print("="*50)
        
        ser.close()
        return 0
        
    except serial.SerialException as e:
        print(f"Error: {e}")
        return 1
    except KeyboardInterrupt:
        print("\n\nInterrupted")
        return 0

if __name__ == '__main__':
    sys.exit(main())
