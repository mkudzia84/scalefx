#!/usr/bin/env python3
"""
Upload a file to HubFX Pico SD card via serial port.
"""

import serial
import time
import sys
import os

def upload_file(port, filepath, dest_path):
    """
    Upload a file to the Pico SD card.
    
    Args:
        port: Serial port (e.g., 'COM10')
        filepath: Local file path to upload
        dest_path: Destination path on SD card (e.g., '/config.yaml')
    """
    if not os.path.exists(filepath):
        print(f"ERROR: File not found: {filepath}")
        return False
    
    file_size = os.path.getsize(filepath)
    print(f"File: {filepath}")
    print(f"Size: {file_size} bytes")
    print(f"Destination: {dest_path}")
    
    # Open serial connection
    try:
        ser = serial.Serial(port, 115200, timeout=5)
        time.sleep(2)  # Wait for device to be ready
        
        # Clear any pending data
        ser.reset_input_buffer()
        
        # Send upload command
        cmd = f"upload {dest_path} {file_size}\r\n"
        print(f"\nSending command: {cmd.strip()}")
        ser.write(cmd.encode('ascii'))
        
        # Wait for READY response
        response = ""
        start_time = time.time()
        while time.time() - start_time < 5:
            if ser.in_waiting:
                line = ser.readline().decode('ascii', errors='ignore').strip()
                print(f"< {line}")
                if line == "READY":
                    break
                response += line + "\n"
        else:
            print("ERROR: Device not ready")
            ser.close()
            return False
        
        # Upload file in chunks
        print("\nUploading...")
        with open(filepath, 'rb') as f:
            bytes_sent = 0
            chunk_size = 512
            
            while bytes_sent < file_size:
                chunk = f.read(chunk_size)
                ser.write(chunk)
                bytes_sent += len(chunk)
                
                # Read progress updates
                while ser.in_waiting:
                    line = ser.readline().decode('ascii', errors='ignore').strip()
                    if line:
                        print(f"< {line}")
                
                # Brief delay to avoid overwhelming the buffer
                time.sleep(0.01)
        
        # Wait for completion message
        print("\nWaiting for completion...")
        start_time = time.time()
        while time.time() - start_time < 5:
            if ser.in_waiting:
                line = ser.readline().decode('ascii', errors='ignore').strip()
                if line:
                    print(f"< {line}")
                    if "SUCCESS" in line:
                        print("\n✓ Upload successful!")
                        ser.close()
                        return True
                    elif "ERROR" in line:
                        print("\n✗ Upload failed!")
                        ser.close()
                        return False
            time.sleep(0.1)
        
        ser.close()
        print("\n⚠ Upload status unclear (timeout)")
        return False
        
    except serial.SerialException as e:
        print(f"ERROR: Serial communication failed: {e}")
        return False
    except Exception as e:
        print(f"ERROR: {e}")
        return False

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python upload_file.py <local_file> <dest_path> [port]")
        print("Example: python upload_file.py ../../../config.yaml /config.yaml COM10")
        sys.exit(1)
    
    local_file = sys.argv[1]
    dest_path = sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else "COM10"
    
    success = upload_file(port, local_file, dest_path)
    sys.exit(0 if success else 1)
