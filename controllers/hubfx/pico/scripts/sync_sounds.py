#!/usr/bin/env python3
"""
Sync sound files from local media/sounds folder to HubFX Pico SD card.

Features:
- Compares local files with SD card contents
- Uploads new or modified files
- Optionally removes files not in source
- Shows progress and statistics

Usage:
    python sync_sounds.py [source_folder] [port]
    python sync_sounds.py                           # Uses ../../media/sounds, auto-detect
    python sync_sounds.py ./sounds COM10
    python sync_sounds.py ./sounds COM10 --delete   # Remove orphaned files on SD
"""

import serial
import serial.tools.list_ports
import time
import sys
import os
import json
import hashlib
import argparse
from pathlib import Path


class SoundSyncer:
    def __init__(self, port=None, baudrate=115200):
        self.port = port or self._find_pico_port()
        self.baudrate = baudrate
        self.ser = None
        self.stats = {
            "uploaded": 0,
            "skipped": 0,
            "deleted": 0,
            "errors": 0,
            "bytes_transferred": 0
        }
    
    def _find_pico_port(self):
        """Auto-detect Pico COM port."""
        ports = serial.tools.list_ports.comports()
        for p in ports:
            if "2E8A" in (p.vid and hex(p.vid) or ""):
                return p.device
            if "Pico" in (p.description or ""):
                return p.device
        return "COM10"
    
    def connect(self):
        """Establish serial connection."""
        print(f"  Connecting to {self.port}...")
        self.ser = serial.Serial(self.port, self.baudrate, timeout=3)
        time.sleep(1)
        self.ser.reset_input_buffer()
        print(f"  [OK] Connected")
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
        
        try:
            start = response.find('{')
            if start >= 0:
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
    
    def init_sd(self):
        """Initialize SD card."""
        response = self.send_command("sd init")
        if "error" in response.lower() and "already" not in response.lower():
            print(f"  [FAIL] SD init failed: {response}")
            return False
        return True
    
    def list_remote_files(self, path="/sounds"):
        """Get list of files on SD card with sizes."""
        files = {}
        
        # Try JSON mode first
        response = self.send_json_command(f"sd ls {path}", delay=1.0)
        
        if response and response.get("status") == "ok":
            for entry in response.get("entries", []):
                name = entry.get("name", "")
                if entry.get("type") == "file":
                    files[name] = entry.get("size", 0)
                elif entry.get("type") == "dir":
                    # Recurse into subdirectory
                    subpath = f"{path}/{name}".replace("//", "/")
                    subfiles = self.list_remote_files(subpath)
                    for subname, size in subfiles.items():
                        files[f"{name}/{subname}"] = size
        else:
            # Fall back to text mode
            text_response = self.send_command(f"sd ls {path}", delay=1.0)
            for line in text_response.split('\n'):
                line = line.strip()
                # Parse lines like "file.wav  12345" or "  dir/"
                if line and not line.startswith('>'):
                    parts = line.split()
                    if len(parts) >= 2 and not parts[0].endswith('/'):
                        try:
                            files[parts[0]] = int(parts[1])
                        except ValueError:
                            pass
                    elif len(parts) >= 1 and parts[0].endswith('/'):
                        # Directory - recurse
                        dirname = parts[0].rstrip('/')
                        subpath = f"{path}/{dirname}".replace("//", "/")
                        subfiles = self.list_remote_files(subpath)
                        for subname, size in subfiles.items():
                            files[f"{dirname}/{subname}"] = size
        
        return files
    
    def create_remote_dir(self, path):
        """Create directory on SD card."""
        response = self.send_command(f"sd mkdir {path}")
        return "error" not in response.lower() or "exists" in response.lower()
    
    def delete_remote_file(self, path):
        """Delete file on SD card."""
        response = self.send_command(f"sd rm {path}")
        return "error" not in response.lower()
    
    def upload_file(self, local_path, remote_path, show_progress=True):
        """Upload file to SD card."""
        if not os.path.exists(local_path):
            return False
        
        file_size = os.path.getsize(local_path)
        
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
                    return False
        
        if not ready:
            return False
        
        # Upload file data
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
                    self.stats["bytes_transferred"] += file_size
                    return True
                elif "ERROR" in line:
                    return False
        
        return False
    
    def get_local_files(self, source_folder):
        """Get list of local sound files with sizes and original paths."""
        files = {}  # lowercase_path -> (original_path, size)
        source_path = Path(source_folder)
        
        for file_path in source_path.rglob("*"):
            if file_path.is_file():
                # Get relative path
                rel_path = str(file_path.relative_to(source_path)).replace("\\", "/")
                # Use lowercase key for FAT32 case-insensitive comparison
                files[rel_path.lower()] = (rel_path, file_path.stat().st_size)
        
        return files
    
    def sync(self, source_folder, dest_folder="/sounds", delete_orphans=False):
        """Sync local folder to SD card."""
        print("")
        print("[1/4] Scanning local files...")
        local_files = self.get_local_files(source_folder)  # lowercase -> (original, size)
        print(f"  Found {len(local_files)} local files")
        
        # Calculate total size
        total_size = sum(size for _, size in local_files.values())
        print(f"  Total size: {total_size:,} bytes ({total_size/1024/1024:.1f} MB)")
        
        print("")
        print("[2/4] Scanning SD card...")
        if not self.init_sd():
            return False
        
        remote_files = self.list_remote_files(dest_folder)
        # Normalize remote file keys to lowercase for FAT32 comparison
        remote_files_lower = {k.lower(): v for k, v in remote_files.items()}
        print(f"  Found {len(remote_files)} remote files")
        
        print("")
        print("[3/4] Comparing files...")
        
        # Determine what needs to be uploaded (case-insensitive comparison for FAT32)
        to_upload = []
        for name_lower, (original_name, size) in local_files.items():
            remote_size = remote_files_lower.get(name_lower)  # Case-insensitive lookup
            if remote_size is None:
                to_upload.append((original_name, size, "new"))
            elif remote_size != size:
                to_upload.append((original_name, size, "modified"))
            else:
                self.stats["skipped"] += 1
        
        # Determine what needs to be deleted (case-insensitive)
        to_delete = []
        if delete_orphans:
            for name in remote_files:
                if name.lower() not in local_files:
                    to_delete.append(name)
        
        print(f"  To upload: {len(to_upload)} files")
        print(f"  To skip: {self.stats['skipped']} files (unchanged)")
        if delete_orphans:
            print(f"  To delete: {len(to_delete)} files")
        
        if not to_upload and not to_delete:
            print("")
            print("  [OK] Already in sync!")
            return True
        
        print("")
        print("[4/4] Syncing...")
        
        # Create necessary directories
        dirs_created = set()
        for name, size, reason in to_upload:
            dir_path = os.path.dirname(name)
            if dir_path and dir_path not in dirs_created:
                remote_dir = f"{dest_folder}/{dir_path}".replace("//", "/")
                self.create_remote_dir(remote_dir)
                dirs_created.add(dir_path)
        
        # Upload files
        for i, (name, size, reason) in enumerate(to_upload, 1):
            local_path = os.path.join(source_folder, name)
            remote_path = f"{dest_folder}/{name}".replace("//", "/")
            
            size_str = f"{size:,}" if size < 1024*1024 else f"{size/1024/1024:.1f}MB"
            print(f"  [{i}/{len(to_upload)}] {name} ({size_str}) [{reason}]...", end=" ", flush=True)
            
            if self.upload_file(local_path, remote_path, show_progress=False):
                print("OK")
                self.stats["uploaded"] += 1
            else:
                print("FAIL")
                self.stats["errors"] += 1
        
        # Delete orphaned files
        for name in to_delete:
            remote_path = f"{dest_folder}/{name}".replace("//", "/")
            print(f"  Deleting {name}...", end=" ", flush=True)
            
            if self.delete_remote_file(remote_path):
                print("OK")
                self.stats["deleted"] += 1
            else:
                print("FAIL")
                self.stats["errors"] += 1
        
        return True


def main():
    parser = argparse.ArgumentParser(description="Sync sound files to HubFX Pico SD card")
    parser.add_argument("source", nargs="?", help="Source folder (default: ../../media/sounds)")
    parser.add_argument("port", nargs="?", help="COM port (default: auto-detect)")
    parser.add_argument("--delete", action="store_true", help="Delete files on SD not in source")
    parser.add_argument("--dest", default="/sounds", help="Destination folder on SD (default: /sounds)")
    
    args = parser.parse_args()
    
    # Determine source folder
    if args.source:
        source_folder = args.source
    else:
        # Default to media/sounds relative to project root
        script_dir = Path(__file__).parent
        source_folder = script_dir.parent.parent.parent.parent / "media" / "sounds"
        if not source_folder.exists():
            # Try relative to current directory
            source_folder = Path("media/sounds")
    
    source_folder = Path(source_folder).resolve()
    
    print("")
    print("============================================")
    print("  HubFX Pico - Sound Sync Utility")
    print("============================================")
    print("")
    
    if not source_folder.exists():
        print(f"ERROR: Source folder not found: {source_folder}")
        print("")
        print("Usage: python sync_sounds.py [source_folder] [port]")
        sys.exit(1)
    
    print(f"Source: {source_folder}")
    print(f"Destination: SD:{args.dest}")
    if args.delete:
        print("Mode: Sync with delete (removes orphaned files)")
    else:
        print("Mode: Upload only (preserves extra files on SD)")
    
    syncer = SoundSyncer(args.port)
    
    try:
        print("")
        print("[0/4] Connecting...")
        syncer.connect()
        
        syncer.sync(str(source_folder), args.dest, delete_orphans=args.delete)
        
        # Print summary
        print("")
        print("=" * 44)
        print("  Summary:")
        print(f"    Uploaded: {syncer.stats['uploaded']} files")
        print(f"    Skipped:  {syncer.stats['skipped']} files (unchanged)")
        if args.delete:
            print(f"    Deleted:  {syncer.stats['deleted']} files")
        if syncer.stats['errors'] > 0:
            print(f"    Errors:   {syncer.stats['errors']}")
        
        bytes_tx = syncer.stats['bytes_transferred']
        if bytes_tx > 0:
            print(f"    Transferred: {bytes_tx:,} bytes ({bytes_tx/1024/1024:.1f} MB)")
        
        print("")
        if syncer.stats['errors'] == 0:
            print("============================================")
            print("  Sync Complete!")
            print("============================================")
        else:
            print("============================================")
            print("  Sync Complete (with errors)")
            print("============================================")
        print("")
        
    except serial.SerialException as e:
        print(f"ERROR: Serial communication failed: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n\nAborted by user.")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
    finally:
        syncer.disconnect()


if __name__ == "__main__":
    main()
