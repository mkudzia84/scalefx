# Build and Flash Guide

> **ACTION DOCUMENT:** How to compile firmware and deploy to hardware.

---

## Quick Commands

```yaml
# Flash CLI (recommended). Active controllers: hubfx (ESP32-S3 master),
# lightfx + gearcontrol (Pico generic expanders). The standalone gunfx Pico
# controller was removed 2026-06-06 — those effects live on the HubFX now.
Flash_CLI_Build: "scalefx-flash build hubfx --no-clean"
Flash_CLI_Flash: "scalefx-flash flash hubfx --port COM15"
Flash_CLI_Release: "scalefx-flash release-flash hubfx --port COM15"
Flash_CLI_Tools: "scalefx-flash tools status"
Flash_CLI_Download: "scalefx-flash tools download"

# Build every active controller (VS Code task: "Build All Controllers")
Build_All: "scalefx-flash build gearcontrol --no-clean && scalefx-flash build hubfx --no-clean && ..."
```

---

## Prerequisites

### All Targets

```yaml
Required_Tools:
  Go:
    install: "winget install GoLang.Go"
    verify: "go version"
    min_version: "1.23+"
    note: "Required for Go CLI, Flash CLI, and ScaleFX Studio"
    path: "Ensure GOPATH/bin is on PATH (default: ~/go/bin)"

  Python:
    install: "winget install astral-sh.uv && uv python install 3.12"
    verify: "python --version"
    min_version: "3.10+"
    note: "Required by PlatformIO. uv-managed Python recommended."
    path: "Add uv python dir to PATH (e.g. ~/.local/bin/uv/python/cpython-3.12-...)"

  PlatformIO:
    install: "uv tool install platformio"
    verify: "pio --version"
    min_version: "6.1+"
    note: "Required for building firmware (all platforms). 'pio' CLI must be on PATH."
    alt_install: "pip install platformio"
```

### Firmware Only

```yaml
  esptool_Standalone:
    install: "scalefx-flash tools download"
    verify: "scalefx-flash tools status"
    note: "Required for ESP32-S3 flashing (auto-downloaded on first use)"
```

### ScaleFX Studio (GUI) Only

```yaml
  Wails_v2:
    install: "go install github.com/wailsapp/wails/v2/cmd/wails@latest"
    verify: "wails version"
    min_version: "v2.9+"
    note: "Wails CLI for building/running the Studio GUI. Installs to GOPATH/bin."

  Node_js:
    install: "winget install OpenJS.NodeJS.LTS"
    verify: "node --version && npm --version"
    min_version: "Node 18+ / npm 9+"
    note: "Required for Svelte frontend (npm install, vite dev server, production build)"

  PowerShell_Execution_Policy:
    check: "Get-ExecutionPolicy -Scope CurrentUser"
    fix: "Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned -Force"
    note: "npm.ps1 requires RemoteSigned policy. Only needed on fresh Windows installs."
```

### Windows-Specific Notes

```yaml
PATH_Refresh:
  issue: "New terminal sessions may not see newly installed tools (Go, Node, wails)"
  fix: "Refresh PATH in PowerShell before running commands:"
  command: '$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")'
  alt: "Or restart VS Code / open a new terminal"
```

---

## Build Process

### Using Flash CLI (Recommended)

```bash
# Build and flash a controller
scalefx-flash flash hubfx
scalefx-flash flash lightfx --port COM10
scalefx-flash flash gearcontrol --no-clean
```

**Options:**
```yaml
controller: "lightfx | gearcontrol | hubfx (required)"
--port PORT: "Specify serial port (default: auto-detect)"
--no-build: "Skip build step (use existing firmware)"
--no-clean: "Skip clean step (incremental build)"
--skip-verify: "Skip post-flash verification"
--no-programs: "Skip the post-flash lightfx program deploy (HubFX only)"
```

**LightFX program deploy (HubFX).** After a successful `flash hubfx` / `upload
hubfx`, the CLI connects over serial and uploads the bundled factory lightfx
programs (`media/presets/lightfx/programs/*.yaml`) to `/lightfx/programs/` on the
device's flash, so a freshly-flashed board comes up with the program catalogue
available. Best-effort — a failure logs a warning, never fails the flash. Pass
`--no-programs` to skip. Deploy them without reflashing with
`scalefx-flash programs hubfx [--port PORT]`.

**Crash coredump (HubFX).** `scalefx-flash coredump hubfx [--port PORT]` pulls
the ESP32 crash coredump from flash and decodes it to a backtrace (esptool read
+ espcoredump + gdb). The firmware ELF must match the flashed build (pull before
reflashing). See [24-COREDUMP-DEBUGGING.md](24-COREDUMP-DEBUGGING.md).

### Using PlatformIO Directly

```bash
pio run -e pico -d controllers/gunfx/pico
```

**Output:** `controllers/gunfx/pico/.pio/build/pico/firmware.uf2`

### Clean Build

```bash
pio run -t clean -d controllers/gunfx/pico
pio run -e pico -d controllers/gunfx/pico
```

---

## Flash Methods

### Method 1: Flash CLI (Recommended)

The Flash CLI (`scalefx-flash`) handles the complete build-and-flash workflow using the binary COBS protocol.

```bash
# Build and flash GunFX
scalefx-flash flash gunfx

# Flash on specific port
scalefx-flash flash gunfx --port COM10

# Flash without rebuilding
scalefx-flash flash lightfx --no-build
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
pio run -t upload -d controllers/gunfx/pico
```

---

## Flash CLI Reference

### Supported Controllers

```yaml
gunfx: "controllers/gunfx/pico/"
lightfx: "controllers/lightfx/pico/"
gearcontrol: "controllers/gearcontrol/pico/"
hubfx: "controllers/hubfx/esp32s3/"         # ESP32-S3, uses standalone esptool
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
Centralized_Tool:
  location: "app/go/scalefx-flash.exe"
  usage: "scalefx-flash build|flash|upload <controller> [options]"
  protocol: "Binary COBS"
  note: "Single tool handles all controllers (Pico + ESP32-S3)"
```

---

## ESP32-S3 Flash: Standalone esptool

The ESP32-S3 flash pipeline uses a standalone esptool binary.
This is the preferred path for the GUI (ScaleFX Studio) and release-flash workflows.

### How It Works

```
Flash request (GUI or CLI)
    │
    ├─ ResolveEsptool()  →  standalone binary found?
    │   ├─ YES → flashWithEsptool()
    │   └─ NO  → error: "run 'tools download'"
    └─ done
```

### Search Order

The esptool resolver checks these locations in order:

| Priority | Location | Use Case |
|----------|----------|----------|
| 1 | `<workspace>/tools/esptool/esptool.exe` | Development (gitignored) |
| 2 | Next to the Go executable | Distribution (bundled) |
| 3 | System `PATH` | System-wide install |

### Installation

```bash
# Via flash CLI (recommended — auto-downloads from GitHub)
scalefx-flash tools download

# Check status
scalefx-flash tools status

# Manual: download from GitHub releases
# https://github.com/espressif/esptool/releases
# Extract esptool.exe to tools/esptool/ or next to the CLI binary
```

### esptool Version

The auto-downloader fetches **esptool v5.2.0** from GitHub Releases.
Asset pattern: `esptool-v5.2.0-{platform}.{zip|tar.gz}`

| Platform | Asset Name | Size |
|----------|-----------|------|
| Windows (amd64) | `esptool-v5.2.0-windows-amd64.zip` | ~58 MB |
| macOS (amd64) | `esptool-v5.2.0-macos-amd64.tar.gz` | ~29 MB |
| macOS (arm64) | `esptool-v5.2.0-macos-arm64.tar.gz` | ~27 MB |
| Linux (amd64) | `esptool-v5.2.0-linux-amd64.tar.gz` | ~17 MB |

The standalone binary is ~12 MB (Windows). It is **gitignored** (`tools/esptool/` in `.gitignore`)
because it's too large for the repository. Users obtain it via `tools download` or manual download.

### Distribution Strategy

For end-user distribution (ScaleFX Studio installer or standalone flash tool):

```yaml
Option_A_Bundle:
  description: "Ship esptool.exe alongside the application binary"
  layout:
    - "scalefx-flash.exe"
    - "esptool.exe"          # Resolver finds it as "colocated"
  pros: "Zero-setup, works offline"
  cons: "Adds ~12 MB to distribution"

Option_B_Auto_Download:
  description: "Download on first use via 'tools download'"
  layout:
    - "scalefx-flash.exe"
  pros: "Smallest distribution, always latest"
  cons: "Requires internet on first ESP32 flash"

Option_C_System_Path:
  description: "User installs esptool globally"
  command: "Download standalone from https://github.com/espressif/esptool/releases"
  pros: "Shared across tools"
  cons: "Manual setup required"
```

**Recommended:** Option A (bundle) for installers, Option B (auto-download) for development.

### Studio GUI Integration

The Studio exposes two Wails bindings for esptool management:

| Binding | Returns | Purpose |
|---------|---------|---------|
| `GetToolsStatus()` | `ToolsStatus` | Check if esptool is available (path, source) |
| `DownloadEsptool()` | — (events) | Download standalone esptool, progress via `firmware:progress` |

The `BuildAndFlash()` and `FlashFromRelease()` bindings automatically use standalone esptool
when available — no GUI changes needed.

### Key Source Files

| File | Purpose |
|------|---------|
| `app/go/firmware/esptool.go` | Resolver + auto-download (search, extract, platform detection) |
| `app/go/firmware/flash_esp32.go` | ESP32 flash pipeline (standalone esptool) |
| `app/go/flash/commands.go` | `tools status` / `tools download` CLI commands |
| `app/go/studio/app.go` | `GetToolsStatus()` / `DownloadEsptool()` GUI bindings |

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
    cause: "Device rebooted but COM port reappeared slower than the timeout"
    fix: |      This is a non-critical error — the UF2 was copied successfully.
      Run 'scalefx-flash flash <controller> --no-build' to re-verify.
      Or connect via CLI and run 'init' to confirm the build number.
    note: "BUILD_NUMBER is auto-incremented on each build. If a verification
      timeout occurs, the firmware IS flashed — just the post-check failed."

  "BUILD_NUMBER auto-incremented after failed flash attempt":
    cause: "Flash CLI increments BUILD_NUMBER before flashing"
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

  "Always use VS Code tasks for building":
    rule: |
      The workspace defines predefined tasks in .vscode/tasks.json for all
      build, flash, and syntax-check operations. AI agents MUST use these
      via create_and_run_task instead of raw run_in_terminal commands.
      See Rule 20 in copilot-instructions.md.
    available_tasks:
      - "Build Firmware (prompts for controller)"
      - "Build and Flash Firmware (prompts for controller)"
      - "Flash Firmware (no build)"
      - "Build All Controllers"
      - "Build Go CLI"
      - "Build Flash CLI"
      - "Build ScaleFX Studio (GUI)"
    important: "tasks.json must be valid JSON (no comments) for create_and_run_task to work"
```

---

## Verification After Flash

```bash
# Quick verification via CLI
app/go/scalefx-cli.exe -p COM5

# Commands to verify:
> init
> status
```

**Expected:** Device responds with INIT_READY containing correct version string.
