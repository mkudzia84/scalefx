"""
GunFX Binary Protocol Servo Tests

Tests for servo control commands using binary protocol:
- SERVO_SET: Set servo position
- SERVO_CONFIG: Configure servo limits and motion profile
- SERVO_RECOIL_JERK: Configure recoil servo jerk behavior
"""

import pytest
import time
import struct
from conftest import (
    GunFxBinaryConnection, GunFxError,
    GUNFX_PKT_SRV_SET, GUNFX_PKT_SRV_SETTINGS, GUNFX_PKT_SRV_RECOIL_JERK
)


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.servo
class TestServoSet:
    """Test SERVO_SET command."""
    
    def test_servo_set_center(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test setting servo to center position (1500µs)."""
        success, _ = fresh_gunfx_binary.servo_set(1, 1500)
        assert success, "SERVO_SET should ACK"
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('servo0') == 1500, "Servo 1 should be at 1500µs"
    
    def test_servo_set_min(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test setting servo to minimum position (500µs)."""
        success, _ = fresh_gunfx_binary.servo_set(1, 500)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('servo0') == 500
        
        # Return to center
        fresh_gunfx_binary.servo_set(1, 1500)
    
    def test_servo_set_max(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test setting servo to maximum position (2500µs)."""
        success, _ = fresh_gunfx_binary.servo_set(1, 2500)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('servo0') == 2500
        
        # Return to center
        fresh_gunfx_binary.servo_set(1, 1500)
    
    def test_servo_all_channels(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test all servo channels (1-3)."""
        positions = [1200, 1500, 1800]
        
        for servo_id in range(1, 4):
            success, _ = fresh_gunfx_binary.servo_set(servo_id, positions[servo_id - 1])
            assert success, f"SERVO_SET id={servo_id} should ACK"
        
        time.sleep(0.3)
        status = fresh_gunfx_binary.wait_for_status()
        
        assert status.get('servo0') == 1200
        assert status.get('servo1') == 1500
        assert status.get('servo2') == 1800
        
        # Reset all to center
        for servo_id in range(1, 4):
            fresh_gunfx_binary.servo_set(servo_id, 1500)
    
    def test_servo_sweep(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test servo sweep through range."""
        # Sweep from 1000 to 2000 in steps
        for pulse in range(1000, 2001, 200):
            success, _ = fresh_gunfx_binary.servo_set(1, pulse)
            assert success
            time.sleep(0.1)
        
        # Return to center
        fresh_gunfx_binary.servo_set(1, 1500)


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.servo
class TestServoConfig:
    """Test SERVO_CONFIG command."""
    
    def test_servo_config_basic(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test basic servo configuration."""
        success, _ = fresh_gunfx_binary.servo_config(1, 1000, 2000)
        assert success, "SERVO_CONFIG should ACK"
    
    def test_servo_config_with_speed(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test servo configuration with speed limit."""
        success, _ = fresh_gunfx_binary.servo_config(
            servo_id=1,
            min_us=1000,
            max_us=2000,
            max_speed=500
        )
        assert success
    
    def test_servo_config_full(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test servo configuration with all parameters."""
        success, _ = fresh_gunfx_binary.servo_config(
            servo_id=1,
            min_us=1000,
            max_us=2000,
            max_speed=500,
            max_accel=1000,
            max_decel=1000
        )
        assert success
    
    def test_servo_config_all_channels(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test configuration on all channels."""
        for servo_id in range(1, 4):
            success, _ = fresh_gunfx_binary.servo_config(servo_id, 1000, 2000)
            assert success, f"SERVO_CONFIG id={servo_id} should ACK"


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.servo
class TestServoRecoilJerk:
    """Test SERVO_RECOIL_JERK command."""
    
    def test_recoil_jerk_basic(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test basic recoil jerk configuration."""
        success, _ = fresh_gunfx_binary.servo_recoil_jerk(1, 50, 10)
        assert success, "SERVO_RECOIL_JERK should ACK"
    
    def test_recoil_jerk_large(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test larger recoil jerk values."""
        success, _ = fresh_gunfx_binary.servo_recoil_jerk(1, 200, 50)
        assert success
    
    def test_recoil_jerk_zero_variance(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test recoil jerk with zero variance (consistent jerk)."""
        success, _ = fresh_gunfx_binary.servo_recoil_jerk(1, 100, 0)
        assert success
    
    def test_recoil_jerk_disabled(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test disabling recoil jerk."""
        success, _ = fresh_gunfx_binary.servo_recoil_jerk(1, 0, 0)
        assert success


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.servo
class TestServoErrors:
    """Test servo error conditions."""
    
    def test_servo_invalid_id_zero(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with id=0 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 0, 1500)
        )
        assert is_nack, "Should NACK with id=0"
        assert error_code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_invalid_id_high(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with id=4 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 4, 1500)
        )
        assert is_nack, "Should NACK with id=4"
        assert error_code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_pulse_too_low(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with pulseUs < 500 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 1, 400)
        )
        assert is_nack, "Should NACK with pulse < 500"
        assert error_code == GunFxError.SERVO_PULSE_RANGE
    
    def test_servo_pulse_too_high(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with pulseUs > 2500 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 1, 2600)
        )
        assert is_nack, "Should NACK with pulse > 2500"
        assert error_code == GunFxError.SERVO_PULSE_RANGE
    
    def test_servo_config_invalid_id(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_CONFIG with invalid id returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SETTINGS, struct.pack('<BHHHHH', 5, 1000, 2000, 0, 0, 0)
        )
        assert is_nack, "Should NACK with id=5"
        assert error_code == GunFxError.SERVO_INVALID_ID
    
    def test_recoil_jerk_invalid_id(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_RECOIL_JERK with invalid id returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_RECOIL_JERK, struct.pack('<BHH', 0, 50, 10)
        )
        assert is_nack, "Should NACK with id=0"
        assert error_code == GunFxError.SERVO_INVALID_ID
