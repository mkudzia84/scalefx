# Build and Flash Guide

> **ACTION DOCUMENT:** How to compile firmware and deploy to hardware.

---

## Quick Commands

```yaml
# Centralized build and flash (recommended)
Build_And_Flash: "python scripts/build_and_flash.py gunfx"
Flash_Specific_Port: "python scripts/build_and_flash.py lightfx --port COM10"
Skip_Build: "python scripts/build_and_flash.py noop --no-build"
Incremental_Build: "python scripts/build_and_flash.py gunfx --no-clean"
Skip_Verify: "python scripts/build_and_flash.py lightfx --skip-verify"

# PlatformIO direct commands
GunFX:
  build: "python -m platformio run -e pico -d controllers/gunfx/pico"
  clean: "python -m platformio run -t clean -d controllers/gunfx/pico"

LightFX:
  build: "python -m platformio run -e pico -d controllers/lightfx/pico"
  clean: "python -m platformio run -t clean -d controllers/lightfx/pico"

NoOp:
  build: "python -m platformio run -e pico -d controllers/noop/pico"
  clean: "python -m platformio run -t clean -d controllers/noop/pico"
```

---

## Prerequisites

```yaml
Required_Tools:
  PlatformIO:
    install: "pip install platformio"
    verify: "pio --version"
  
  Python_Packages:
    install: "pip install pyserial"
    verify: "python -c 'import serial'"
```

---

## Build Process

### Using Centralized Script (Recommended)

```bash
# Build and flash a controller
python scripts/build_and_flash.py gunfx
python scripts/build_and_flash.py lightfx --port COM10
python scripts/build_and_flash.py noop --no-build
```

**Options:**
```yaml
controller: "gunfx | lightfx | noop (required)"
--port PORT: "Specify serial port (default: auto-detect)"
--no-build: "Skip build step (use existing firmware)"
--no-clean: "Skip clean step (incremental build)"
--skip-verify: "Skip post-flash verification"
--timeout SEC: "BOOTSEL wait timeout (default: 15s)"
```

### Using PlatformIO Directly

```bash
python -m platformio run -e pico -d controllers/gunfx/pico
```

**Output:** `controllers/gunfx/pico/.pio/build/pico/firmware.uf2`

### Clean Build

```bash
python -m platformio run -t clean -d controllers/gunfx/pico
python -m platformio run -e pico -d controllers/gunfx/pico
```

---

## Flash Methods

### Method 1: build_and_flash.py (Recommended)

The centralized script at `scripts/build_and_flash.py` handles the complete build-and-flash workflow using the binary COBS protocol.

```bash
# Build and flash GunFX
python scripts/build_and_flash.py gunfx

# Flash on specific port
python scripts/build_and_flash.py gunfx --port COM10

# Flash without rebuilding
python scripts/build_and_flash.py lightfx --no-build
```

**Process:**
```
1. Build firmware via PlatformIO (unless --no-build)
2. Increment BUILD_NUMBER in source (.ino file)
3. Detect serial port (auto-detect or --port)
4. Send binary INIT packet (COBS encoded)
5. Receive INIT_READY, parse binary payload (name, version, platform, build)
6. Send binary BOOTSEL command
7. Wait for RPI-RP2 drive to appear
8. Copy firmware.uf2 to drive
9. Wait for device reboot
10. Verify device responds with correct INIT_READY (unless --skip-verify)
```

> **Note:** The script uses binary COBS protocol packets from the test framework (`build_packet(CorePacket.INIT)`, `build_packet(CorePacket.BOOTSEL)`). There is no text-mode INIT.

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
# Only works when device is already in BOOTSEL mode
python -m platformio run -t upload -d controllers/gunfx/pico
```

---

## build_and_flash.py Reference

### Script Location

```
scripts/build_and_flash.py    # Single centralized script for all controllers
```

### Supported Controllers

```yaml
gunfx: "controllers/gunfx/pico/"
lightfx: "controllers/lightfx/pico/"
gearcontrol: "controllers/gearcontrol/pico/"
hubfx: "controllers/hubfx/esp32s3/"         # ESP32-S3, uses esptool for upload
noop: "controllers/noop/pico/"
```

### Expected Output

```
╔══════════════════════════════════════════╗
║        ScaleFX Build & Flash             ║
║        Controller: gunfx                 ║
╚══════════════════════════════════════════╝

▶ Building firmware...
  ✓ Build successful (217,600 bytes)

▶ Detecting serial port...
  ✓ Found: COM5 (Raspberry Pi Pico)

▶ Connecting to device...
  ✓ Connected to GunFX v0.3.0

▶ Entering BOOTSEL mode...
  ✓ BOOTSEL command sent
  ✓ RPI-RP2 drive detected: G:

▶ Flashing firmware...
  ✓ Copied firmware.uf2 (MD5 verified)

▶ Verifying...
  ✓ Device rebooted successfully

══════════════════════════════════════════
  Flash Complete!
══════════════════════════════════════════
```

---

## Script Location

```yaml
Centralized_Script:
  location: "scripts/build_and_flash.py"
  usage: "python scripts/build_and_flash.py <controller> [options]"
  protocol: "Binary COBS (uses test framework's build_packet())"
  note: "Single script handles all Pico controllers"
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
      - "Device not responding to BOOTSEL command (old firmware)"
      - "USB cable is charge-only (no data)"
      - "USB port issue"
    fixes:
      - "Try manual BOOTSEL method (hold button, plug in, release)"
      - "Use different USB cable"
      - "Try different USB port"
  
  "BOOTSEL command not working":
    cause: "Device firmware doesn't support binary BOOTSEL command"
    fix: "Manual BOOTSEL: hold button while plugging in USB, then release"
  
  "Port not found":
    fixes:
      - "Specify port: --port COM10"
      - "Check device is connected and recognized by OS"

Post_Flash_Errors:
  "Device not responding after flash":
    cause: "Firmware crash on startup"
    fix: "Flash known-good firmware using manual BOOTSEL"

  "Verification timeout (flash succeeded but script reports failure)":
    cause: "Device rebooted but COM port reappeared slower than the script timeout"
    fix: |
      This is a non-critical error — the UF2 was copied successfully.
      Run 'python scripts/build_and_flash.py <controller> --no-build' to re-verify.
      Or connect via CLI and run 'init' to confirm the build number.
    note: "The script auto-increments BUILD_NUMBER on each build. If a verification
      timeout occurs, the firmware IS flashed — just the post-check failed."

  "BUILD_NUMBER auto-incremented after failed flash attempt":
    cause: "build_and_flash.py increments BUILD_NUMBER before flashing"
    impact: "Non-critical — the next successful flash will carry the incremented number"
    note: "Track build numbers by checking the device's INIT_READY response, not the
      source code define alone"

AI_Agent_Troubleshooting:
  "CLI commands sent to wrong terminal":
    cause: "VS Code has multiple terminals open; run_in_terminal goes to the active one"
    fix: |
      Always quit the interactive CLI session before running shell commands.
      The CLI session captures stdin — shell commands typed there fail with
      'Unknown command'. Send 'quit' first, then run the shell command.
    best_practice: "Use separate terminals for CLI sessions and build commands"

  "python -c with multiline code fails in PowerShell":
    cause: "PowerShell treats newlines in -c argument as ScriptBlock, not Python code"
    fix: |
      Create a temporary .py file instead, or use single-line python -c commands.
      For complex verification, use the interactive CLI or write a test script.
    best_practice: "For multi-line Python in PowerShell, write a .py file"
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
