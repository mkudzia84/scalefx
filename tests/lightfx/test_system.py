"""
LightFX System Command Tests
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, LightFxCommands, CommandBuilder,
    CorePacket, CoreError
)


@pytest.mark.hardware
@pytest.mark.lightfx
class TestLightFxSystem:
    """System command tests for LightFX controller."""
    
    def test_init_returns_init_ready(self, fresh_connection: ScaleFXConnection):
        """INIT command should return INIT_READY with device info."""
        conn = ScaleFXConnection(port=fresh_connection.port)
        try:
            assert conn.connect(init=False), "Failed to open port"
            
            response = conn.send_and_wait(CommandBuilder.init())
            assert response is not None, "No response received"
            assert response.is_init_ready, f"Expected INIT_READY, got 0x{response.packet_type:02X}"
            assert len(response.payload) > 0, "Empty payload"
        finally:
            conn.close()
    
    def test_keepalive_returns_ack(self, lightfx: ScaleFXConnection):
        """KEEPALIVE command should return ACK."""
        success, response = lightfx.send_expect_ack(CommandBuilder.keepalive())
        assert success, "KEEPALIVE failed"
    
    def test_status_request(self, lightfx: ScaleFXConnection):
        """STATUS_REQ should return STATUS packet."""
        response = lightfx.send_and_wait(CommandBuilder.status_req())
        assert response is not None, "No response"
        assert response.packet_type == CorePacket.STATUS, "Expected STATUS"
    
    def test_shutdown_returns_ack(self, fresh_connection: ScaleFXConnection):
        """SHUTDOWN command should return ACK."""
        success, response = fresh_connection.send_expect_ack(CommandBuilder.shutdown())
        assert success, "SHUTDOWN failed"
    
    @pytest.mark.destructive
    def test_reboot(self, fresh_connection: ScaleFXConnection):
        """REBOOT command should reboot device."""
        fresh_connection.send(CommandBuilder.reboot())
        time.sleep(2.0)
        fresh_connection.close()
        time.sleep(0.5)
        assert fresh_connection.connect(), "Failed to reconnect after reboot"
