"""
GearControl Gear Control Tests

Tests gear deploy/retract/stop commands:
- Single gear operations
- All-gear operations
- Gear configuration
- Door configuration

Requires: GearControl Pico connected via USB serial.
"""

import pytest
import time
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
    # Stop all motors on teardown
    try:
        c.send_expect_ack(GearControlCommands.gear_all(2))  # ACTION_STOP
    except Exception:
        pass
    c.disconnect()


# =============================================================================
# Gear Deploy / Retract / Stop
# =============================================================================

class TestGearControl:
    """Test gear deploy, retract, and stop commands."""

    @pytest.mark.parametrize("gear_id", [0, 1, 2])
    def test_gear_deploy_ack(self, conn, gear_id):
        """GEAR_DEPLOY should ACK for valid gear IDs."""
        packet = GearControlCommands.gear_deploy(gear_id)
        success, _ = conn.send_expect_ack(packet)
        assert success
        # Stop after test
        conn.send_expect_ack(GearControlCommands.gear_stop(gear_id))

    @pytest.mark.parametrize("gear_id", [0, 1, 2])
    def test_gear_retract_ack(self, conn, gear_id):
        """GEAR_RETRACT should ACK for valid gear IDs."""
        packet = GearControlCommands.gear_retract(gear_id)
        success, _ = conn.send_expect_ack(packet)
        assert success
        conn.send_expect_ack(GearControlCommands.gear_stop(gear_id))

    @pytest.mark.parametrize("gear_id", [0, 1, 2])
    def test_gear_stop_ack(self, conn, gear_id):
        """GEAR_STOP should ACK for valid gear IDs."""
        packet = GearControlCommands.gear_stop(gear_id)
        success, _ = conn.send_expect_ack(packet)
        assert success

    def test_gear_deploy_invalid_id_nack(self, conn):
        """GEAR_DEPLOY with invalid ID should NACK."""
        packet = GearControlCommands.gear_deploy(3)
        success, response = conn.send_expect_ack(packet)
        assert not success

    def test_gear_all_deploy(self, conn):
        """GEAR_ALL with action=1 (deploy) should ACK."""
        packet = GearControlCommands.gear_all(1)  # Deploy
        success, _ = conn.send_expect_ack(packet)
        assert success
        conn.send_expect_ack(GearControlCommands.gear_all(2))  # Stop

    def test_gear_all_retract(self, conn):
        """GEAR_ALL with action=0 (retract) should ACK."""
        packet = GearControlCommands.gear_all(0)  # Retract
        success, _ = conn.send_expect_ack(packet)
        assert success
        conn.send_expect_ack(GearControlCommands.gear_all(2))  # Stop

    def test_gear_all_stop(self, conn):
        """GEAR_ALL with action=2 (stop) should ACK."""
        packet = GearControlCommands.gear_all(2)  # Stop
        success, _ = conn.send_expect_ack(packet)
        assert success


# =============================================================================
# Gear Configuration
# =============================================================================

class TestGearConfig:
    """Test gear configuration commands."""

    @pytest.mark.parametrize("gear_id", [0, 1, 2])
    def test_gear_config_ack(self, conn, gear_id):
        """GEAR_CONFIG should ACK with valid parameters."""
        packet = GearControlCommands.gear_config(
            gear_id, flags=0x03, stall_current_mA=1500, timeout_ms=10000
        )
        success, _ = conn.send_expect_ack(packet)
        assert success

    def test_gear_config_invalid_id(self, conn):
        """GEAR_CONFIG with invalid gear ID should NACK."""
        packet = GearControlCommands.gear_config(3, 0x01, 1500, 10000)
        success, _ = conn.send_expect_ack(packet)
        assert not success


# =============================================================================
# Door Configuration
# =============================================================================

class TestDoorConfig:
    """Test door servo configuration."""

    @pytest.mark.parametrize("gear_id", [0, 1, 2])
    def test_door_config_ack(self, conn, gear_id):
        """DOOR_CONFIG should ACK with valid parameters."""
        packet = GearControlCommands.door_config(
            gear_id, open0_us=2000, close0_us=1000,
            open1_us=2000, close1_us=1000
        )
        success, _ = conn.send_expect_ack(packet)
        assert success

    def test_door_config_invalid_id(self, conn):
        """DOOR_CONFIG with invalid gear ID should NACK."""
        packet = GearControlCommands.door_config(3, 2000, 1000, 2000, 1000)
        success, _ = conn.send_expect_ack(packet)
        assert not success
