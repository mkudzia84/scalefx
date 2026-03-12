# HubFX Pico - Test Suite

Automated pytest-based testing for CLI commands and firmware functionality.

## Quick Start

```bash
# Install dependencies
pip install pytest pyserial

# Run all tests
pytest tests/

# Run specific test file
pytest tests/test_cli_system.py

# Run with verbose output
pytest tests/ -v

# Run only JSON output tests
pytest tests/ -m json

# Run hardware-dependent tests
pytest tests/ -m hardware
```

## Test Structure

### Core Framework

#### `conftest.py`
Pytest configuration and shared fixtures:
- `SerialConnection` class - Handles serial communication with Pico
- `serial_port` fixture - Auto-detects COM port
- `pico` fixture - Module-scoped connection (reused across tests)
- `fresh_pico` fixture - Function-scoped connection (fresh for each test)

### CLI Test Modules

#### `test_cli_system.py`
System-level commands:
- `ping` - Heartbeat/connectivity
- `version` - Firmware version and build info
- `status` - System status summary
- `help` - Command help display
- Unknown command handling
- `--json` flag parsing

#### `test_cli_storage.py`
SD card and storage commands:
- `sd init` - Initialize SD card
- `sd ls` - Directory listing
- `sd tree` - Directory tree view
- `sd info` - Card info and capacity
- `sd cat` - File content display
- `sd rm` - File deletion
- Upload/download operations

#### `test_cli_engine.py`
Engine effects control:
- `engine status` - Effect state
- `engine start` - Start engine simulation
- `engine stop` - Stop engine
- State transition tests

#### `test_cli_gun.py`
Gun effects control:
- `gun status` - Gun state
- `gun fire` - Start firing
- `gun ceasefire` - Stop firing
- `gun heater` - Barrel heater control
- `gun servo` - Servo position control
- Fire sequence tests

#### `test_cli_config.py`
Configuration management:
- `config display` - Show current config
- `config backup` - Backup to file
- `config restore` - Restore from backup
- `config reload` - Reload from disk
- JSON structure validation

#### `test_cli_audio.py`
Audio playback and codec:
- `audio play` - Play sound files
- `audio stop` - Stop playback
- `volume` - Volume control
- `codec dump` - Register dump
- `codec reset` - Reset codec
- Codec debug commands

### Legacy Tests

#### `test_audio_mixer.py`
Comprehensive audio mixer testing (492 lines):
- Sound file playback
- Channel management
- Volume control
- Loop mode
- Concurrent playback

## Test Markers

Use markers to filter tests:

```bash
# Hardware-dependent tests (require physical connections)
pytest -m hardware

# JSON output validation tests
pytest -m json

# Slow tests
pytest -m slow
```

## Configuration

### Serial Port

Set via environment variable:
```bash
set PICO_PORT=COM4
pytest tests/
```

Or auto-detection will find the first available Pico.

### Timeouts

Default timeout is 2 seconds per command. Adjust in test if needed:
```python
response = pico.send("slow command", delay=5.0)
```

## Writing New Tests

### Basic Test Pattern

```python
def test_example_command(fresh_pico: SerialConnection):
    """Test description."""
    response = fresh_pico.send("command args")
    assert "expected" in response
```

### JSON Response Pattern

```python
@pytest.mark.json
def test_json_command(fresh_pico: SerialConnection):
    """Test JSON output."""
    data = fresh_pico.send_json("command --json")
    assert data["status"] == "ok"
    assert "field" in data
```

### Hardware Test Pattern

```python
@pytest.mark.hardware
def test_hardware_feature(fresh_pico: SerialConnection):
    """Test requiring hardware."""
    response = fresh_pico.send("codec scan", delay=1.0)
    assert "device" in response.lower()
```

## Common Issues

### COM Port Access
If you get "Access denied" errors:
1. Close any serial monitors (Arduino IDE, PlatformIO, PuTTY)
2. Disconnect/reconnect USB cable
3. Check Device Manager for correct COM port

### Device Not Responding
If tests timeout:
1. Check USB connection
2. Try power cycling the Pico
3. Verify COM port with `mode` command (Windows)
4. Check if another process has the port open

### I2C Errors
- **Error 4 (no device):** Check 3.3V/GND power to codec
- **Error 5 (timeout):** Lower I2C speed in `audio_config.h` to 50kHz or 10kHz

## See Also

- [../docs/AUDIO_CONFIGURATION.md](../docs/AUDIO_CONFIGURATION.md) - I2C speed tuning
- [../scripts/](../scripts/) - Build and flash utilities
