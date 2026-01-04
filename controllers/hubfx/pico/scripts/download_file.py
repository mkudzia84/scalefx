#!/usr/bin/env python3
"""
Download a file from HubFX Pico SD card via serial port.
"""

import serial
import time
import sys
import os

def download_file(port, remote_path, local_path):
    """
    Download a file from the Pico SD card.
    
    Args:
        port: Serial port (e.g., 'COM10')
        remote_path: Remote file path (e.g., '/test.bin')
        local_path: Local file path to save to
    """
    print(f"Remote file: {remote_path}")
    print(f"Local file: {local_path}")
    print(f"Port: {port}")
    
    # Open serial connection
    try:
        ser = serial.Serial(port, 115200, timeout=5)
        time.sleep(2)  # Wait for device to be ready
        
        # Clear any pending data
        ser.reset_input_buffer()
        
        # Send download command
        cmd = f"sd download {remote_path}\r\n"
        print(f"\nSending command: {cmd.strip()}")
        ser.write(cmd.encode('ascii'))
        
        # Wait for SIZE and READY response
        file_size = None
        start_time = time.time()
        while time.time() - start_time < 5:
            if ser.in_waiting:
                line = ser.readline().decode('ascii', errors='ignore').strip()
                print(f"< {line}")
                
                if line.startswith("SIZE: "):
                    file_size = int(line.split(": ")[1])
                    print(f"File size: {file_size} bytes")
                elif line == "READY":
                    break
                elif line.startswith("ERROR"):
                    print("Download failed!")
                    ser.close()
                    return False
        
        if file_size is None:
            print("ERROR: Did not receive file size")
            ser.close()
            return False
        
        # Send START confirmation
        print("Sending START...")
        ser.write(b"START\n")
        ser.flush()
        time.sleep(0.1)
        
        # Receive file data
        print("\nDownloading...")
        bytes_received = 0
        
        with open(local_path, 'wb') as f:
            while bytes_received < file_size:
                if ser.in_waiting:
                    # Read available data
                    chunk = ser.read(ser.in_waiting)
                    
                    # Check for progress messages (text)
                    if b'\n' in chunk:
                        # Split on newlines to extract progress messages
                        parts = chunk.split(b'\n')
                        for i, part in enumerate(parts):
                            if b'PROGRESS:' in part or b'SUCCESS:' in part:
                                msg = part.decode('ascii', errors='ignore').strip()
                                print(f"< {msg}")
                            elif i < len(parts) - 1 or not part:
                                # Empty part or not the last part (has newline after it)
                                continue
                            else:
                                # Last part without newline - it's data
                                f.write(part)
                                bytes_received += len(part)
                    else:
                        # Pure binary data
                        f.write(chunk)
                        bytes_received += len(chunk)
                
                # Brief delay
                time.sleep(0.001)
        
        print(f"\n✓ Downloaded {bytes_received} bytes")
        
        # Wait for final messages
        time.sleep(0.5)
        while ser.in_waiting:
            line = ser.readline().decode('ascii', errors='ignore').strip()
            if line:
                print(f"< {line}")
        
        ser.close()
        return True
        
    except serial.SerialException as e:
        print(f"ERROR: Serial communication failed: {e}")
        return False
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python download_file.py <remote_path> <local_path> [port]")
        print("Example: python download_file.py /test.bin downloaded.bin COM10")
        sys.exit(1)
    
    remote_path = sys.argv[1]
    local_path = sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else "COM10"
    
    success = download_file(port, remote_path, local_path)
    sys.exit(0 if success else 1)
