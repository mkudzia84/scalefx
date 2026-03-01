"""
NoOp System Command Tests

Tests for core system commands on the minimal NoOp controller.

Fixture Usage:
- noop: Shared module-scoped connection (for most tests)
- exclusive_connection: Fresh connection that temporarily closes shared one
- serial_port: Just the port name, for manual connection handling
"""

import pytest
import time
import struct

from tests.framework import (
    ScaleFXConnection, CommandBuilder,
    CorePacket, CoreError
)


@pytest.mark.hardware
@pytest.mark.noop
class TestNoOpSystem:
    """System command tests for NoOp controller."""
    
    def test_init_returns_init_ready(self, exclusive_connection: ScaleFXConnection):
        """INIT command should return INIT_READY."""
        conn = exclusive_connection
        
        response = conn.send_and_wait(CommandBuilder.init())
        assert response is not None, "No response received"
        assert response.is_init_ready, f"Expected INIT_READY, got 0x{response.packet_type:02X}"
        
        # Verify payload contains device info
        assert len(response.payload) > 0, "Empty payload"
    
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
    
    def test_status_returns_counter(self, noop: ScaleFXConnection):
        """STATUS should return command counter in payload."""
        response = noop.send_and_wait(CommandBuilder.status_req())
        assert response is not None, "No response"
        assert response.packet_type == CorePacket.STATUS
        
        # Payload should be 4 bytes (uint32 little-endian counter)
        assert len(response.payload) >= 4, f"Expected 4+ bytes, got {len(response.payload)}"
        
        counter = struct.unpack('<I', response.payload[:4])[0]
        assert counter > 0, "Counter should be > 0 after commands"
    
    def test_status_increments_counter(self, noop: ScaleFXConnection):
        """Each command should increment counter."""
        # Get initial status
        response1 = noop.send_and_wait(CommandBuilder.status_req())
        assert response1 is not None
        assert response1.packet_type == CorePacket.STATUS
        counter1 = struct.unpack('<I', response1.payload[:4])[0]
        
        # Get second status
        response2 = noop.send_and_wait(CommandBuilder.status_req())
        assert response2 is not None
        assert response2.packet_type == CorePacket.STATUS
        counter2 = struct.unpack('<I', response2.payload[:4])[0]
        
        # Counter should have incremented
        assert counter2 > counter1, f"Counter didn't increment: {counter1} -> {counter2}"
    
    def test_shutdown_returns_ack(self, exclusive_connection: ScaleFXConnection):
        """SHUTDOWN command should return ACK."""
        conn = exclusive_connection
        
        # First init
        response = conn.send_and_wait(CommandBuilder.init())
        assert response is not None and response.is_init_ready, "INIT failed"
        
        # Then shutdown
        success, response = conn.send_expect_ack(CommandBuilder.shutdown())
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
            time.sleep(0.05)
    
    @pytest.mark.slow
    def test_connection_timeout(self, exclusive_connection: ScaleFXConnection):
        """Connection should timeout after inactivity."""
        pytest.skip("Timeout test requires long wait - run manually")
    
    @pytest.mark.destructive
    def test_reboot(self, exclusive_connection: ScaleFXConnection):
        """REBOOT command should reboot device."""
        conn = exclusive_connection
        
        # Init first
        response = conn.send_and_wait(CommandBuilder.init())
        assert response is not None and response.is_init_ready, "INIT failed"
        
        # Send reboot (no response expected - device reboots immediately)
        conn.send(CommandBuilder.reboot())
        time.sleep(2.0)
        
        conn.close()
        time.sleep(0.5)
        
        # Should be able to reconnect
        assert conn.connect(), "Failed to reconnect after reboot"
    
    @pytest.mark.destructive
    def test_bootsel(self, exclusive_connection: ScaleFXConnection):
        """BOOTSEL command should enter bootloader."""
        pytest.skip("BOOTSEL test requires manual verification")
