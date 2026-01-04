"""
Test JSON output functionality for HubFX Pico
Tests both normal operation and error conditions
"""

import serial
import time
import json

PORT = 'COM10'
BAUD = 115200

def send_command(ser, cmd):
    """Send command and capture response"""
    print(f"\n>>> {cmd}")
    ser.write(f"{cmd}\n".encode())
    time.sleep(0.3)
    
    response = ""
    while ser.in_waiting:
        response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        time.sleep(0.1)
    
    print(response)
    return response

def test_json_parsing(response, cmd):
    """Try to parse JSON from response"""
    try:
        # Find JSON in response (skip echo line)
        lines = response.strip().split('\n')
        for line in lines:
            if line.strip().startswith('{') or line.strip().startswith('['):
                json_obj = json.loads(line)
                print(f"[OK] Valid JSON for: {cmd}")
                print(f"  Parsed: {json_obj}")
                return True
    except json.JSONDecodeError as e:
        print(f"[FAIL] Invalid JSON for: {cmd}")
        print(f"  Error: {e}")
        return False
    
    print(f"[FAIL] No JSON found for: {cmd}")
    return False

def main():
    print("=" * 60)
    print("HubFX Pico - JSON Output Test")
    print("=" * 60)
    
    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
        time.sleep(2)  # Wait for serial to stabilize
        
        # Clear buffer
        ser.read_all()
        
        print("\n[TEST 1] Version command with JSON")
        resp = send_command(ser, "version --json")
        test_json_parsing(resp, "version --json")
        
        print("\n[TEST 2] Version command with -j flag")
        resp = send_command(ser, "version -j")
        test_json_parsing(resp, "version -j")
        
        print("\n[TEST 3] Status command with JSON")
        resp = send_command(ser, "status --json")
        test_json_parsing(resp, "status --json")
        
        print("\n[TEST 4] Engine status with JSON")
        resp = send_command(ser, "engine status --json")
        test_json_parsing(resp, "engine status --json")
        
        print("\n[TEST 5] SD card info with JSON")
        resp = send_command(ser, "sdinfo --json")
        test_json_parsing(resp, "sdinfo --json")
        
        print("\n[TEST 6] List directory with JSON")
        resp = send_command(ser, "ls --json")
        test_json_parsing(resp, "ls --json")
        
        print("\n[TEST 7] Tree with JSON")
        resp = send_command(ser, "tree --json")
        test_json_parsing(resp, "tree --json")
        
        print("\n[TEST 8] Error condition - invalid path with JSON")
        resp = send_command(ser, "ls /nonexistent --json")
        test_json_parsing(resp, "ls /nonexistent --json")
        
        print("\n[TEST 9] Error condition - invalid file with JSON")
        resp = send_command(ser, "cat /nonexistent.txt")
        # This should show error (not JSON since no flag)
        
        print("\n[TEST 10] Normal output (no JSON flag)")
        resp = send_command(ser, "version")
        if '{' not in resp:
            print("[OK] Normal text output (no JSON)")
        else:
            print("[FAIL] Unexpected JSON in normal output")
        
        print("\n" + "=" * 60)
        print("Test complete!")
        print("=" * 60)
        
        ser.close()
        
    except serial.SerialException as e:
        print(f"Serial error: {e}")
        print(f"Make sure Pico is connected to {PORT}")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    main()
