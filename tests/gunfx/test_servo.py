"""
GunFX Servo Command Tests

Tests for SERVO_SET, SERVO_SETTINGS, and SERVO_RECOIL commands.
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, GunFxCommands,
    GunFxError
)


@pytest.mark.hardware
@pytest.mark.gunfx
class TestGunFxServo:
    """Servo command tests."""
    
    def test_servo_set_center(self, gunfx: ScaleFXConnection):
        """SERVO_SET to center position (1500µs)."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(1, 1500))
        assert success, f"SERVO_SET failed: {response}"
    
    def test_servo_set_min(self, gunfx: ScaleFXConnection):
        """SERVO_SET to minimum position (500µs)."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(1, 500))
        assert success, "SERVO_SET to min failed"
    
    def test_servo_set_max(self, gunfx: ScaleFXConnection):
        """SERVO_SET to maximum position (2500µs)."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(1, 2500))
        assert success, "SERVO_SET to max failed"
    
    def test_servo_set_all_servos(self, gunfx: ScaleFXConnection):
        """SERVO_SET for all three servos."""
        for servo_id in [1, 2, 3]:
            success, response = gunfx.send_expect_ack(
                GunFxCommands.servo_set(servo_id, 1500)
            )
            assert success, f"SERVO_SET for servo {servo_id} failed"
    
    def test_servo_set_invalid_id_zero(self, gunfx: ScaleFXConnection):
        """SERVO_SET with ID=0 should fail."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(0, 1500))
        assert not success, "Should reject servo ID 0"
        assert response.error_code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_set_invalid_id_over(self, gunfx: ScaleFXConnection):
        """SERVO_SET with ID>3 should fail."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(4, 1500))
        assert not success, "Should reject servo ID > 3"
        assert response.error_code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_set_pulse_under_range(self, gunfx: ScaleFXConnection):
        """SERVO_SET with pulse<500 should fail."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(1, 400))
        assert not success, "Should reject pulse < 500"
        assert response.error_code == GunFxError.SERVO_PULSE_RANGE
    
    def test_servo_set_pulse_over_range(self, gunfx: ScaleFXConnection):
        """SERVO_SET with pulse>2500 should fail."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(1, 2600))
        assert not success, "Should reject pulse > 2500"
        assert response.error_code == GunFxError.SERVO_PULSE_RANGE
    
    def test_servo_detach(self, gunfx: ScaleFXConnection):
        """SERVO_SET with pulse=-1 should detach servo."""
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(1, -1))
        assert success, "SERVO_SET detach failed"
    
    def test_servo_settings(self, gunfx: ScaleFXConnection):
        """SERVO_SETTINGS should configure servo limits."""
        success, response = gunfx.send_expect_ack(
            GunFxCommands.servo_settings(1, 1000, 2000, 4000, 8000, 8000)
        )
        assert success, "SERVO_SETTINGS failed"
    
    def test_servo_settings_invalid_min_max(self, gunfx: ScaleFXConnection):
        """SERVO_SETTINGS with min>=max should fail."""
        success, response = gunfx.send_expect_ack(
            GunFxCommands.servo_settings(1, 2000, 1000, 4000, 8000, 8000)
        )
        assert not success, "Should reject min >= max"
        assert response.error_code == GunFxError.SERVO_MIN_MAX
    
    def test_servo_recoil(self, gunfx: ScaleFXConnection):
        """SERVO_RECOIL should configure recoil effect."""
        success, response = gunfx.send_expect_ack(
            GunFxCommands.servo_recoil(1, 50, 25)
        )
        assert success, "SERVO_RECOIL failed"
    
    def test_servo_motion_profile(self, gunfx: ScaleFXConnection):
        """Test servo smooth motion between positions."""
        # Move to start
        gunfx.send_expect_ack(GunFxCommands.servo_set(1, 1000))
        time.sleep(0.5)
        
        # Move to end
        gunfx.send_expect_ack(GunFxCommands.servo_set(1, 2000))
        time.sleep(0.5)
        
        # Back to center
        success, response = gunfx.send_expect_ack(GunFxCommands.servo_set(1, 1500))
        assert success
