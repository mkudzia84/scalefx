"""
GearControl Servo Tests

Tests servo position and configuration commands:
- Servo position setting (0-7)
- Servo settings (limits, speed, accel, decel - SRV_SETTINGS pattern)
- Yaw configuration and input
- Boundary / invalid parameter handling

Requires: GearControl Pico connected via USB serial.
"""

import pytest
from tests.framework import (
    ScaleFXConnection, CorePacket,
    GearControlPacket, GearControlError, GearControlCommands,
)


@pytest.fixture
def conn(request):
    """Connect and initialize GearControl controller."""
    port = request.config.getoption("--port")
    c = ScaleFXConnection(port=port)
    assert c.connect(init=True), "Failed to connect and init GearControl"
    yield c
    c.disconnect()


# =============================================================================
# Servo Position
# =============================================================================

class TestServoPosition:
    """Test servo position commands."""

    @pytest.mark.parametrize("servo_id", range(8))
    def test_servo_set_ack(self, conn, servo_id):
        """SERVO_SET should ACK for all valid servo IDs (0-7)."""
        packet = GearControlCommands.servo_set(servo_id, 1500)
        success, _ = conn.send_expect_ack(packet)
        assert success

    def test_servo_set_invalid_id(self, conn):
        """SERVO_SET with invalid ID should NACK."""
        packet = GearControlCommands.servo_set(8, 1500)
        success, _ = conn.send_expect_ack(packet)
        assert not success

    @pytest.mark.parametrize("pulse", [500, 1000, 1500, 2000, 2500])
    def test_servo_set_various_positions(self, conn, pulse):
        """SERVO_SET should accept common pulse widths."""
        packet = GearControlCommands.servo_set(0, pulse)
        success, _ = conn.send_expect_ack(packet)
        assert success


# =============================================================================
# Servo Configuration
# =============================================================================

class TestServoConfig:
    """Test servo settings commands (SRV_SETTINGS pattern)."""

    @pytest.mark.parametrize("servo_id", range(8))
    def test_servo_settings_ack(self, conn, servo_id):
        """SRV_SETTINGS should ACK for all valid servo IDs."""
        packet = GearControlCommands.servo_settings(servo_id, 500, 2500)
        success, _ = conn.send_expect_ack(packet)
        assert success

    def test_servo_settings_invalid_id(self, conn):
        """SRV_SETTINGS with invalid servo ID should NACK."""
        packet = GearControlCommands.servo_settings(8, 500, 2500)
        success, _ = conn.send_expect_ack(packet)
        assert not success


# =============================================================================
# Yaw Control
# =============================================================================

class TestYawControl:
    """Test yaw configuration and input commands."""

    @pytest.mark.parametrize("gear_id", [0, 1, 2])
    def test_yaw_config_ack(self, conn, gear_id):
        """YAW_CONFIG should ACK for valid gear IDs."""
        packet = GearControlCommands.yaw_config(gear_id, 1500, 1000, 2000)
        success, _ = conn.send_expect_ack(packet)
        assert success

    def test_yaw_config_invalid_id(self, conn):
        """YAW_CONFIG with invalid gear ID should NACK."""
        packet = GearControlCommands.yaw_config(3, 1500, 1000, 2000)
        success, _ = conn.send_expect_ack(packet)
        assert not success

    @pytest.mark.parametrize("position", [1000, 1500, 2000])
    def test_yaw_input_ack(self, conn, position):
        """YAW_INPUT should ACK with valid positions."""
        packet = GearControlCommands.yaw_input(position)
        success, _ = conn.send_expect_ack(packet)
        assert success
