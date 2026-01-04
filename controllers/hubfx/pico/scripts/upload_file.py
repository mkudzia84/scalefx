#!/usr/bin/env python3
"""
Upload a file to HubFX Pico SD card or flash via serial port.
"""

import serial
import time
import sys
import os

def upload_file(port, filepath, dest_path, target='sd'):
    """
    Upload a file to the Pico SD card or flash.
    
    Args:
        port: Serial port (e.g., 'COM10')
        filepath: Local file path to upload
        dest_path: Destination path (e.g., '/config.yaml')
        target: 'sd' or 'flash' (default: 'sd')
    """
    if not os.path.exists(filepath):
        print(f"ERROR: File not found: {filepath}")
        return False
    
    file_size = os.path.getsize(filepath)
    print(f"File: {filepath}")
    print(f"Size: {file_size} bytes")
    print(f"Destination: {dest_path}")
    print(f"Target: {target.upper()}")
    
    # Check file size limits
    if target == 'flash' and file_size > 1048576:
        print("ERROR: Flash uploads limited to 1MB")
        return False
    elif target == 'sd' and file_size > 104857600:
        print("ERROR: SD uploads limited to 100MB")
        return False
    
    # Open serial connection
    try:
        ser = serial.Serial(port, 115200, timeout=5)
        time.sleep(2)  # Wait for device to be ready
        
        # Clear any pending data
        ser.reset_input_buffer()
        
        # Send upload command
        cmd = f"{target} upload {dest_path} {file_size}\r\n"
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
                
                # Read progress updates and flow control
                while ser.in_waiting:
                    byte = ser.read(1)
                    if byte == b'\x13':  # XOFF
                        print("< [XOFF] Pausing...")
                        # Wait for XON
                        while True:
                            byte = ser.read(1)
                            if byte == b'\x11':  # XON
                                print("< [XON] Resuming...")
                                break
                    else:
                        # Try to read a full line
                        line = byte + ser.read_until(b'\n', size=256)
                        line_str = line.decode('ascii', errors='ignore').strip()
                        if line_str:
                            print(f"< {line_str}")
                
                # Brief delay to avoid overwhelming the buffer
                time.sleep(0.01)
        
        # Wait for completion message
        print("\nWaiting for completion...")
        start_time = time.time()
        while time.time() - start_time < 10:
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
        print("Usage: python upload_file.py <local_file> <dest_path> [port] [target]")
        print("Example: python upload_file.py config.yaml /config.yaml COM10 flash")
        print("         python upload_file.py config.yaml /config.yaml COM10 sd")
        print("Target: 'sd' or 'flash' (default: sd)")
        sys.exit(1)
    
    local_file = sys.argv[1]
    dest_path = sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else "COM10"
    target = sys.argv[4].lower() if len(sys.argv) > 4 else "sd"
    
    if target not in ['sd', 'flash']:
        print("ERROR: Target must be 'sd' or 'flash'")
        sys.exit(1)
    
    success = upload_file(port, local_file, dest_path, target)
    sys.exit(0 if success else 1)
