"""
GunFX System Tests

Tests for system-level commands: INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE.
These commands are handled by SerialInitHandler.
"""

import pytest
import time
from conftest import GunFxConnection


class TestInitialization:
    """Test INIT/INIT_READY handshake."""
    
    def test_device_initialized(self, gunfx: GunFxConnection):
        """Test that device responded to INIT with INIT_READY."""
        assert gunfx.is_initialized, "Device should be initialized after INIT"
    
    def test_device_name(self, gunfx: GunFxConnection):
        """Test device name is set correctly."""
        assert gunfx.device_name.startswith("GunFX"), f"Expected GunFX-*, got {gunfx.device_name}"
    
    def test_firmware_version(self, gunfx: GunFxConnection):
        """Test firmware version is present."""
        assert gunfx.firmware_version, "Firmware version should be present"
        # Version should be in semver format (without 'v' prefix)
        parts = gunfx.firmware_version.split('.')
        assert len(parts) >= 2, f"Version should be semver format, got {gunfx.firmware_version}"
    
    def test_build_number(self, gunfx: GunFxConnection):
        """Test build number is present."""
        assert gunfx.build_number >= 0, "Build number should be non-negative"
    
    def test_platform(self, gunfx: GunFxConnection):
        """Test platform is RP2040."""
        assert gunfx.platform == "RP2040", f"Expected RP2040, got {gunfx.platform}"


class TestKeepalive:
    """Test KEEPALIVE command."""
    
    def test_keepalive(self, fresh_gunfx: GunFxConnection):
        """Test KEEPALIVE command is accepted (no response expected)."""
        # KEEPALIVE should not error - it just resets the timeout timer
        response = fresh_gunfx.send_command("KEEPALIVE", delay=0.1)
        # No ACK expected for keepalive, but also no error
        assert "NACK" not in response, "KEEPALIVE should not return NACK"


class TestShutdown:
    """Test SHUTDOWN command."""
    
    @pytest.mark.destructive
    def test_shutdown_safe_state(self, fresh_gunfx: GunFxConnection):
        """Test SHUTDOWN puts device in safe state."""
        # First start some activity
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        
        # Send shutdown
        response = fresh_gunfx.send_command("SHUTDOWN", delay=0.5)
        # No ACK expected (fire-and-forget)
        
        # Wait for status and verify safe state
        time.sleep(0.5)
        status = fresh_gunfx.wait_for_status(timeout=2.0)
        
        if status:
            assert status.get('firing') == False, "Should not be firing after shutdown"
            assert status.get('heaterOn') == False, "Heater should be off after shutdown"
            assert status.get('fanOn') == False, "Fan should be off after shutdown"


class TestRebootBootsel:
    """Test REBOOT and BOOTSEL commands.
    
    NOTE: These tests will cause the device to reboot/enter bootloader,
    so they are marked as destructive and skipped by default.
    """
    
    @pytest.mark.skip(reason="Would cause device reboot - run manually if needed")
    @pytest.mark.destructive
    def test_reboot_command(self, fresh_gunfx: GunFxConnection):
        """Test REBOOT command triggers reboot."""
        # This will reboot the device!
        fresh_gunfx.send_command("REBOOT")
        # Device will disconnect after this
    
    @pytest.mark.skip(reason="Would cause device to enter bootloader - run manually if needed")
    @pytest.mark.destructive
    def test_bootsel_command(self, fresh_gunfx: GunFxConnection):
        """Test BOOTSEL command enters bootloader."""
        # This will enter BOOTSEL mode!
        fresh_gunfx.send_command("BOOTSEL")
        # Device will disconnect and RPI-RP2 drive will appear


class TestReconnection:
    """Test reconnection behavior."""
    
    def test_reinit(self, fresh_gunfx: GunFxConnection):
        """Test sending INIT again resets state."""
        # Start some activity
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        
        # Send INIT again
        fresh_gunfx.ser.write(b"INIT protocol=text\n")
        time.sleep(0.5)
        
        # Should get INIT_READY
        response = ""
        while fresh_gunfx.ser.in_waiting:
            response += fresh_gunfx.ser.read(fresh_gunfx.ser.in_waiting).decode('utf-8', errors='ignore')
        
        assert "INIT_READY" in response, "Should receive INIT_READY on reinit"
        
        # Verify state was reset (firing should stop)
        time.sleep(0.3)
        status = fresh_gunfx.wait_for_status(timeout=2.0)
        if status:
            assert status.get('firing') == False, "Firing should stop on reinit"
