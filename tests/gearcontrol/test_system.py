"""
GearControl System Tests

Tests core system commands against a GearControl controller:
- INIT / INIT_READY handshake
- STATUS request/response
- REBOOT

Requires: GearControl Pico connected via USB serial.
"""

import pytest
from tests.framework import (
    ScaleFXConnection, CorePacket,
    GearControlPacket, GearControlCommands,
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
# INIT
# =============================================================================

class TestInit:
    """Verify INIT handshake with GearControl controller."""

    def test_init_ready_received(self, conn):
        """INIT should return INIT_READY with controller info."""
        assert conn.is_initialized
        assert conn.device_name is not None
        assert 'gear' in conn.device_name.lower()

    def test_init_ready_has_version(self, conn):
        """INIT_READY should include firmware version."""
        assert conn.device_version is not None
        assert len(conn.device_version) > 0


# =============================================================================
# STATUS
# =============================================================================

class TestStatus:
    """Verify STATUS response from GearControl controller."""

    def test_status_returns_payload(self, conn):
        """STATUS should return core header + 26-byte module data."""
        from tests.framework.commands import CoreCommands
        packet = CoreCommands.status_request()
        success, response = conn.send_and_receive(packet)
        assert success
        assert response is not None
        # Core header (12) + GearControl module data (26) = 38 bytes
        assert len(response.payload) >= 38, (
            f"Expected ≥38 bytes, got {len(response.payload)}"
        )

    def test_status_gear_states_valid(self, conn):
        """All three gear states should be valid GearState values (0-5)."""
        from tests.framework.commands import CoreCommands
        packet = CoreCommands.status_request()
        success, response = conn.send_and_receive(packet)
        assert success
        module_data = response.payload[12:]
        for i in range(3):
            state = module_data[i * 7]
            assert state <= 5, f"Gear {i} state {state} out of range"
