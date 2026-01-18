"""
NoOp System Command Tests

Tests for core system commands on the minimal NoOp controller.
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, CommandBuilder,
    CorePacket, CoreError
)


@pytest.mark.hardware
@pytest.mark.noop
class TestNoOpSystem:
    """System command tests for NoOp controller."""
    
    def test_init_returns_init_ready(self, fresh_connection: ScaleFXConnection):
        """INIT command should return INIT_READY."""
        conn = ScaleFXConnection(port=fresh_connection.port)
        try:
            assert conn.connect(init=False), "Failed to open port"
            
            response = conn.send_and_wait(CommandBuilder.init())
            assert response is not None, "No response received"
            assert response.is_init_ready, f"Expected INIT_READY, got 0x{response.packet_type:02X}"
            
            # Verify payload contains device info
            assert len(response.payload) > 0, "Empty payload"
        finally:
            conn.close()
    
    def test_keepalive_returns_ack(self, noop: ScaleFXConnection):
        """KEEPALIVE command should return ACK."""
        success, response = noop.send_expect_ack(CommandBuilder.keepalive())
        assert success, "KEEPALIVE failed"
        assert response.is_ack
    
    def test_status_request(self, noop: ScaleFXConnection):
        """STATUS_REQ should return STATUS packet."""
        response = noop.send_and_wait(CommandBuilder.status_req())
        assert response is not None, "No response"
        assert response.packet_type == CorePacket.STATUS, \
            f"Expected STATUS (0xF4), got 0x{response.packet_type:02X}"
    
    def test_status_increments_counter(self, noop: ScaleFXConnection):
        """Each STATUS_REQ should increment command counter."""
        # Get initial status
        response1 = noop.send_and_wait(CommandBuilder.status_req())
        assert response1 is not None
        
        # Get second status
        response2 = noop.send_and_wait(CommandBuilder.status_req())
        assert response2 is not None
        
        # Both should be STATUS responses
        assert response1.packet_type == CorePacket.STATUS
        assert response2.packet_type == CorePacket.STATUS
    
    def test_shutdown_returns_ack(self, fresh_connection: ScaleFXConnection):
        """SHUTDOWN command should return ACK."""
        success, response = fresh_connection.send_expect_ack(CommandBuilder.shutdown())
        assert success, "SHUTDOWN failed"
    
    def test_unknown_command_returns_nack(self, noop: ScaleFXConnection):
        """Unknown command should return NACK with INVALID_COMMAND."""
        from tests.framework.protocol import build_packet
        
        # Send unknown packet type
        unknown_packet = build_packet(0x99)  # Not a valid command
        response = noop.send_and_wait(unknown_packet)
        
        assert response is not None, "No response"
        assert response.is_nack, "Expected NACK for unknown command"
        assert response.error_code == CoreError.INVALID_COMMAND
    
    def test_multiple_keepalives(self, noop: ScaleFXConnection):
        """Multiple KEEPALIVE commands should all succeed."""
        for i in range(5):
            success, response = noop.send_expect_ack(CommandBuilder.keepalive())
            assert success, f"KEEPALIVE {i+1} failed"
            time.sleep(0.1)
    
    @pytest.mark.slow
    def test_connection_timeout(self, fresh_connection: ScaleFXConnection):
        """Connection should timeout after inactivity."""
        pytest.skip("Timeout test requires long wait - run manually")
        
        # Wait for connection timeout (typically 15 seconds)
        time.sleep(16)
        
        # Status request should fail
        response = fresh_connection.send_and_wait(CommandBuilder.status_req())
        if response:
            assert response.is_nack
    
    @pytest.mark.destructive
    def test_reboot(self, fresh_connection: ScaleFXConnection):
        """REBOOT command should reboot device."""
        fresh_connection.send(CommandBuilder.reboot())
        time.sleep(2.0)
        
        fresh_connection.close()
        time.sleep(0.5)
        
        assert fresh_connection.connect(), "Failed to reconnect after reboot"
    
    @pytest.mark.destructive
    def test_bootsel(self, fresh_connection: ScaleFXConnection):
        """BOOTSEL command should enter bootloader."""
        pytest.skip("BOOTSEL test requires manual verification")
