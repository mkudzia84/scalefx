#!/usr/bin/env python3
"""
Simple flash upload test with flow control.
Protocol: Send chunk, wait for OK, repeat until DONE.
"""
import serial
import time
import os
import sys

PORT = 'COM10'
BAUD = 115200
CHUNK_SIZE = 128  # Must match device

def upload_file(file_path, flash_path):
    file_size = os.path.getsize(file_path)
    print(f"Uploading {file_path} ({file_size} bytes) to {flash_path}")
    
    port = serial.Serial(PORT, BAUD, timeout=2)
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
            line = port.readline().decode('utf-8', errors='ignore').strip()
            print(f"< {line}")
            if line == "READY":
                break
            if line == "ERR" or "Error" in line:
                print("Device not ready")
                return False
        
        # Send file in chunks, wait for OK after each
        with open(file_path, 'rb') as f:
            sent = 0
            chunk_num = 0
            
            while sent < file_size:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                
                port.write(chunk)
                sent += len(chunk)
                chunk_num += 1
                
                # Wait for OK
                response = port.readline().decode('utf-8', errors='ignore').strip()
                print(f"  Chunk {chunk_num}: sent {len(chunk)} bytes, total {sent}/{file_size} -> {response}")
                
                if response == "DONE":
                    print("\n✓ Upload complete!")
                    return True
                elif response != "OK":
                    print(f"\n✗ Unexpected response: {response}")
                    return False
        
        # Should get DONE after last chunk
        response = port.readline().decode('utf-8', errors='ignore').strip()
        print(f"< {response}")
        
        if response == "DONE":
            print("\n✓ Upload complete!")
            return True
        else:
            print(f"\n✗ Expected DONE, got: {response}")
            return False
            
    finally:
        port.close()

def verify_device():
    """Check device is responsive after upload"""
    print("\nVerifying device is responsive...")
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
    print("FLASH UPLOAD TEST - YIELD() APPROACH")
    print("="*60)
    
    success = upload_file(config_path, "/config_test.yaml")
    
    if success:
        verify_device()
    
    sys.exit(0 if success else 1)
