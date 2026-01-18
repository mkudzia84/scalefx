# ScaleFX Controller Tests

Python-based functional test suite and interactive CLI for testing ScaleFX controllers over USB/serial.

## Directory Structure

```
tests/
├── conftest.py                    # pytest fixtures
├── framework/                     # Common connectivity framework
│   ├── connection.py              # Serial connection handling
│   ├── protocol.py                # COBS encoding/decoding, CRC-8
│   ├── packets.py                 # Packet type constants
│   └── commands.py                # High-level command builders
├── gunfx/                         # GunFX controller tests
├── lightfx/                       # LightFX controller tests
├── noop/                          # NoOp controller tests
└── cli/
    └── interactive.py             # Interactive text CLI
```

## Installation

```bash
pip install -r requirements.txt
```

## Running Tests

**IMPORTANT:** Always build and flash fresh firmware before running tests!

```bash
# Step 1: Build and flash (recommended before each test run)
cd controllers/gunfx/pico
python scripts/build_and_flash.py
cd ../../..

# Step 2: Run tests
pytest tests/gunfx/ -v

# Or run all tests
pytest tests/ -v

# Run specific controller tests
pytest tests/lightfx/ -v
pytest tests/noop/ -v

# Run with specific port (Windows)
$env:SCALEFX_PORT="COM5"; pytest tests/gunfx/ -v

# Run with specific port (Linux/Mac)
SCALEFX_PORT=/dev/ttyACM0 pytest tests/gunfx/ -v
```

## Interactive CLI

```bash
# Start interactive CLI
python -m tests.cli.interactive

# Or with specific port
python -m tests.cli.interactive --port COM3
```

### CLI Commands

The CLI provides human-readable text commands that are translated to binary protocol.
Commands are context-sensitive based on the connected controller type.

**System Commands (always available):**
```
help                    # Show available commands
ports                   # List serial ports
connect [port]          # Connect to controller
disconnect              # Disconnect
```

**Protocol Commands (after connect):**
```
init                    # Initialize connection (detects controller type)
shutdown                # Safe shutdown
reboot                  # Reboot device
bootsel                 # Enter USB bootloader
status                  # Request status
keepalive               # Send keepalive
```

**GunFX Commands (after init on GunFX controller):**
```
gunfx.trigger on 600          # Start firing at 600 RPM
gunfx.trigger off 3000        # Stop firing, 3000ms fan delay
gunfx.servo set 1 1500        # Set servo 1 to 1500µs
gunfx.servo.config 1 1000 2000 4000 8000 8000  # Configure servo
gunfx.smoke heat on           # Enable heater
gunfx.smoke heat off          # Disable heater
```

**LightFX Commands (after init on LightFX controller):**
```
lightfx.led set 1 255         # Set LED channel 1 to brightness 255
lightfx.led off 0             # Turn off all LEDs
lightfx.led.seq clear 1       # Clear sequence on channel 1
lightfx.led.seq start 0       # Start all sequences
lightfx.servo set 1 1500      # Set servo position
lightfx.power                 # Request power status
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `SCALEFX_PORT` | COM3 (Win) / /dev/ttyACM0 (Linux) | Serial port |
| `SCALEFX_BAUD` | 115200 | Baud rate |
| `SCALEFX_TIMEOUT` | 2.0 | Response timeout (seconds) |

## Protocol Details

All communication uses binary COBS protocol:
- Packet format: `[type:u8][len:u8][payload:0-64][crc8:u8]`
- COBS encoded with 0x00 delimiter
- CRC-8 polynomial: 0x07
