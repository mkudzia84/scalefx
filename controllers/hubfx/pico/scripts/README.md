# HubFX Pico - Build and Deploy Scripts

Utilities for building firmware and managing files on the Pico.

## Quick Reference

```bash
# Build and flash firmware
.\scripts\build_and_flash.ps1

# Upload config with verification
python scripts/upload_config.py

# Sync sound files to SD card
python scripts/sync_sounds.py
```

## Available Scripts

### `build_and_flash.ps1` - Build and Flash Firmware

Complete automated build and flash workflow:

```powershell
# Full build and flash
.\scripts\build_and_flash.ps1

# Flash only (skip build)
.\scripts\build_and_flash.ps1 -NoBuild

# Skip post-flash verification
.\scripts\build_and_flash.ps1 -SkipVerify

# Custom COM port
.\scripts\build_and_flash.ps1 -ComPort COM5
```

**Features:**
- Builds firmware with PlatformIO
- Calculates MD5 checksum
- Triggers BOOTSEL mode via serial (`sys bootsel`)
- Waits for RPI-RP2 drive
- Copies firmware and verifies device reboot
- Displays firmware version after flash

### `upload_config.py` - Upload Configuration

Upload and verify config.yaml:

```bash
# Upload ./config.yaml (auto-detect port)
python scripts/upload_config.py

# Upload specific file
python scripts/upload_config.py myconfig.yaml

# Specify COM port
python scripts/upload_config.py config.yaml COM5
```

**Features:**
- Uploads config.yaml to SD card
- Verifies file size after upload
- Triggers `config reload`
- Validates configuration structure
- Shows config summary (engine/gun settings)

### `sync_sounds.py` - Sync Sound Files

Synchronize local sound files with SD card:

```bash
# Sync from default media/sounds folder
python scripts/sync_sounds.py

# Sync from specific folder
python scripts/sync_sounds.py ./my_sounds

# Sync with delete (remove orphaned files on SD)
python scripts/sync_sounds.py --delete

# Specify COM port
python scripts/sync_sounds.py ./sounds COM5
```

**Features:**
- Compares local files with SD card contents
- Only uploads new or modified files (by size)
- Creates directories automatically
- Optional `--delete` to remove files not in source
- Shows detailed progress and statistics

## Prerequisites

### PowerShell Scripts
- Windows PowerShell 5.1+ or PowerShell Core 7+
- Python 3.7+ with `platformio` installed

### Python Scripts
- Python 3.7+
- `pyserial` package: `pip install pyserial`

## Workflow Examples

### First-Time Setup

```powershell
# 1. Build and flash firmware
.\scripts\build_and_flash.ps1

# 2. Upload configuration
python scripts/upload_config.py config.yaml

# 3. Sync sound files
python scripts/sync_sounds.py
```

### After Code Changes

```powershell
# Just rebuild and flash
.\scripts\build_and_flash.ps1
```

### After Config Changes

```bash
# Upload and reload
python scripts/upload_config.py
```

### After Adding Sound Files

```bash
# Sync new files
python scripts/sync_sounds.py
```

## Troubleshooting

### BOOTSEL Mode Issues

**Problem:** Device doesn't enter BOOTSEL mode

- **Solution 1:** Manual BOOTSEL:
  1. Disconnect USB
  2. Hold BOOTSEL button
  3. Connect USB while holding
  4. Release button

- **Solution 2:** Check if `sys bootsel` command works:
  ```
  > sys bootsel
  ```

### COM Port Not Found

**Problem:** Script can't find COM port

- Check Device Manager for actual port
- Set port explicitly: `-ComPort COM5` or second argument
- Environment variable: `set PICO_PORT=COM5`

### Upload Failures

**Problem:** File upload times out

- Ensure SD card is initialized: `sd init`
- Check available space: `sd info`
- Try smaller files first to test connection

### Sync Shows All Files as "new"

**Problem:** Every file uploads even when unchanged

- File comparison uses size only (not checksum)
- SD card may have different files
- Run with `--delete` to remove orphaned files

## File Locations

| Script | Purpose |
|--------|---------|
| `build_and_flash.ps1` | Build firmware, flash via BOOTSEL |
| `upload_config.py` | Upload config.yaml with verification |
| `sync_sounds.py` | Sync sound files from media/sounds |

## See Also

- [../tests/README.md](../tests/README.md) - Automated tests
- [../docs/AUDIO_CONFIGURATION.md](../docs/AUDIO_CONFIGURATION.md) - Audio setup
- [../platformio.ini](../platformio.ini) - Build configuration
