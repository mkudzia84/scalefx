"""
GunFX System Command Tests

Tests for core system commands: INIT, SHUTDOWN, KEEPALIVE, STATUS, REBOOT, BOOTSEL.
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, GunFxCommands, CommandBuilder,
    CorePacket, CoreError
)


@pytest.mark.hardware
@pytest.mark.gunfx
class TestGunFxSystem:
    """System command tests for GunFX controller."""
    
    def test_init_returns_init_ready(self, fresh_connection: ScaleFXConnection):
        """INIT command should return INIT_READY with device info."""
        # Fresh connection already sends INIT, but let's test explicitly
        conn = ScaleFXConnection(port=fresh_connection.port)
        try:
            assert conn.connect(init=False), "Failed to open port"
            
            response = conn.send_and_wait(CommandBuilder.init())
            assert response is not None, "No response received"
            assert response.is_init_ready, f"Expected INIT_READY, got 0x{response.packet_type:02X}"
            
            # Payload should contain device info
            assert len(response.payload) > 0, "Empty payload"
        finally:
            conn.close()
    
    def test_keepalive_returns_ack(self, gunfx: ScaleFXConnection):
        """KEEPALIVE command should return ACK."""
        success, response = gunfx.send_expect_ack(CommandBuilder.keepalive())
        assert success, "KEEPALIVE failed"
        assert response.is_ack, "Expected ACK"
    
    def test_status_request(self, gunfx: ScaleFXConnection):
        """STATUS_REQ should return STATUS packet."""
        response = gunfx.send_and_wait(CommandBuilder.status_req())
        assert response is not None, "No response"
        assert response.packet_type == CorePacket.STATUS, "Expected STATUS"
    
    def test_shutdown_returns_ack(self, fresh_connection: ScaleFXConnection):
        """SHUTDOWN command should return ACK."""
        success, response = fresh_connection.send_expect_ack(CommandBuilder.shutdown())
        assert success, "SHUTDOWN failed"
    
    def test_commands_before_init_rejected(self):
        """Commands before INIT should be rejected."""
        import os
        port = os.environ.get("SCALEFX_PORT", "COM3")
        conn = ScaleFXConnection(port=port)
        try:
            if not conn.connect(init=False):
                pytest.skip("Could not connect")
            
            # Try sending a command without INIT
            response = conn.send_and_wait(GunFxCommands.trigger_on(600))
            
            # Should get NACK with NOT_INITIALIZED
            if response:
                assert response.is_nack, "Expected NACK"
                assert response.error_code == CoreError.NOT_INITIALIZED
        finally:
            conn.close()
    
    @pytest.mark.destructive
    def test_reboot(self, fresh_connection: ScaleFXConnection):
        """REBOOT command should reboot device (no response expected)."""
        # Send reboot - no response expected
        fresh_connection.send(CommandBuilder.reboot())
        
        # Wait for device to reboot
        time.sleep(2.0)
        
        # Try to reconnect
        fresh_connection.close()
        time.sleep(0.5)
        assert fresh_connection.connect(), "Failed to reconnect after reboot"
    
    @pytest.mark.destructive
    def test_bootsel(self, fresh_connection: ScaleFXConnection):
        """BOOTSEL command should enter bootloader (device becomes mass storage)."""
        pytest.skip("BOOTSEL test requires manual verification")
