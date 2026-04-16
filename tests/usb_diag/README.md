# USB Slave Detection Diagnostic Tool

Connects to HubFX ESP32-S3 via serial and runs a comprehensive diagnostic sequence to troubleshoot USB host and slave detection issues.

## Usage

```bash
# From this directory:
go run . -port COM16

# With verbose packet logging:
go run . -port COM16 -verbose

# Reset USB bus before diagnostics:
go run . -port COM16 -reset

# Continuous monitoring (repeat every 5 seconds):
go run . -port COM16 -loop 5

# Without port arg — lists available serial ports:
go run .
```

## What It Tests

| Step | Command | What It Checks |
|------|---------|----------------|
| 1 | IDENTIFY | Controller type, firmware version, build number |
| 2 | STATUS | Core status, HubFX flags (Core1, Audio, Flash, SD, USB), slave bitmask |
| 3 | USB_RESET (optional) | Power-cycles the USB root port |
| 4 | USB_DEVICES_REQ | USB host state, CDC device enumeration, VID/PID, device state |
| 5 | SLAVE_LIST | Registered slaves, connected/ready status |
| 6 | SLAVE_INFO ×3 | Per-slave board info (GunFX, LightFX, GearControl) |
| 7 | STATUS (final) | Re-check after queries to detect state changes |

## Diagnostic Hints

The tool provides contextual hints for common issues:

- **No USB devices**: Cable, power, or USB hub problems
- **Default PID (0x000A)**: Custom `tusb_config.h` PID not active
- **Mounted but no slave type**: INIT handshake failed
- **Connected but not ready**: INIT timeout or firmware mismatch
- **USB host not initialized**: Hardware or firmware initialization failure

## Build

```bash
cd tests/usb_diag
go build -o usb_diag.exe .
```

Requires the `scalefx` module (resolved via `replace` directive in go.mod).
