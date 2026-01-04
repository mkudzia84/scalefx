# Test Scripts

## Flash Upload Tests

### Root Cause: USB/Serial Deadlock ✓ IDENTIFIED

**The Problem:**
Arduino USB CDC (TinyUSB) requires the main loop to return frequently to service USB interrupts and transfer data from hardware buffers. The original upload handler was blocking in a tight loop waiting for serial data while simultaneously performing flash writes, creating a deadlock:

1. Upload handler blocks in: `while (!Serial.available())`
2. Simultaneously calls `flash->flushBuffer()` which blocks 10-100ms
3. USB stack needs main loop to return to transfer data
4. Deadlock: Can't receive data because we're blocking the main loop

**The Solution:**
Made the upload handler fully non-blocking:
- Only read when `Serial.available()` returns true (no blocking wait)
- Call `yield()` in loop to allow USB stack to run
- Periodic flush instead of blocking flush
- Single timeout check instead of nested wait loops

### Tests

1. **Simple test** - `test_flash_simple.ps1`
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tests\test_flash_simple.ps1
   ```
   Tests basic flash init/info/ls commands

2. **Debug upload** - `debug_upload.py`
   ```bash
   python tests\debug_upload.py
   ```
   Sends 5-byte test file with detailed logging

3. **Full upload test** - `test_flash_upload.ps1`
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tests\test_flash_upload.ps1
   ```
   Complete upload test with flow control

### After Power Cycle

Deploy new firmware with fix:
```bash
.\scripts\upload.ps1
```

Then test:
```bash
python tests\debug_upload.py
```
