#!/usr/bin/env python3
"""
Upload config.yaml to HubFX Pico SD card with verification.

Features:
- Uploads config.yaml to SD card
- Verifies upload by reading back file info
- Triggers config reload
- Validates configuration after reload

Usage:
    python upload_config.py [config_file] [port]
    python upload_config.py                      # Uses ./config.yaml, auto-detect port
    python upload_config.py config.yaml COM10
"""

import serial
import serial.tools.list_ports
import time
import sys
import os
import json
import hashlib


class ConfigUploader:
    def __init__(self, port=None, baudrate=115200):
        self.port = port or self._find_pico_port()
        self.baudrate = baudrate
        self.ser = None
    
    def _find_pico_port(self):
        """Auto-detect Pico COM port."""
        ports = serial.tools.list_ports.comports()
        for p in ports:
            # Look for Pico USB identifiers
            if "2E8A" in (p.vid and hex(p.vid) or ""):
                return p.device
            if "Pico" in (p.description or ""):
                return p.device
        # Fall back to common port
        return "COM10"
    
    def connect(self):
        """Establish serial connection."""
        print(f"Connecting to {self.port}...")
        self.ser = serial.Serial(self.port, self.baudrate, timeout=3)
        time.sleep(1)  # Wait for device
        self.ser.reset_input_buffer()
        return True
    
    def disconnect(self):
        """Close serial connection."""
        if self.ser and self.ser.is_open:
            self.ser.close()
    
    def send_command(self, cmd, delay=0.5):
        """Send command and return response."""
        self.ser.reset_input_buffer()
        self.ser.write(f"{cmd}\r\n".encode('ascii'))
        time.sleep(delay)
        
        response = ""
        while self.ser.in_waiting:
            response += self.ser.read(self.ser.in_waiting).decode('ascii', errors='ignore')
            time.sleep(0.1)
        
        return response.strip()
    
    def send_json_command(self, cmd, delay=0.5):
        """Send command with --json flag and parse response."""
        response = self.send_command(f"{cmd} --json", delay)
        
        # Find JSON in response (may have prompt chars)
        try:
            # Look for JSON object
            start = response.find('{')
            if start >= 0:
                # Find matching close brace
                depth = 0
                for i, c in enumerate(response[start:], start):
                    if c == '{':
                        depth += 1
                    elif c == '}':
                        depth -= 1
                        if depth == 0:
                            return json.loads(response[start:i+1])
        except json.JSONDecodeError:
            pass
        
        return None
    
    def upload_file(self, local_path, remote_path):
        """Upload file to SD card."""
        if not os.path.exists(local_path):
            print(f"ERROR: File not found: {local_path}")
            return False
        
        file_size = os.path.getsize(local_path)
        
        # Calculate local MD5
        with open(local_path, 'rb') as f:
            local_hash = hashlib.md5(f.read()).hexdigest()
        
        print(f"  Local file: {local_path}")
        print(f"  Size: {file_size} bytes")
        print(f"  MD5: {local_hash}")
        
        # Send upload command
        cmd = f"sd upload {remote_path} {file_size}"
        self.ser.reset_input_buffer()
        self.ser.write(f"{cmd}\r\n".encode('ascii'))
        
        # Wait for READY
        start = time.time()
        ready = False
        while time.time() - start < 5:
            if self.ser.in_waiting:
                line = self.ser.readline().decode('ascii', errors='ignore').strip()
                if "READY" in line:
                    ready = True
                    break
                elif "ERROR" in line:
                    print(f"  ERROR: {line}")
                    return False
        
        if not ready:
            print("  ERROR: Device not ready for upload")
            return False
        
        # Upload file data
        print("  Uploading...")
        with open(local_path, 'rb') as f:
            bytes_sent = 0
            chunk_size = 512
            
            while bytes_sent < file_size:
                chunk = f.read(chunk_size)
                self.ser.write(chunk)
                bytes_sent += len(chunk)
                
                # Handle flow control
                while self.ser.in_waiting:
                    byte = self.ser.read(1)
                    if byte == b'\x13':  # XOFF
                        while True:
                            byte = self.ser.read(1)
                            if byte == b'\x11':  # XON
                                break
                
                time.sleep(0.01)
        
        # Wait for completion
        start = time.time()
        while time.time() - start < 10:
            if self.ser.in_waiting:
                line = self.ser.readline().decode('ascii', errors='ignore').strip()
                if "SUCCESS" in line:
                    print("  ✓ Upload complete")
                    return True
                elif "ERROR" in line:
                    print(f"  ✗ {line}")
                    return False
        
        print("  ⚠ Upload status unclear (timeout)")
        return False
    
    def verify_upload(self, remote_path, expected_size):
        """Verify uploaded file exists and has correct size."""
        response = self.send_json_command(f"sd info {remote_path}")
        
        if response and response.get("status") == "ok":
            actual_size = response.get("size", 0)
            if actual_size == expected_size:
                print(f"  ✓ File verified: {actual_size} bytes")
                return True
            else:
                print(f"  ✗ Size mismatch: expected {expected_size}, got {actual_size}")
                return False
        else:
            # Try text mode
            text_response = self.send_command(f"sd info {remote_path}")
            if str(expected_size) in text_response:
                print(f"  ✓ File verified")
                return True
        
        print("  ⚠ Could not verify file")
        return False
    
    def reload_config(self):
        """Trigger configuration reload."""
        print("  Reloading configuration...")
        response = self.send_command("config reload", delay=1.0)
        
        if "success" in response.lower() or "loaded" in response.lower():
            print("  ✓ Configuration reloaded")
            return True
        elif "error" in response.lower():
            print(f"  ✗ Reload failed: {response}")
            return False
        else:
            print("  ✓ Reload command sent")
            return True
    
    def validate_config(self):
        """Validate configuration after reload."""
        response = self.send_json_command("config display")
        
        if response and response.get("status") == "ok":
            config = response.get("config", {})
            
            # Check required sections
            required = ["engine", "gun"]
            missing = [s for s in required if s not in config]
            
            if missing:
                print(f"  ⚠ Missing sections: {missing}")
                return False
            
            print("  ✓ Configuration valid")
            
            # Show summary
            if "engine" in config:
                engine = config["engine"]
                print(f"      Engine: enabled={engine.get('enabled', 'N/A')}")
            if "gun" in config:
                gun = config["gun"]
                print(f"      Gun: modes={len(gun.get('modes', []))}")
            
            return True
        else:
            # Try text mode
            text_response = self.send_command("config display")
            if "engine:" in text_response.lower() or "gun:" in text_response.lower():
                print("  ✓ Configuration appears valid")
                return True
        
        print("  ⚠ Could not validate configuration")
        return False


def main():
    print("")
    print("╔══════════════════════════════════════════╗")
    print("║  HubFX Pico - Config Upload Utility      ║")
    print("╚══════════════════════════════════════════╝")
    print("")
    
    # Parse arguments
    config_file = sys.argv[1] if len(sys.argv) > 1 else "config.yaml"
    port = sys.argv[2] if len(sys.argv) > 2 else None
    
    if not os.path.exists(config_file):
        print(f"ERROR: Config file not found: {config_file}")
        sys.exit(1)
    
    file_size = os.path.getsize(config_file)
    
    uploader = ConfigUploader(port)
    
    try:
        # Step 1: Connect
        print("[1/4] Connecting...")
        uploader.connect()
        print(f"  ✓ Connected to {uploader.port}")
        
        # Step 2: Upload
        print("")
        print("[2/4] Uploading config.yaml...")
        if not uploader.upload_file(config_file, "/config.yaml"):
            sys.exit(1)
        
        # Step 3: Verify
        print("")
        print("[3/4] Verifying upload...")
        if not uploader.verify_upload("/config.yaml", file_size):
            print("  ⚠ Verification failed, continuing anyway...")
        
        # Step 4: Reload and validate
        print("")
        print("[4/4] Reloading configuration...")
        uploader.reload_config()
        time.sleep(0.5)
        uploader.validate_config()
        
        print("")
        print("╔══════════════════════════════════════════╗")
        print("║  Config Upload Complete!                 ║")
        print("╚══════════════════════════════════════════╝")
        print("")
        
    except serial.SerialException as e:
        print(f"ERROR: Serial communication failed: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)
    finally:
        uploader.disconnect()


if __name__ == "__main__":
    main()
