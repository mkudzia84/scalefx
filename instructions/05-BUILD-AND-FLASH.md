# Build and Flash Guide

> **ACTION DOCUMENT:** How to compile firmware and deploy to hardware.

---

## Quick Commands

```yaml
GunFX:
  build: "cd controllers/gunfx/pico && pio run"
  flash: "python scripts/build_and_flash.py"
  clean: "pio run -t clean"

LightFX:
  build: "cd controllers/lightfx/pico && pio run"
  flash: "python scripts/build_and_flash.py"
  clean: "pio run -t clean"

NoOp:
  build: "cd controllers/noop/pico && pio run"
  flash: "Manual BOOTSEL required"
  clean: "pio run -t clean"
```

---

## Prerequisites

```yaml
Required_Tools:
  PlatformIO:
    install: "pip install platformio"
    verify: "pio --version"
  
  Python_Packages:
    install: "pip install pyserial colorama"
    verify: "python -c 'import serial; import colorama'"
```

---

## Build Process

### Standard Build

```bash
cd controllers/gunfx/pico
pio run
```

**Output:** `.pio/build/pico/firmware.uf2`

### Clean Build

```bash
pio run -t clean
pio run
```

### Build All Controllers

```bash
# PowerShell
@("gunfx", "lightfx", "noop") | ForEach-Object {
    Write-Host "Building $_..." -ForegroundColor Cyan
    Set-Location "controllers/$_/pico"
    pio run
    Set-Location ../../..
}
```

---

## Flash Methods

### Method 1: build_and_flash.py (Recommended)

```bash
cd controllers/gunfx/pico
python scripts/build_and_flash.py
```

**Process:**
```
1. Increment build number in source
2. Clean build
3. Compile firmware
4. Auto-detect serial port
5. Send BOOTSEL command
6. Wait for RPI-RP2 drive
7. Copy firmware.uf2
8. Verify new version
```

**Options:**
```yaml
--no-build: "Skip compilation, use existing .uf2"
--no-clean: "Incremental build (faster)"
--skip-verify: "Skip post-flash verification"
--port COM5: "Specify serial port"
--timeout 30: "BOOTSEL wait timeout (seconds)"
```

### Method 2: Manual BOOTSEL

```yaml
Steps:
  1: "Disconnect Pico from USB"
  2: "Hold BOOTSEL button"
  3: "Connect USB while holding BOOTSEL"
  4: "Release BOOTSEL after connecting"
  5: "RPI-RP2 drive appears"
  6: "Copy .pio/build/pico/firmware.uf2 to drive"
  7: "Device auto-reboots"
```

### Method 3: PlatformIO Upload

```bash
# Only works when device is in BOOTSEL mode
pio run -t upload
```

---

## build_and_flash.py Reference

### Script Location

```
controllers/{name}/pico/scripts/build_and_flash.py
```

### Expected Output

```
============================================
  GunFX Pico - Build and Flash
============================================

[1/6] Incrementing build number...
    [OK] Build number: 42 → 43

[2/6] Building firmware...
    [OK] Firmware: 94352 bytes
    [OK] Flash: 9.0%, RAM: 8.2%

[3/6] Detecting serial port...
    [OK] Found: COM5 (Raspberry Pi Pico)

[4/6] Sending BOOTSEL command...
    [OK] Connected to GunFX-A4B2 v0.3.0
    [OK] BOOTSEL command sent

[5/6] Copying firmware...
    [OK] RPI-RP2 drive detected: E:
    [OK] Firmware copied

[6/6] Verifying...
    [OK] Device rebooted
    [OK] Version: 0.3.0 (build 43)

============================================
  Flash Complete!
============================================
```

---

## Creating build_and_flash.py for New Controller

### Copy Template

```bash
cp controllers/gunfx/pico/scripts/build_and_flash.py \
   controllers/newfx/pico/scripts/
```

### Modifications Required

```yaml
1_Source_File_Path:
  location: "extract_version_from_source()"
  change: 'source_file = project_dir / "src" / "newfx_pico.ino"'

2_Device_Name:
  location: "verify_device()"
  change: 'if "NewFX" not in init_ready_response:'

3_Banner:
  location: "print_banner()"
  change: '"NewFX Pico - Build and Flash"'
```

---

## Troubleshooting

```yaml
Build_Errors:
  "platform-raspberrypi not found":
    fix: "pio pkg install -g -p 'https://github.com/maxgerhardt/platform-raspberrypi.git'"
  
  "Library not found":
    fix: "Check lib_deps paths in platformio.ini are relative to project"
  
  "Compilation error":
    fix: "Run 'pio run -v' for verbose output"

Flash_Errors:
  "RPI-RP2 drive not appearing":
    causes:
      - "Device not in BOOTSEL mode"
      - "USB cable is charge-only (no data)"
      - "USB port issue"
    fixes:
      - "Try manual BOOTSEL method"
      - "Use different USB cable"
      - "Try different USB port"
  
  "BOOTSEL command not working":
    cause: "Device not responding to serial"
    fix: "Manual BOOTSEL: hold button, plug in, release"
  
  "Port not found":
    fix: "Specify port explicitly: --port COM5"

Post_Flash_Errors:
  "Device not responding after flash":
    cause: "Firmware crash on startup"
    fix: "Flash known-good firmware using manual BOOTSEL"
  
  "Version mismatch":
    cause: "Old firmware cached"
    fix: "Clean build: pio run -t clean && pio run"
```

---

## Remote Build (HubFX Pi)

```yaml
Tasks_Available:
  - label: "Build HubFX Pi on Raspberry Pi (Remote SSH)"
    command: "ssh helifx@helifx 'cd /home/helifx/helifx/controllers/hubfx/pi && make'"
  
  - label: "Clean HubFX Pi on Raspberry Pi (Remote SSH)"
    command: "ssh helifx@helifx 'cd /home/helifx/helifx/controllers/hubfx/pi && make clean'"
  
  - label: "Sync and Build HubFX Pi on Raspberry Pi"
    command: |
      scp -r ${workspaceFolder}/controllers/hubfx/pi/* helifx@helifx:/home/helifx/helifx/controllers/hubfx/pi/
      ssh helifx@helifx 'cd /home/helifx/helifx/controllers/hubfx/pi && make'

Usage:
  vscode: "Ctrl+Shift+B → Select task"
  cli: "Use run_task tool with task ID"
```

---

## Verification After Flash

```bash
# Quick verification via CLI
python -m tests.cli.interactive

# Commands to verify:
> connect
> init
> status
```

**Expected:** Device responds with INIT_READY containing correct version string.
