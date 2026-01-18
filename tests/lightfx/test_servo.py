"""
LightFX Servo Command Tests
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, LightFxCommands,
    LightFxError
)


@pytest.mark.hardware
@pytest.mark.lightfx
class TestLightFxServo:
    """Servo command tests."""
    
    def test_servo_set_center(self, lightfx: ScaleFXConnection):
        """SERVO_SET to center position."""
        success, response = lightfx.send_expect_ack(
            LightFxCommands.servo_set(1, 1500)
        )
        assert success, f"SERVO_SET failed: {response}"
    
    def test_servo_set_all(self, lightfx: ScaleFXConnection):
        """SERVO_SET for all three servos."""
        for servo_id in [1, 2, 3]:
            success, response = lightfx.send_expect_ack(
                LightFxCommands.servo_set(servo_id, 1500)
            )
            assert success, f"SERVO_SET for servo {servo_id} failed"
    
    def test_servo_set_range(self, lightfx: ScaleFXConnection):
        """SERVO_SET across full range."""
        for pulse in [500, 1000, 1500, 2000, 2500]:
            success, response = lightfx.send_expect_ack(
                LightFxCommands.servo_set(1, pulse)
            )
            assert success, f"SERVO_SET to {pulse}µs failed"
            time.sleep(0.2)
    
    def test_servo_set_invalid_id(self, lightfx: ScaleFXConnection):
        """SERVO_SET with invalid ID should fail."""
        success, response = lightfx.send_expect_ack(
            LightFxCommands.servo_set(4, 1500)
        )
        assert not success, "Should reject servo ID > 3"
        assert response.error_code == LightFxError.INVALID_SERVO
    
    def test_servo_settings(self, lightfx: ScaleFXConnection):
        """SERVO_SETTINGS configuration."""
        success, response = lightfx.send_expect_ack(
            LightFxCommands.servo_settings(1, 1000, 2000, 4000, 8000, 8000)
        )
        assert success, "SERVO_SETTINGS failed"
    
    def test_servo_detach(self, lightfx: ScaleFXConnection):
        """SERVO_SET with -1 should detach."""
        success, response = lightfx.send_expect_ack(
            LightFxCommands.servo_set(1, -1)
        )
        assert success, "SERVO_SET detach failed"
