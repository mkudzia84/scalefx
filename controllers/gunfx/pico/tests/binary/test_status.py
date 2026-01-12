"""
GunFX Binary Protocol Status Tests

Tests for STATUS_REQ/STATUS using binary protocol:
- STATUS_REQ command triggers status response
- Status packet format and parsing (28-byte format)
- Status field values and metrics
"""

import pytest
import time
import struct
from conftest import GunFxBinaryConnection, SFX_PKT_STATUS, SFX_PKT_STATUS_REQ


@pytest.mark.hardware
@pytest.mark.binary
class TestStatusRequest:
    """Test STATUS_REQ command functionality."""
    
    def test_status_req_returns_status(self, gunfx_binary: GunFxBinaryConnection):
        """Test that STATUS_REQ command returns a STATUS response."""
        status = gunfx_binary.request_status(timeout=2.0)
        assert status is not None, "STATUS_REQ should return a STATUS response"
    
    def test_status_req_multiple_times(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS_REQ can be called multiple times."""
        for i in range(3):
            status = gunfx_binary.request_status(timeout=2.0)
            assert status is not None, f"STATUS_REQ #{i+1} should return STATUS"
            time.sleep(0.1)
    
    def test_status_req_immediate_response(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS_REQ returns quickly."""
        start = time.time()
        status = gunfx_binary.request_status(timeout=2.0)
        elapsed = time.time() - start
        
        assert status is not None
        assert elapsed < 0.5, f"STATUS_REQ should respond quickly, took {elapsed:.2f}s"


@pytest.mark.hardware
@pytest.mark.binary
class TestStatusFormat:
    """Test STATUS packet format."""
    
    def test_status_received(self, gunfx_binary: GunFxBinaryConnection):
        """Test that STATUS packets are received."""
        status = gunfx_binary.request_status(timeout=3.0)
        assert status is not None, "Should receive STATUS packet"
    
    def test_status_has_firing_field(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS contains firing field."""
        status = gunfx_binary.request_status()
        assert 'firing' in status, "STATUS should have 'firing' field"
        assert isinstance(status['firing'], bool)
    
    def test_status_has_flash_fields(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS contains flash fields."""
        status = gunfx_binary.request_status()
        assert 'flashActive' in status, "STATUS should have 'flashActive' field"
        assert 'flashFading' in status, "STATUS should have 'flashFading' field"
    
    def test_status_has_smoke_fields(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS contains smoke generator fields."""
        status = gunfx_binary.request_status()
        assert 'heaterOn' in status, "STATUS should have 'heaterOn' field"
        assert 'fanOn' in status, "STATUS should have 'fanOn' field"
        assert 'fanSpindown' in status, "STATUS should have 'fanSpindown' field"
    
    def test_status_has_servo_fields(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS contains servo position fields."""
        status = gunfx_binary.request_status()
        assert 'servo0' in status, "STATUS should have 'servo0' field"
        assert 'servo1' in status, "STATUS should have 'servo1' field"
        assert 'servo2' in status, "STATUS should have 'servo2' field"
    
    def test_status_has_rpm_field(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS contains RPM field."""
        status = gunfx_binary.request_status()
        assert 'rpm' in status, "STATUS should have 'rpm' field"
    
    def test_status_has_metrics_fields(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS contains new metrics fields."""
        status = gunfx_binary.request_status()
        assert 'shotsFired' in status, "STATUS should have 'shotsFired' field"
        assert 'heaterOnTimeMs' in status, "STATUS should have 'heaterOnTimeMs' field"
        assert 'uptimeMs' in status, "STATUS should have 'uptimeMs' field"
        assert 'freeRam' in status, "STATUS should have 'freeRam' field"
        assert 'fanSpeed' in status, "STATUS should have 'fanSpeed' field"


@pytest.mark.hardware
@pytest.mark.binary
class TestStatusValues:
    """Test STATUS field values."""
    
    def test_status_idle_state(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test STATUS values when idle."""
        # Ensure clean state
        fresh_gunfx_binary.trigger_off(0)
        fresh_gunfx_binary.smoke_heat(False)
        time.sleep(0.5)
        
        status = fresh_gunfx_binary.request_status()
        assert status.get('firing') == False
        assert status.get('rpm') == 0
        assert status.get('heaterOn') == False
    
    def test_status_firing_state(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test STATUS values during firing."""
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.3)
        
        status = fresh_gunfx_binary.request_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 600
        
        fresh_gunfx_binary.trigger_off(0)
    
    def test_status_servo_positions(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test STATUS reflects servo positions."""
        # Set known positions
        fresh_gunfx_binary.servo_set(1, 1200)
        fresh_gunfx_binary.servo_set(2, 1500)
        fresh_gunfx_binary.servo_set(3, 1800)
        time.sleep(0.3)
        
        status = fresh_gunfx_binary.request_status()
        assert status.get('servo0') == 1200
        assert status.get('servo1') == 1500
        assert status.get('servo2') == 1800
        
        # Reset
        for i in range(1, 4):
            fresh_gunfx_binary.servo_set(i, 1500)
    
    def test_status_heater_state(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test STATUS reflects heater state."""
        fresh_gunfx_binary.smoke_heat(True)
        time.sleep(0.3)
        
        status = fresh_gunfx_binary.request_status()
        assert status.get('heaterOn') == True
        
        fresh_gunfx_binary.smoke_heat(False)
        time.sleep(0.3)
        
        status = fresh_gunfx_binary.request_status()
        assert status.get('heaterOn') == False
    
    def test_status_uptime_increases(self, gunfx_binary: GunFxBinaryConnection):
        """Test uptime increases over time."""
        status1 = gunfx_binary.request_status()
        time.sleep(0.5)
        status2 = gunfx_binary.request_status()
        
        uptime1 = status1.get('uptimeMs', 0)
        uptime2 = status2.get('uptimeMs', 0)
        
        assert uptime2 > uptime1, f"Uptime should increase: {uptime1} -> {uptime2}"
    
    def test_status_free_ram_positive(self, gunfx_binary: GunFxBinaryConnection):
        """Test freeRam is a reasonable positive value."""
        status = gunfx_binary.request_status()
        free_ram = status.get('freeRam', 0)
        
        # RP2040 has 264KB SRAM, free RAM should be positive and reasonable
        assert free_ram > 0, "freeRam should be positive"
        assert free_ram < 300000, f"freeRam={free_ram} seems too large"


@pytest.mark.hardware
@pytest.mark.binary
class TestStatusMetrics:
    """Test STATUS metrics tracking."""
    
    def test_shots_fired_increments(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test shotsFired increments after burst."""
        # Get initial count
        status1 = fresh_gunfx_binary.request_status()
        initial_shots = status1.get('shotsFired', 0)
        
        # Fire a short burst
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(1.0)  # Allow some shots
        fresh_gunfx_binary.trigger_off(0)
        time.sleep(0.3)
        
        # Check shots increased
        status2 = fresh_gunfx_binary.request_status()
        final_shots = status2.get('shotsFired', 0)
        
        assert final_shots > initial_shots, f"shotsFired should increase: {initial_shots} -> {final_shots}"
    
    def test_heater_on_time_accumulates(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test heaterOnTimeMs accumulates when heater is on."""
        # Get initial time
        status1 = fresh_gunfx_binary.request_status()
        initial_time = status1.get('heaterOnTimeMs', 0)
        
        # Turn heater on briefly
        fresh_gunfx_binary.smoke_heat(True)
        time.sleep(1.0)
        fresh_gunfx_binary.smoke_heat(False)
        time.sleep(0.3)
        
        # Check time accumulated
        status2 = fresh_gunfx_binary.request_status()
        final_time = status2.get('heaterOnTimeMs', 0)
        
        assert final_time > initial_time, f"heaterOnTimeMs should increase: {initial_time} -> {final_time}"
        # Should have accumulated roughly 1 second
        delta = final_time - initial_time
        assert 800 <= delta <= 1500, f"heaterOnTimeMs delta={delta}ms, expected ~1000ms"


@pytest.mark.hardware
@pytest.mark.binary
class TestStatusBinaryFormat:
    """Test binary STATUS packet structure."""
    
    def test_status_flags_byte(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS flags byte structure."""
        status = gunfx_binary.request_status()
        
        # All flag fields should be booleans
        assert isinstance(status.get('firing'), bool)
        assert isinstance(status.get('flashActive'), bool)
        assert isinstance(status.get('flashFading'), bool)
        assert isinstance(status.get('heaterOn'), bool)
        assert isinstance(status.get('fanOn'), bool)
        assert isinstance(status.get('fanSpindown'), bool)
    
    def test_status_u16_fields(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS u16 fields are valid."""
        status = gunfx_binary.request_status()
        
        # All u16 fields should be integers in valid range
        assert 0 <= status.get('fanOffRemainingMs', 0) <= 65535
        assert 0 <= status.get('servo0', 0) <= 65535
        assert 0 <= status.get('servo1', 0) <= 65535
        assert 0 <= status.get('servo2', 0) <= 65535
        assert 0 <= status.get('rpm', 0) <= 65535
    
    def test_status_u32_fields(self, gunfx_binary: GunFxBinaryConnection):
        """Test STATUS u32 fields are valid."""
        status = gunfx_binary.request_status()
        
        # All u32 fields should be integers in valid range
        assert 0 <= status.get('shotsFired', 0) <= 0xFFFFFFFF
        assert 0 <= status.get('heaterOnTimeMs', 0) <= 0xFFFFFFFF
        assert 0 <= status.get('uptimeMs', 0) <= 0xFFFFFFFF
        assert 0 <= status.get('freeRam', 0) <= 0xFFFFFFFF
    
    def test_status_servo_typical_range(self, gunfx_binary: GunFxBinaryConnection):
        """Test servo values are in typical PWM range."""
        status = gunfx_binary.request_status()
        
        # Servo values should typically be 500-2500µs or 0 (not configured)
        for field in ['servo0', 'servo1', 'servo2']:
            value = status.get(field, 0)
            assert value == 0 or (500 <= value <= 2500), f"{field}={value} out of range"
