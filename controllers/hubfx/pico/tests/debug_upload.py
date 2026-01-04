#!/usr/bin/env python3
"""Minimal flash upload test to debug hanging issue"""

import serial
import time
import sys

def test_minimal_upload():
    """Test with very small data"""
    port = 'COM10'
    
    print("Opening serial port...")
    s = serial.Serial(port, 115200, timeout=5)
    time.sleep(0.5)
    
    # Initialize flash first
    print("\n0. Initializing flash...")
    s.write(b'flash init\r\n')
    time.sleep(1)
    while s.in_waiting:
        line = s.readline().decode('ascii', errors='ignore').strip()
        if line:
            print(f"   < {line}")
    
    # Send upload command for tiny file
    test_data = b"Hello"
    size = len(test_data)
    
    print(f"\n1. Sending command: flash upload /tiny.txt {size}")
    s.write(f'flash upload /tiny.txt {size}\r\n'.encode())
    
    # Read response
    print("\n2. Waiting for READY...")
    start = time.time()
    while time.time() - start < 3:
        if s.in_waiting:
            line = s.readline().decode('ascii', errors='ignore').strip()
            print(f"   < {line}")
            if "READY" in line:
                break
    else:
        print("   ERROR: No READY received!")
        s.close()
        return False
    
    # Send data
    print(f"\n3. Sending {size} bytes of data...")
    s.write(test_data)
    s.flush()
    print(f"   Sent: {test_data}")
    
    # Read responses
    print("\n4. Waiting for responses (10s timeout)...")
    start = time.time()
    success = False
    while time.time() - start < 10:
        if s.in_waiting:
            line = s.readline().decode('ascii', errors='ignore').strip()
            if line:
                print(f"   < {line}")
                if "SUCCESS" in line:
                    success = True
                    break
                elif "ERROR" in line:
                    print("   Upload failed!")
                    break
        time.sleep(0.1)
    
    if not success:
        print("\n   TIMEOUT: No SUCCESS message received!")
        print("   Device may be stuck in upload handler")
    
    s.close()
    return success

if __name__ == "__main__":
    print("=== Minimal Flash Upload Test ===\n")
    result = test_minimal_upload()
    print(f"\n=== Test {'PASSED' if result else 'FAILED'} ===")
    sys.exit(0 if result else 1)
