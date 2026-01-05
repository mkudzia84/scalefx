#!/usr/bin/env python3
"""
Test flash upload with flow control protocol.
Device sends NEXT:<bytes> after each chunk - host waits for it.
"""
import serial
import time
import os
import sys

PORT = 'COM10'
BAUD = 115200
CHUNK_SIZE = 64  # Must match device

def upload_with_flow_control(port, file_path, flash_path):
    """Upload file using flow-controlled protocol"""
    if not os.path.exists(file_path):
        print(f"ERROR: File not found: {file_path}")
        return False
    
    file_size = os.path.getsize(file_path)
    print(f"\n{'='*60}")
    print(f"UPLOADING: {os.path.basename(file_path)}")
    print(f"Size: {file_size} bytes")
    print(f"Chunk size: {CHUNK_SIZE} bytes")
    print(f"Destination: {flash_path}")
    print(f"{'='*60}")
    
    # Send upload command
    cmd = f"flash upload {flash_path} {file_size}\n"
    print(f">>> {cmd.strip()}")
    port.write(cmd.encode())
    
    # Wait for READY
    start = time.time()
    ready = False
    while time.time() - start < 5:
        if port.in_waiting:
            line = port.readline().decode('utf-8', errors='ignore').strip()
            print(f"< {line}")
            if 'READY' in line:
                ready = True
                break
            if 'ERROR' in line:
                return False
        time.sleep(0.01)
    
    if not ready:
        print("ERROR: No READY response")
        return False
    
    # Send file in chunks, waiting for NEXT after each
    start_time = time.time()
    bytes_sent = 0
    
    with open(file_path, 'rb') as f:
        while bytes_sent < file_size:
            # Read and send chunk
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break
            
            port.write(chunk)
            bytes_sent += len(chunk)
            
            # Wait for NEXT or SUCCESS/ERROR
            got_response = False
            timeout = time.time() + 30  # 30 sec timeout - flash erase can take 100ms+
            
            while time.time() < timeout:
                if port.in_waiting:
                    line = port.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        print(f"< {line}")
                        if line.startswith('NEXT:'):
                            got_response = True
                            break
                        if 'SUCCESS' in line:
                            got_response = True
                            break
                        if 'ERROR' in line:
                            print(f"ERROR from device: {line}")
                            return False
                time.sleep(0.01)
            
            if not got_response:
                print(f"ERROR: No response after sending {bytes_sent} bytes")
                return False
            
            # Progress
            percent = (bytes_sent / file_size) * 100
            print(f"  Sent: {bytes_sent}/{file_size} ({percent:.1f}%)")
    
    elapsed = time.time() - start_time
    print(f"\n{'='*60}")
    print(f"Upload completed in {elapsed:.2f} seconds")
    print(f"Throughput: {bytes_sent / elapsed:.0f} bytes/sec")
    print(f"{'='*60}")
    return True

def main():
    print("="*60)
    print("FLASH UPLOAD WITH FLOW CONTROL")
    print("="*60)
    
    # Open port
    try:
        port = serial.Serial(PORT, BAUD, timeout=1)
        time.sleep(1)  # Wait for device
    except Exception as e:
        print(f"ERROR: Cannot open {PORT}: {e}")
        return 1
    
    try:
        # Initialize flash
        print("\n[1/3] Initializing flash...")
        port.write(b"flash init\n")
        time.sleep(2)
        while port.in_waiting:
            print(port.readline().decode('utf-8', errors='ignore'), end='')
        
        # Upload file
        print("\n[2/3] Uploading config.yaml with flow control...")
        config_path = r"E:\data\OneDrive\CAD\CODE\scalefx\controllers\hubfx\pico\config.yaml"
        success = upload_with_flow_control(port, config_path, "/config_test.yaml")
        
        if not success:
            print("\n*** UPLOAD FAILED ***")
            return 1
        
        # Verify device responsive
        print("\n[3/3] Verifying device is responsive...")
        time.sleep(1)
        port.reset_input_buffer()
        port.write(b"flash ls /\n")
        time.sleep(1)
        
        response = ""
        while port.in_waiting:
            response += port.readline().decode('utf-8', errors='ignore')
        
        if response:
            print(response)
            print("\n*** SUCCESS - Device is responsive! ***")
            return 0
        else:
            print("WARNING: No response from device")
            return 1
            
    finally:
        port.close()

if __name__ == "__main__":
    sys.exit(main())
