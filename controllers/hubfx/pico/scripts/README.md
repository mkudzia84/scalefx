# HubFX Pico - Build and Flash Scripts

Utilities for building, flashing, and uploading firmware to Raspberry Pi Pico.

## Available Scripts

### Build and Flash

#### `build_and_flash.ps1` (Recommended)
Complete automated build and flash workflow with verification:
```powershell
.\scripts\build_and_flash.ps1
```

**Features:**
- Clean build environment
- Compile firmware with error checking
- Calculate MD5 checksum
- Enter BOOTSEL mode automatically
- Flash firmware to Pico
- Verify upload integrity
- Optional: Run tests after flash

**Usage:**
```powershell
# Normal build and flash
.\scripts\build_and_flash.ps1

# Skip post-flash tests
.\scripts\build_and_flash.ps1 -SkipTests
```

#### `upload.ps1`
Quick upload of pre-built firmware:
```powershell
.\scripts\upload.ps1
```
Assumes firmware is already built in `.pio/build/pico/firmware.uf2`.

### File Transfer Utilities

#### `upload_file.py`
Upload individual files to Pico flash filesystem:
```bash
python scripts/upload_file.py <local_file> <remote_path>
```

**Example:**
```bash
python scripts/upload_file.py config.yaml /config.yaml
python scripts/upload_file.py sounds/startup.wav /sounds/startup.wav
```

**Note:** Requires `flash init` to be run on device first.

#### `download_file.py`
Download files from Pico flash filesystem:
```bash
python scripts/download_file.py <remote_path> <local_file>
```

**Example:**
```bash
python scripts/download_file.py /config.yaml config_backup.yaml
python scripts/download_file.py /logs/boot.log boot_log.txt
```

## Prerequisites

### PowerShell Scripts
- Windows PowerShell 5.1+ or PowerShell Core 7+
- Python 3.7+ with `platformio` installed
- `pyserial` package: `pip install pyserial`

### Python Scripts
- Python 3.7+
- `pyserial` package: `pip install pyserial`

## Build Process Details

The automated build process (`build_and_flash.ps1`) performs:

1. **Clean** - Remove old build artifacts
2. **Build** - Compile firmware with PlatformIO
3. **Checksum** - Calculate MD5 hash of firmware
4. **BOOTSEL** - Enter bootloader mode via serial command
5. **Flash** - Copy firmware to BOOTSEL drive (G:)
6. **Verify** - Confirm firmware size and checksum
7. **Test** (optional) - Run automated tests

## Troubleshooting

### BOOTSEL Mode Issues

**Problem:** Device doesn't enter BOOTSEL mode
- **Solution 1:** Manually enter BOOTSEL:
  1. Disconnect USB
  2. Hold BOOTSEL button
  3. Connect USB while holding
  4. Release button

- **Solution 2:** Use `reset` command in serial monitor:
  ```
  > reset
  ```

### COM Port Not Found

**Problem:** Script can't find COM10
- Check Device Manager for actual COM port
- Edit script to use correct port:
  ```powershell
  $port = New-Object System.IO.Ports.SerialPort "COM5", 115200
  ```

### Upload Verification Failed

**Problem:** Checksum mismatch after upload
- Try slower USB port (USB 2.0 instead of 3.0)
- Use different USB cable
- Manually copy firmware to BOOTSEL drive

### Flash Filesystem Full

**Problem:** `upload_file.py` fails with "No space"
- Check available space: `flash info`
- Remove unused files: `flash rm <filename>`
- Increase flash partition size in code (requires reflash)

## Advanced Usage

### Custom Build Flags

Edit `platformio.ini` to customize build:
```ini
build_flags = 
    -DAUDIO_SAMPLE_RATE=48000          ; Use 48kHz instead of 44.1kHz
    -DWM8960_I2C_SPEED=100000          ; Increase I2C speed (good wiring)
    -DAUDIO_DEBUG_TIMING=1             ; Enable timing diagnostics
```

See [../docs/AUDIO_CONFIGURATION.md](../docs/AUDIO_CONFIGURATION.md) for all configuration options.

### Manual Build Commands

```bash
# Clean build
python -m platformio run --target clean

# Build only
python -m platformio run

# Build and upload via USB (no BOOTSEL)
python -m platformio run -t upload

# Build for specific environment
python -m platformio run -e pico

# Verbose output
python -m platformio run -v
```

## See Also

- [../tests/README.md](../tests/README.md) - Test scripts
- [../docs/AUDIO_CONFIGURATION.md](../docs/AUDIO_CONFIGURATION.md) - Configuration guide
- [../platformio.ini](../platformio.ini) - Build configuration
