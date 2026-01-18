# Test Suite Guide

> **ACTION DOCUMENT:** How to run tests and add test coverage.

---

## Quick Commands

```yaml
Run_All_Tests:
  gunfx: "pytest tests/gunfx/ -v"
  lightfx: "pytest tests/lightfx/ -v"
  noop: "pytest tests/noop/ -v"

Run_Specific_Test:
  file: "pytest tests/gunfx/test_trigger.py -v"
  class: "pytest tests/gunfx/test_trigger.py::TestTriggerOn -v"
  method: "pytest tests/gunfx/test_trigger.py::TestTriggerOn::test_valid_rpm -v"

With_Port_Override:
  command: "SCALEFX_PORT=COM5 pytest tests/gunfx/ -v"
```

---

## Prerequisites

```yaml
Install_Dependencies:
  command: "pip install -r tests/requirements.txt"
  packages:
    - "pyserial>=3.5"
    - "pytest>=7.0.0"
    - "colorama>=0.4.6"

Hardware_Setup:
  - "Connect controller via USB"
  - "Ensure no other app uses the port"
  - "Note port name (COM3, /dev/ttyACM0)"
```

---

## CRITICAL: Build Before Test

```yaml
Always_Flash_First:
  reason: "Tests may fail if firmware is out of sync with test code"
  
  workflow:
    1: "cd controllers/gunfx/pico"
    2: "python scripts/build_and_flash.py"
    3: "cd ../../.."
    4: "pytest tests/gunfx/ -v"
```

### Full Test Cycle Script

```powershell
# test_controller.ps1
param([string]$Controller = "gunfx")

$ErrorActionPreference = "Stop"

Write-Host "Building and flashing $Controller..." -ForegroundColor Cyan
Set-Location "controllers\$Controller\pico"
python scripts\build_and_flash.py

Write-Host "Running tests..." -ForegroundColor Cyan
Set-Location ..\..\..
pytest "tests\$Controller\" -v --tb=short

Write-Host "Done!" -ForegroundColor Green
```

---

## Test Structure

```yaml
Directory_Layout:
  tests/:
    conftest.py: "pytest fixtures (connection, etc.)"
    requirements.txt: "Python dependencies"
    framework/:
      __init__.py: "Public exports"
      connection.py: "ScaleFXConnection class"
      protocol.py: "COBS, CRC, packet building"
      packets.py: "Packet/error constants"
      commands.py: "Command builders"
    cli/:
      interactive.py: "Interactive CLI"
    gunfx/:
      test_system.py: "INIT, SHUTDOWN, STATUS"
      test_trigger.py: "Trigger commands"
      test_servo.py: "Servo commands"
      test_smoke.py: "Smoke commands"
    lightfx/:
      test_system.py: "INIT, SHUTDOWN, STATUS"
      test_led.py: "LED commands"
      test_servo.py: "Servo commands"
      test_power.py: "Power monitor"
    noop/:
      test_system.py: "Core protocol only"
```

---

## Connection Fixture

```python
# conftest.py provides this automatically
@pytest.fixture
def connection():
    """Provide connected ScaleFXConnection."""
    port = os.environ.get('SCALEFX_PORT') or auto_detect_port()
    conn = ScaleFXConnection(port=port)
    conn.connect(init=False)
    yield conn
    conn.close()
```

**Usage in tests:**
```python
class TestExample:
    def test_something(self, connection):
        # connection is auto-injected by pytest
        response = connection.send_and_wait(packet)
```

---

## Writing Tests

### Test Template

```python
"""Tests for [feature]."""
import pytest
from tests.framework import (
    ScaleFXConnection, CommandBuilder, XxxCommands,
    XxxPacket, XxxError, CoreError
)
from tests.framework.protocol import build_packet


class TestFeatureName:
    """Tests for FEATURE_NAME command."""
    
    def test_valid_parameters(self, connection):
        """Test with valid parameters."""
        # Initialize device first
        connection.send_and_wait(CommandBuilder.init())
        
        # Send command
        packet = XxxCommands.feature_name(valid_param)
        success, response = connection.send_expect_ack(packet)
        
        # Assert success
        assert success, f"Expected ACK, got: {response}"
    
    def test_edge_case_min(self, connection):
        """Test minimum valid value."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = XxxCommands.feature_name(MIN_VALUE)
        success, response = connection.send_expect_ack(packet)
        
        assert success
    
    def test_edge_case_max(self, connection):
        """Test maximum valid value."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = XxxCommands.feature_name(MAX_VALUE)
        success, response = connection.send_expect_ack(packet)
        
        assert success
    
    def test_invalid_parameter(self, connection):
        """Test with invalid parameter value."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = XxxCommands.feature_name(INVALID_VALUE)
        success, response = connection.send_expect_ack(packet)
        
        assert not success
        assert response.error_code == XxxError.EXPECTED_ERROR
    
    def test_missing_payload(self, connection):
        """Test with empty payload."""
        connection.send_and_wait(CommandBuilder.init())
        
        # Build packet manually with empty payload
        packet = build_packet(XxxPacket.FEATURE_NAME, b'')
        success, response = connection.send_expect_ack(packet)
        
        assert not success
        assert response.error_code == CoreError.MISSING_PARAMETER
    
    def test_without_init(self, connection):
        """Test command without initialization."""
        # Skip init - go straight to command
        packet = XxxCommands.feature_name(valid_param)
        success, response = connection.send_expect_ack(packet)
        
        # May succeed or fail depending on firmware design
        # Document expected behavior
```

---

## Test Patterns

### Pattern: Expect ACK

```python
packet = XxxCommands.some_command(param)
success, response = connection.send_expect_ack(packet)
assert success
```

### Pattern: Expect NACK with Specific Error

```python
packet = XxxCommands.some_command(invalid_param)
success, response = connection.send_expect_ack(packet)
assert not success
assert response.error_code == XxxError.SPECIFIC_ERROR
```

### Pattern: Expect Data Response

```python
packet = CommandBuilder.status_req()
response = connection.send_and_wait(packet)
assert response.packet_type == CorePacket.STATUS
assert len(response.payload) >= expected_length
```

### Pattern: State-Dependent Test

```python
def test_requires_trigger_on(self, connection):
    """Test that depends on trigger being active."""
    connection.send_and_wait(CommandBuilder.init())
    
    # Setup: activate trigger
    connection.send_expect_ack(GunFxCommands.trigger_on(600))
    
    # Test the dependent behavior
    packet = GunFxCommands.some_command()
    success, response = connection.send_expect_ack(packet)
    assert success
    
    # Cleanup: deactivate trigger
    connection.send_expect_ack(GunFxCommands.trigger_off())
```

---

## Test Categories

```yaml
System_Tests:
  file_pattern: "test_system.py"
  covers:
    - "INIT / INIT_READY"
    - "SHUTDOWN"
    - "STATUS / STATUS_REQ"
    - "KEEPALIVE"
    - "REBOOT"
    - "BOOTSEL"
  
  required_tests:
    - "test_init_response"
    - "test_init_ready_format"
    - "test_shutdown"
    - "test_status_request"
    - "test_keepalive"

Feature_Tests:
  file_pattern: "test_<feature>.py"
  covers:
    - "All commands for that feature"
    - "Valid parameters"
    - "Invalid parameters"
    - "Edge cases"
    - "Error conditions"
```

---

## Running Tests

### Basic Run

```bash
pytest tests/gunfx/ -v
```

### With Detailed Failures

```bash
pytest tests/gunfx/ -v --tb=long
```

### Stop on First Failure

```bash
pytest tests/gunfx/ -v -x
```

### Only Failed Tests (Re-run)

```bash
pytest tests/gunfx/ -v --lf
```

### With Coverage

```bash
pytest tests/gunfx/ -v --cov=tests.framework
```

---

## Troubleshooting

```yaml
"No ScaleFX device found":
  cause: "Port not detected"
  fixes:
    - "Check USB connection"
    - "Set SCALEFX_PORT environment variable"
    - "Verify device shows in Device Manager"

"Connection refused / timeout":
  cause: "Another application using port"
  fixes:
    - "Close other serial monitors"
    - "Close Arduino IDE"
    - "Kill other Python processes"

"NACK with unexpected error":
  cause: "Firmware mismatch or bug"
  fixes:
    - "Rebuild and reflash firmware"
    - "Check test is using correct packet format"

"Test passes locally but fails in CI":
  cause: "Timing sensitivity"
  fixes:
    - "Add small delays between commands"
    - "Increase timeout values"
```

---

## Adding Tests for New Command

```yaml
Checklist:
  - "[ ] Create test file: tests/xxxfx/test_<feature>.py"
  - "[ ] Add test class with descriptive name"
  - "[ ] Add test_valid_parameters"
  - "[ ] Add test_edge_cases (min/max values)"
  - "[ ] Add test_invalid_parameters"
  - "[ ] Add test_missing_payload"
  - "[ ] Run tests: pytest tests/xxxfx/test_<feature>.py -v"
  - "[ ] Verify all tests pass"
```
