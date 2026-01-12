# GunFX Pico Test Suite

Functional test suite for the GunFX Pico board using the text-based serial protocol.

## Prerequisites

- Python 3.8+
- pytest
- pyserial
- GunFX board connected via USB (typically `/dev/ttyACM0` on Linux or `COM3` on Windows)

## Installation

```bash
pip install pytest pyserial
```

## Configuration

Set the `GUNFX_PORT` environment variable to specify the serial port:

```bash
# Linux
export GUNFX_PORT=/dev/ttyACM0

# Windows
set GUNFX_PORT=COM3
```

Default port is `COM3` if not specified.

## Running Tests

### Run all tests
```bash
cd controllers/gunfx/pico
pytest tests/ -v
```

### Run specific test file
```bash
pytest tests/test_trigger.py -v
pytest tests/test_servo.py -v
```

### Run by marker
```bash
# Only hardware tests (requires connected device)
pytest tests/ -m hardware -v

# Skip firing tests (no physical gun effects)
pytest tests/ -m "not firing" -v

# Skip destructive tests (REBOOT, BOOTSEL)
pytest tests/ -m "not destructive" -v

# Only smoke generator tests
pytest tests/ -m smoke -v

# Only servo tests
pytest tests/ -m servo -v
```

### Quick validation
```bash
# Run fast tests only
pytest tests/ -m "not slow and not destructive" -v
```

## Test Markers

| Marker | Description |
|--------|-------------|
| `hardware` | Requires connected GunFX hardware |
| `firing` | Tests that trigger gun effects (LED, sound, etc.) |
| `servo` | Tests that move servo motors |
| `smoke` | Tests that control smoke generator |
| `slow` | Tests that take longer than 5 seconds |
| `destructive` | Tests that reboot/reset the device |

## Test Files

| File | Description |
|------|-------------|
| `conftest.py` | Test configuration, fixtures, and `GunFxConnection` class |
| `test_system.py` | INIT, SHUTDOWN, KEEPALIVE, connection tests |
| `test_trigger.py` | TRIGGER_ON, TRIGGER_OFF command tests |
| `test_servo.py` | SERVO_SET, SERVO_CONFIG, SERVO_RECOIL_JERK tests |
| `test_smoke.py` | SMOKE_HEAT, SMOKE_SETTINGS command tests |
| `test_status.py` | STATUS telemetry format and content tests |
| `test_errors.py` | Error handling and NACK response tests |

## Protocol Commands Tested

### System Commands
- `INIT protocol=text` - Initialize with text protocol
- `KEEPALIVE` - Heartbeat to maintain connection
- `SHUTDOWN` - Clean shutdown (no ACK)
- `REBOOT` - Restart device (no ACK)
- `BOOTSEL` - Enter bootloader mode (no ACK)

### Trigger Commands
- `TRIGGER_ON rpm=N` - Start firing at N RPM (60-3000)
- `TRIGGER_OFF [fanDelayMs=N]` - Stop firing, optional fan delay

### Servo Commands
- `SERVO_SET id=N pulseUs=P` - Set servo position (500-2500µs)
- `SERVO_CONFIG id=N minUs=M maxUs=X [defaultUs=D]` - Configure servo limits
- `SERVO_RECOIL_JERK id=N jerkUs=J varianceUs=V` - Configure recoil servo

### Smoke Commands
- `SMOKE_HEAT enable=0|1` - Enable/disable heater
- `SMOKE_SETTINGS fanSpeedPct=F heaterPct=H pumpSpeedPct=P` - Configure smoke parameters

### Responses
- `INIT_READY protocol=text build=N` - Connection established
- `ACK` - Command accepted
- `NACK code=0xNN reason="..."` - Command rejected with error code
- `STATUS firing=T rpm=N ...` - Periodic telemetry

## GunFxConnection Class

The `GunFxConnection` class in `conftest.py` provides a convenient interface:

```python
from conftest import GunFxConnection

gunfx = GunFxConnection(port="COM3")
if gunfx.connect():
    # Send command and expect ACK
    success, response = gunfx.send_and_expect_ack("TRIGGER_ON rpm=600")
    
    # Wait for STATUS telemetry
    status = gunfx.wait_for_status(timeout=2.0)
    print(f"Firing: {status.get('firing')}, RPM: {status.get('rpm')}")
    
    # Stop and clean up
    gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    gunfx.close()
```

## Writing New Tests

```python
import pytest
from conftest import GunFxConnection

class TestMyFeature:
    """Test description."""
    
    @pytest.mark.hardware
    @pytest.mark.firing
    def test_my_feature(self, fresh_gunfx: GunFxConnection):
        """Test specific behavior."""
        # fresh_gunfx fixture provides clean connection
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=600")
        assert success
        
        status = fresh_gunfx.wait_for_status()
        assert status.get('firing') == True
        
        # Always clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
```

## Fixtures

- `gunfx` - Module-scoped connection (reused across tests in same file)
- `fresh_gunfx` - Function-scoped connection (fresh for each test)

## Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | Success |
| 0x01 | UNKNOWN_COMMAND | Command not recognized |
| 0x02 | INVALID_PARAMETER | Parameter value invalid |
| 0x03 | MISSING_PARAMETER | Required parameter missing |
| 0x20 | SERVO_INVALID_ID | Servo ID out of range (1-3) |
| 0x21 | SERVO_PULSE_RANGE | Pulse width out of range (500-2500µs) |
| 0x22 | SERVO_MIN_MAX | Min/max configuration error |
| 0x23 | SERVO_NOT_CONFIGURED | Servo not configured |
| 0x40 | INVALID_RPM | RPM out of range (60-3000) |
| 0x41 | ALREADY_FIRING | Already in firing state |
| 0x42 | NOT_FIRING | Not in firing state |

## Troubleshooting

### "No device found"
- Check `GUNFX_PORT` environment variable
- Verify device is connected via `ls /dev/ttyACM*` (Linux) or Device Manager (Windows)

### "Connection timeout"
- Device may be in bad state - power cycle
- Check baud rate matches (115200)

### "NACK received"
- Check error code and reason in response
- Verify parameter values are in valid ranges

### Tests hang
- Device may have disconnected - check USB connection
- Keepalive timeout may have triggered - reconnect
