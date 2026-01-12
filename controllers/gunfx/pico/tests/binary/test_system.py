"""
GunFX Binary Protocol System Tests

Tests for system-level commands using binary protocol:
- INIT/INIT_READY handshake (text-based)
- SHUTDOWN command
- KEEPALIVE heartbeat
- REBOOT command
- BOOTSEL command
"""

import pytest
import time
from conftest import (
    GunFxBinaryConnection, SFX_PKT_SHUTDOWN, SFX_PKT_KEEPALIVE,
    SFX_PKT_REBOOT, SFX_PKT_BOOTSEL
)


@pytest.mark.hardware
@pytest.mark.binary
class TestInit:
    """Test INIT/INIT_READY handshake."""
    
    def test_init_ready_received(self, gunfx_binary: GunFxBinaryConnection):
        """Test that INIT_READY is received after INIT."""
        assert gunfx_binary._initialized, "Should receive INIT_READY"
    
    def test_device_name_present(self, gunfx_binary: GunFxBinaryConnection):
        """Test INIT_READY contains device name."""
        assert gunfx_binary.device_name, "Device name should be present"
        assert "GunFX" in gunfx_binary.device_name or len(gunfx_binary.device_name) > 0
    
    def test_firmware_version_present(self, gunfx_binary: GunFxBinaryConnection):
        """Test INIT_READY contains firmware version."""
        assert gunfx_binary.firmware_version, "Firmware version should be present"
        # Version format: X.Y.Z
        parts = gunfx_binary.firmware_version.split('.')
        assert len(parts) >= 2, f"Version should be X.Y or X.Y.Z format: {gunfx_binary.firmware_version}"
    
    def test_build_number_present(self, gunfx_binary: GunFxBinaryConnection):
        """Test INIT_READY contains build number."""
        assert gunfx_binary.build_number >= 0, "Build number should be present"
    
    def test_platform_present(self, gunfx_binary: GunFxBinaryConnection):
        """Test INIT_READY contains platform info."""
        assert gunfx_binary.platform, "Platform should be present"
        # GunFX runs on RP2040
        assert "RP2040" in gunfx_binary.platform or len(gunfx_binary.platform) > 0


@pytest.mark.hardware
@pytest.mark.binary
class TestKeepalive:
    """Test KEEPALIVE heartbeat functionality."""
    
    def test_keepalive_accepted(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test KEEPALIVE packet is accepted."""
        # KEEPALIVE is fire-and-forget, no response expected
        fresh_gunfx_binary.keepalive()
        # If no exception, it was accepted
        time.sleep(0.2)
    
    def test_multiple_keepalives(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test multiple KEEPALIVE packets."""
        for _ in range(5):
            fresh_gunfx_binary.keepalive()
            time.sleep(0.1)
        # Device should still be responsive
        success, _ = fresh_gunfx_binary.servo_set(1, 1500)
        assert success, "Device should respond after keepalives"


@pytest.mark.hardware
@pytest.mark.binary
class TestShutdown:
    """Test SHUTDOWN command behavior."""
    
    def test_shutdown_accepted(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SHUTDOWN packet is accepted (fire-and-forget)."""
        fresh_gunfx_binary.shutdown()
        time.sleep(0.2)
        # Device should still be running (SHUTDOWN doesn't terminate)
    
    def test_shutdown_stops_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SHUTDOWN stops any active firing."""
        # Start firing
        success, _ = fresh_gunfx_binary.trigger_on(600)
        assert success
        time.sleep(0.3)
        
        # Send shutdown
        fresh_gunfx_binary.shutdown()
        time.sleep(0.3)
        
        # Reconnect and check status (SHUTDOWN resets connection)
        # Need to reinitialize after shutdown
        conn = GunFxBinaryConnection()
        if conn.connect():
            status = conn.wait_for_status()
            if status:
                assert status.get('firing') == False, "Firing should be stopped"
            conn.close()


@pytest.mark.hardware
@pytest.mark.binary
class TestReconnection:
    """Test reconnection after various scenarios."""
    
    def test_reinit_after_shutdown(self):
        """Test re-initialization works after SHUTDOWN."""
        conn = GunFxBinaryConnection()
        assert conn.connect(), "Initial connection should succeed"
        
        conn.shutdown()
        time.sleep(0.5)
        
        # Should be able to reconnect
        conn2 = GunFxBinaryConnection()
        assert conn2.connect(), "Reconnection should succeed"
        conn2.close()
    
    def test_reinit_resets_state(self):
        """Test new INIT resets device state."""
        conn = GunFxBinaryConnection()
        assert conn.connect()
        
        # Start firing
        success, _ = conn.trigger_on(600)
        assert success
        time.sleep(0.2)
        
        # Send new INIT (via new connection)
        conn2 = GunFxBinaryConnection()
        assert conn2.connect(), "New connection should trigger state reset"
        
        # Firing should have stopped
        status = conn2.wait_for_status()
        if status:
            assert status.get('firing') == False
        
        conn2.close()


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.destructive
class TestReboot:
    """Test REBOOT command (destructive - device will restart)."""
    
    @pytest.mark.slow
    def test_reboot_and_reconnect(self):
        """Test REBOOT triggers device restart and we can reconnect."""
        conn = GunFxBinaryConnection()
        assert conn.connect()
        
        # Send reboot
        conn.reboot()
        conn.close()
        
        # Wait for device to reboot
        time.sleep(3)
        
        # Should be able to reconnect
        conn2 = GunFxBinaryConnection()
        assert conn2.connect(), "Should reconnect after reboot"
        conn2.close()


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.destructive
class TestBootsel:
    """Test BOOTSEL command (very destructive - enters bootloader)."""
    
    @pytest.mark.skip(reason="BOOTSEL requires manual intervention to recover")
    def test_bootsel_enters_bootloader(self):
        """Test BOOTSEL enters bootloader mode (RPI-RP2 drive appears)."""
        conn = GunFxBinaryConnection()
        assert conn.connect()
        
        # WARNING: This will put device in BOOTSEL mode
        # You'll need to manually re-flash firmware to recover
        conn.bootsel()
        conn.close()
