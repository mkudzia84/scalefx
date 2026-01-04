#!/usr/bin/env python3
"""
Test script to verify binary file upload/download integrity.
Creates a test file, uploads it, downloads it, and compares checksums.
"""

import os
import hashlib
import sys

def main():
    os.chdir(r'e:\data\OneDrive\CAD\CODE\scalefx\controllers\hubfx\pico')
    
    print("=== Binary File Integrity Test ===\n")
    
    # Check if files exist
    if not os.path.exists('test_20mb.bin'):
        print("ERROR: test_20mb.bin not found")
        return 1
    
    if not os.path.exists('downloaded_20mb.bin'):
        print("ERROR: downloaded_20mb.bin not found")
        return 1
    
    # Get file sizes
    orig_size = os.path.getsize('test_20mb.bin')
    down_size = os.path.getsize('downloaded_20mb.bin')
    
    print(f"Original file:    {orig_size:,} bytes")
    print(f"Downloaded file:  {down_size:,} bytes")
    print(f"Size match:       {orig_size == down_size}")
    print()
    
    # Calculate MD5 checksums
    print("Calculating checksums...")
    
    with open('test_20mb.bin', 'rb') as f:
        orig_md5 = hashlib.md5(f.read()).hexdigest()
    
    with open('downloaded_20mb.bin', 'rb') as f:
        down_md5 = hashlib.md5(f.read()).hexdigest()
    
    print(f"Original MD5:     {orig_md5}")
    print(f"Downloaded MD5:   {down_md5}")
    print(f"Checksum match:   {orig_md5 == down_md5}")
    print()
    
    # Byte-by-byte comparison
    print("Performing byte-by-byte comparison...")
    with open('test_20mb.bin', 'rb') as f1, open('downloaded_20mb.bin', 'rb') as f2:
        bytes_compared = 0
        chunk_size = 65536
        match = True
        
        while True:
            chunk1 = f1.read(chunk_size)
            chunk2 = f2.read(chunk_size)
            
            if not chunk1 and not chunk2:
                break
            
            if chunk1 != chunk2:
                match = False
                print(f"MISMATCH at byte {bytes_compared}")
                break
            
            bytes_compared += len(chunk1)
            
            if bytes_compared % (1024 * 1024) == 0:
                print(f"  Verified: {bytes_compared // (1024 * 1024)} MB")
    
    print(f"Total bytes compared: {bytes_compared:,}")
    print(f"Files identical:      {match}")
    print()
    
    if match and orig_md5 == down_md5 and orig_size == down_size:
        print("✓ SUCCESS: Binary file transfer integrity verified!")
        return 0
    else:
        print("✗ FAILURE: Files do not match!")
        return 1

if __name__ == "__main__":
    sys.exit(main())
