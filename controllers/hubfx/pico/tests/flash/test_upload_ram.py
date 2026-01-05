#!/usr/bin/env python3
"""
Flash upload test - RAM buffer approach.
Protocol: 
  1. Send command, wait for READY
  2. Stream all data (no flow control)
  3. Wait for RX progress updates
  4. Wait for WRITING... then DONE
"""
import serial
import time
import os
import sys

PORT = 'COM10'
BAUD = 115200

def upload_file(file_path, flash_path):
    file_size = os.path.getsize(file_path)
    print(f"Uploading {file_path} ({file_size} bytes) to {flash_path}")
    
    if file_size > 65536:
        print("ERROR: Max 64KB for RAM buffer approach")
        return False
    
    port = serial.Serial(PORT, BAUD, timeout=5)
    time.sleep(1)
    
    try:
        # Initialize flash
        port.reset_input_buffer()
        port.write(b"flash init\n")
        time.sleep(1)
        while port.in_waiting:
            print(port.readline().decode('utf-8', errors='ignore'), end='')
        
        # Send upload command
        cmd = f"flash upload {flash_path} {file_size}\n"
        print(f"\n>>> {cmd.strip()}")
        port.write(cmd.encode())
        
        # Wait for READY
        while True:
            if port.in_waiting:
                line = port.readline().decode('utf-8', errors='ignore').strip()
                print(f"< {line}")
                if line == "READY":
                    break
                if "ERR" in line:
                    return False
            time.sleep(0.01)
        
        # Stream entire file without waiting for ACKs
        print(f"\nSending {file_size} bytes...")
        with open(file_path, 'rb') as f:
            data = f.read()
            port.write(data)
        print(f"Sent {len(data)} bytes")
        
        # Wait for completion (WRITING... then DONE)
        print("\nWaiting for flash write...")
        timeout = time.time() + 30  # 30 sec for flash write
        
        while time.time() < timeout:
            if port.in_waiting:
                line = port.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(f"< {line}")
                    if line == "DONE":
                        print("\n✓ Upload complete!")
                        return True
                    if "ERR" in line:
                        print(f"\n✗ Error: {line}")
                        return False
            time.sleep(0.05)
        
        print("\n✗ Timeout waiting for completion")
        return False
            
    finally:
        port.close()

def verify_device():
    """Check device is responsive after upload"""
    print("\nVerifying device is responsive...")
    time.sleep(1)
    
    port = serial.Serial(PORT, BAUD, timeout=2)
    time.sleep(0.5)
    
    try:
        port.reset_input_buffer()
        port.write(b"flash ls /\n")
        time.sleep(1)
        
        response = ""
        while port.in_waiting:
            response += port.readline().decode('utf-8', errors='ignore')
        
        if response:
            print(response)
            return True
        else:
            print("No response from device!")
            return False
    finally:
        port.close()

if __name__ == "__main__":
    config_path = r"E:\data\OneDrive\CAD\CODE\scalefx\controllers\hubfx\pico\config.yaml"
    
    print("="*60)
    print("FLASH UPLOAD TEST - RAM BUFFER APPROACH")
    print("="*60)
    
    success = upload_file(config_path, "/config_test.yaml")
    
    if success:
        verify_device()
    
    sys.exit(0 if success else 1)
