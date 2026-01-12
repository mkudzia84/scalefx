"""
GunFX Servo Tests

Tests for SERVO_SET, SERVO_CONFIG, and SERVO_RECOIL_JERK commands.
Controls 3 gun servos with motion profiling.
"""

import pytest
import time
from conftest import GunFxConnection


class TestServoSet:
    """Test SERVO_SET command."""
    
    @pytest.mark.servo
    def test_servo_set_center(self, fresh_gunfx: GunFxConnection):
        """Test setting servo to center position (1500µs)."""
        for servo_id in [1, 2, 3]:
            success, response = fresh_gunfx.send_and_expect_ack(
                f"SERVO_SET id={servo_id} pulseUs=1500"
            )
            assert success, f"SERVO_SET id={servo_id} pulseUs=1500 should ACK, got: {response}"
    
    @pytest.mark.servo
    def test_servo_set_min(self, fresh_gunfx: GunFxConnection):
        """Test setting servo to minimum position (500µs)."""
        success, response = fresh_gunfx.send_and_expect_ack("SERVO_SET id=1 pulseUs=500")
        assert success, f"SERVO_SET pulseUs=500 should ACK, got: {response}"
        
        # Reset to center
        fresh_gunfx.send_command("SERVO_SET id=1 pulseUs=1500")
    
    @pytest.mark.servo
    def test_servo_set_max(self, fresh_gunfx: GunFxConnection):
        """Test setting servo to maximum position (2500µs)."""
        success, response = fresh_gunfx.send_and_expect_ack("SERVO_SET id=1 pulseUs=2500")
        assert success, f"SERVO_SET pulseUs=2500 should ACK, got: {response}"
        
        # Reset to center
        fresh_gunfx.send_command("SERVO_SET id=1 pulseUs=1500")
    
    @pytest.mark.servo
    def test_servo_set_all_servos(self, fresh_gunfx: GunFxConnection):
        """Test setting all three servos independently."""
        positions = [(1, 1200), (2, 1500), (3, 1800)]
        
        for servo_id, pulse_us in positions:
            success, response = fresh_gunfx.send_and_expect_ack(
                f"SERVO_SET id={servo_id} pulseUs={pulse_us}"
            )
            assert success, f"SERVO_SET id={servo_id} should ACK"
        
        # Verify positions in status
        time.sleep(0.3)
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        
        # Status uses servo0, servo1, servo2 keys
        assert status.get('servo0') == 1200 or abs(status.get('servo0', 0) - 1200) < 50
        assert status.get('servo1') == 1500 or abs(status.get('servo1', 0) - 1500) < 50
        assert status.get('servo2') == 1800 or abs(status.get('servo2', 0) - 1800) < 50
        
        # Reset to center
        for i in [1, 2, 3]:
            fresh_gunfx.send_command(f"SERVO_SET id={i} pulseUs=1500")
    
    def test_servo_set_invalid_id_zero(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with invalid servo ID (0) returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=0 pulseUs=1500")
        assert success, f"SERVO_SET id=0 should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == 0x20, f"Error code should be SERVO_INVALID_ID (0x20), got 0x{code:02x}"
    
    def test_servo_set_invalid_id_four(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with invalid servo ID (4) returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=4 pulseUs=1500")
        assert success, f"SERVO_SET id=4 should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == 0x20, f"Error code should be SERVO_INVALID_ID (0x20), got 0x{code:02x}"
    
    def test_servo_set_invalid_pulse_too_low(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with pulse < 500µs returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=1 pulseUs=499")
        assert success, f"SERVO_SET pulseUs=499 should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == 0x21, f"Error code should be SERVO_PULSE_RANGE (0x21), got 0x{code:02x}"
    
    def test_servo_set_invalid_pulse_too_high(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with pulse > 2500µs returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=1 pulseUs=2501")
        assert success, f"SERVO_SET pulseUs=2501 should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == 0x21, f"Error code should be SERVO_PULSE_RANGE (0x21), got 0x{code:02x}"


class TestServoConfig:
    """Test SERVO_CONFIG command."""
    
    @pytest.mark.servo
    def test_servo_config_limits(self, fresh_gunfx: GunFxConnection):
        """Test configuring servo pulse limits."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SERVO_CONFIG id=1 minUs=1000 maxUs=2000"
        )
        assert success, f"SERVO_CONFIG should ACK, got: {response}"
    
    @pytest.mark.servo
    def test_servo_config_motion_profile(self, fresh_gunfx: GunFxConnection):
        """Test configuring servo motion profile."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SERVO_CONFIG id=1 maxSpeedUsPerSec=500 maxAccelUsPerSec2=1000 maxDecelUsPerSec2=1000"
        )
        assert success, f"SERVO_CONFIG motion profile should ACK, got: {response}"
    
    @pytest.mark.servo
    def test_servo_config_full(self, fresh_gunfx: GunFxConnection):
        """Test configuring all servo parameters at once."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SERVO_CONFIG id=2 minUs=1000 maxUs=2000 maxSpeedUsPerSec=500 "
            "maxAccelUsPerSec2=1000 maxDecelUsPerSec2=1000"
        )
        assert success, f"Full SERVO_CONFIG should ACK, got: {response}"
    
    def test_servo_config_invalid_id(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_CONFIG with invalid servo ID returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack(
            "SERVO_CONFIG id=5 minUs=1000 maxUs=2000"
        )
        assert success, f"SERVO_CONFIG id=5 should NACK, got: {response}"


class TestServoRecoilJerk:
    """Test SERVO_RECOIL_JERK command."""
    
    @pytest.mark.servo
    def test_servo_recoil_jerk_basic(self, fresh_gunfx: GunFxConnection):
        """Test configuring recoil jerk."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SERVO_RECOIL_JERK id=1 jerkUs=50 varianceUs=10"
        )
        assert success, f"SERVO_RECOIL_JERK should ACK, got: {response}"
    
    @pytest.mark.servo
    def test_servo_recoil_jerk_all_servos(self, fresh_gunfx: GunFxConnection):
        """Test configuring recoil jerk for all servos."""
        jerk_configs = [
            (1, 30, 5),
            (2, 50, 15),
            (3, 20, 5),
        ]
        
        for servo_id, jerk_us, variance_us in jerk_configs:
            success, response = fresh_gunfx.send_and_expect_ack(
                f"SERVO_RECOIL_JERK id={servo_id} jerkUs={jerk_us} varianceUs={variance_us}"
            )
            assert success, f"SERVO_RECOIL_JERK id={servo_id} should ACK"
    
    @pytest.mark.servo
    def test_servo_recoil_jerk_disable(self, fresh_gunfx: GunFxConnection):
        """Test disabling recoil jerk by setting to zero."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SERVO_RECOIL_JERK id=1 jerkUs=0 varianceUs=0"
        )
        assert success, f"SERVO_RECOIL_JERK disable should ACK, got: {response}"
    
    def test_servo_recoil_jerk_invalid_id(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_RECOIL_JERK with invalid servo ID returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack(
            "SERVO_RECOIL_JERK id=0 jerkUs=50 varianceUs=10"
        )
        assert success, f"SERVO_RECOIL_JERK id=0 should NACK, got: {response}"


class TestServoWithFiring:
    """Test servo behavior during firing."""
    
    @pytest.mark.servo
    @pytest.mark.firing
    def test_servo_recoil_during_fire(self, fresh_gunfx: GunFxConnection):
        """Test servo recoil jerk is applied during firing."""
        # Configure recoil jerk
        fresh_gunfx.send_and_expect_ack("SERVO_RECOIL_JERK id=1 jerkUs=50 varianceUs=10")
        fresh_gunfx.send_command("SERVO_SET id=1 pulseUs=1500")
        time.sleep(0.2)
        
        # Start firing
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(1.0)  # Let it fire for a bit
        
        # Note: We can't directly measure recoil jerk via status,
        # but we verify the system doesn't error
        status = fresh_gunfx.wait_for_status()
        assert status is not None
        assert status.get('firing') == True
        
        # Clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        fresh_gunfx.send_command("SERVO_RECOIL_JERK id=1 jerkUs=0 varianceUs=0")
    
    @pytest.mark.servo
    @pytest.mark.firing
    def test_servo_position_during_fire(self, fresh_gunfx: GunFxConnection):
        """Test servo position can be changed while firing."""
        # Start firing
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        
        # Change servo position
        success, response = fresh_gunfx.send_and_expect_ack("SERVO_SET id=1 pulseUs=1200")
        assert success, "Should be able to set servo while firing"
        
        time.sleep(0.2)
        success, response = fresh_gunfx.send_and_expect_ack("SERVO_SET id=1 pulseUs=1800")
        assert success, "Should be able to set servo while firing"
        
        # Clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        fresh_gunfx.send_command("SERVO_SET id=1 pulseUs=1500")
