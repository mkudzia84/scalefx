"""
GunFX Text Protocol Status Tests

Tests for STATUS telemetry using text protocol:
- STATUS_REQ command triggers status response
- Status packet format and parsing
- Status field values and types
- Status metrics tracking
"""

import pytest
import time
from conftest import GunFxConnection


@pytest.mark.hardware
@pytest.mark.text
class TestStatusRequest:
    """Test STATUS_REQ command functionality."""
    
    def test_status_req_returns_status(self, gunfx: GunFxConnection):
        """Test that STATUS_REQ command returns a STATUS response."""
        status = gunfx.request_status(timeout=2.0)
        assert status is not None, "STATUS_REQ should return a STATUS response"
    
    def test_status_req_multiple_times(self, gunfx: GunFxConnection):
        """Test STATUS_REQ can be called multiple times."""
        for i in range(3):
            status = gunfx.request_status(timeout=2.0)
            assert status is not None, f"STATUS_REQ #{i+1} should return STATUS"
            time.sleep(0.1)
    
    def test_status_req_immediate_response(self, gunfx: GunFxConnection):
        """Test STATUS_REQ returns quickly."""
        start = time.time()
        status = gunfx.request_status(timeout=2.0)
        elapsed = time.time() - start
        
        assert status is not None
        assert elapsed < 0.5, f"STATUS_REQ should respond quickly, took {elapsed:.2f}s"


@pytest.mark.hardware
@pytest.mark.text
class TestStatusFormat:
    """Test STATUS packet format."""
    
    def test_status_has_firing_field(self, gunfx: GunFxConnection):
        """Test STATUS contains firing field."""
        status = gunfx.request_status()
        assert 'firing' in status, "STATUS should have 'firing' field"
        assert isinstance(status['firing'], bool)
    
    def test_status_has_flash_fields(self, gunfx: GunFxConnection):
        """Test STATUS contains flash fields."""
        status = gunfx.request_status()
        assert 'flashActive' in status, "STATUS should have 'flashActive' field"
        assert 'flashFading' in status, "STATUS should have 'flashFading' field"
    
    def test_status_has_smoke_fields(self, gunfx: GunFxConnection):
        """Test STATUS contains smoke generator fields."""
        status = gunfx.request_status()
        assert 'heaterOn' in status, "STATUS should have 'heaterOn' field"
        assert 'fanOn' in status, "STATUS should have 'fanOn' field"
        assert 'fanSpindown' in status, "STATUS should have 'fanSpindown' field"
    
    def test_status_has_servo_fields(self, gunfx: GunFxConnection):
        """Test STATUS contains servo position fields."""
        status = gunfx.request_status()
        assert 'servo0' in status, "STATUS should have 'servo0' field"
        assert 'servo1' in status, "STATUS should have 'servo1' field"
        assert 'servo2' in status, "STATUS should have 'servo2' field"
    
    def test_status_has_rpm_field(self, gunfx: GunFxConnection):
        """Test STATUS contains RPM field."""
        status = gunfx.request_status()
        assert 'rpm' in status, "STATUS should have 'rpm' field"
    
    def test_status_has_metrics_fields(self, gunfx: GunFxConnection):
        """Test STATUS contains new metrics fields."""
        status = gunfx.request_status()
        assert 'shotsFired' in status, "STATUS should have 'shotsFired' field"
        assert 'heaterOnTimeMs' in status, "STATUS should have 'heaterOnTimeMs' field"
        assert 'uptimeMs' in status, "STATUS should have 'uptimeMs' field"
        assert 'freeRam' in status, "STATUS should have 'freeRam' field"
        assert 'fanSpeed' in status, "STATUS should have 'fanSpeed' field"
    
    def test_status_field_types(self, gunfx: GunFxConnection):
        """Test STATUS field types are correct."""
        status = gunfx.request_status()
        
        # Boolean fields
        assert isinstance(status.get('firing'), bool)
        assert isinstance(status.get('flashActive'), bool)
        assert isinstance(status.get('flashFading'), bool)
        assert isinstance(status.get('heaterOn'), bool)
        assert isinstance(status.get('fanOn'), bool)
        assert isinstance(status.get('fanSpindown'), bool)
        
        # Integer fields
        assert isinstance(status.get('servo0'), int)
        assert isinstance(status.get('servo1'), int)
        assert isinstance(status.get('servo2'), int)
        assert isinstance(status.get('rpm'), int)
        assert isinstance(status.get('shotsFired'), int)
        assert isinstance(status.get('heaterOnTimeMs'), int)
        assert isinstance(status.get('uptimeMs'), int)
        assert isinstance(status.get('freeRam'), int)
        assert isinstance(status.get('fanSpeed'), int)


@pytest.mark.hardware
@pytest.mark.text
class TestStatusValues:
    """Test STATUS field values."""
    
    def test_status_idle_state(self, fresh_gunfx: GunFxConnection):
        """Test STATUS values when idle."""
        # Ensure clean state
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
        time.sleep(0.5)
        
        status = fresh_gunfx.request_status()
        assert status.get('firing') == False
        assert status.get('rpm') == 0
        assert status.get('heaterOn') == False
    
    def test_status_servo_range(self, fresh_gunfx: GunFxConnection):
        """Test servo values are in valid PWM range."""
        status = fresh_gunfx.request_status()
        
        for field in ['servo0', 'servo1', 'servo2']:
            value = status.get(field, 0)
            assert value == 0 or (500 <= value <= 2500), f"{field}={value} out of range"
    
    def test_status_uptime_increases(self, gunfx: GunFxConnection):
        """Test uptime increases over time."""
        status1 = gunfx.request_status()
        time.sleep(0.5)
        status2 = gunfx.request_status()
        
        uptime1 = status1.get('uptimeMs', 0)
        uptime2 = status2.get('uptimeMs', 0)
        
        assert uptime2 > uptime1, f"Uptime should increase: {uptime1} -> {uptime2}"
    
    def test_status_free_ram_positive(self, gunfx: GunFxConnection):
        """Test freeRam is a reasonable positive value."""
        status = gunfx.request_status()
        free_ram = status.get('freeRam', 0)
        
        # RP2040 has 264KB SRAM, free RAM should be positive and reasonable
        assert free_ram > 0, "freeRam should be positive"
        assert free_ram < 300000, f"freeRam={free_ram} seems too large"
    
    def test_status_firing_state(self, fresh_gunfx: GunFxConnection):
        """Test STATUS values during firing."""
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.3)
        
        status = fresh_gunfx.request_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 600
        
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    def test_status_heater_state(self, fresh_gunfx: GunFxConnection):
        """Test STATUS reflects heater state."""
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(0.3)
        
        status = fresh_gunfx.request_status()
        assert status.get('heaterOn') == True
        
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
        time.sleep(0.3)
        
        status = fresh_gunfx.request_status()
        assert status.get('heaterOn') == False
    
    def test_status_servo_positions(self, fresh_gunfx: GunFxConnection):
        """Test STATUS reflects servo positions."""
        fresh_gunfx.send_command("SERVO_SET id=1 pulseUs=1200")
        fresh_gunfx.send_command("SERVO_SET id=2 pulseUs=1500")
        fresh_gunfx.send_command("SERVO_SET id=3 pulseUs=1800")
        time.sleep(0.3)
        
        status = fresh_gunfx.request_status()
        assert status.get('servo0') == 1200
        assert status.get('servo1') == 1500
        assert status.get('servo2') == 1800
        
        # Reset servos
        for i in range(1, 4):
            fresh_gunfx.send_command(f"SERVO_SET id={i} pulseUs=1500")


@pytest.mark.hardware
@pytest.mark.text
class TestStatusMetrics:
    """Test STATUS metrics tracking."""
    
    def test_shots_fired_increments(self, fresh_gunfx: GunFxConnection):
        """Test shotsFired increments after burst."""
        # Get initial count
        status1 = fresh_gunfx.request_status()
        initial_shots = status1.get('shotsFired', 0)
        
        # Fire a short burst
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(1.0)  # Allow some shots
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        time.sleep(0.3)
        
        # Check shots increased
        status2 = fresh_gunfx.request_status()
        final_shots = status2.get('shotsFired', 0)
        
        assert final_shots > initial_shots, f"shotsFired should increase: {initial_shots} -> {final_shots}"
    
    def test_heater_on_time_accumulates(self, fresh_gunfx: GunFxConnection):
        """Test heaterOnTimeMs accumulates when heater is on."""
        # Get initial time
        status1 = fresh_gunfx.request_status()
        initial_time = status1.get('heaterOnTimeMs', 0)
        
        # Turn heater on briefly
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(1.0)
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
        time.sleep(0.3)
        
        # Check time accumulated
        status2 = fresh_gunfx.request_status()
        final_time = status2.get('heaterOnTimeMs', 0)
        
        assert final_time > initial_time, f"heaterOnTimeMs should increase: {initial_time} -> {final_time}"
        # Should have accumulated roughly 1 second
        delta = final_time - initial_time
        assert 800 <= delta <= 1500, f"heaterOnTimeMs delta={delta}ms, expected ~1000ms"
    
    def test_fan_speed_reflects_state(self, fresh_gunfx: GunFxConnection):
        """Test fanSpeed reflects fan state."""
        # Ensure heater/fan off
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
        time.sleep(0.5)  # Wait for spindown
        
        status1 = fresh_gunfx.request_status()
        # Fan may still be in spindown, but when off should be 0
        
        # Turn heater on (should turn fan on)
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(0.3)
        
        status2 = fresh_gunfx.request_status()
        assert status2.get('fanOn') == True or status2.get('fanSpeed', 0) > 0
        
        # Clean up
        fresh_gunfx.send_command("SMOKE_HEAT on=0")


@pytest.mark.hardware
@pytest.mark.text
class TestStatusConsistency:
    """Test STATUS consistency and reliability."""
    
    def test_status_consistent_during_idle(self, gunfx: GunFxConnection):
        """Test STATUS values are consistent when idle."""
        statuses = []
        for _ in range(5):
            statuses.append(gunfx.request_status())
            time.sleep(0.1)
        
        # Firing state should be consistent
        firing_values = [s.get('firing') for s in statuses]
        assert all(f == firing_values[0] for f in firing_values), "firing should be consistent"
        
        # Heater state should be consistent
        heater_values = [s.get('heaterOn') for s in statuses]
        assert all(h == heater_values[0] for h in heater_values), "heaterOn should be consistent"
    
    def test_status_updates_after_command(self, fresh_gunfx: GunFxConnection):
        """Test STATUS reflects state changes after commands."""
        # Start idle
        status1 = fresh_gunfx.request_status()
        assert status1.get('firing') == False
        
        # Start firing
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        status2 = fresh_gunfx.request_status()
        assert status2.get('firing') == True
        
        # Stop firing
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        time.sleep(0.2)
        status3 = fresh_gunfx.request_status()
        assert status3.get('firing') == False
