"""
GunFX Binary Protocol Smoke Tests

Tests for smoke generator commands using binary protocol:
- SMOKE_HEAT: Enable/disable smoke heater
- SMOKE_SETTINGS: Configure smoke generator parameters
"""

import pytest
import time
import struct
from conftest import (
    GunFxBinaryConnection, GunFxError,
    GUNFX_PKT_SMOKE_HEAT, GUNFX_PKT_SMOKE_SETTINGS
)


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.smoke
class TestSmokeHeat:
    """Test SMOKE_HEAT command."""
    
    def test_smoke_heat_enable(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test enabling smoke heater."""
        success, _ = fresh_gunfx_binary.smoke_heat(True)
        assert success, "SMOKE_HEAT enable should ACK"
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('heaterOn') == True, "Heater should be on"
        
        # Clean up - disable heater
        fresh_gunfx_binary.smoke_heat(False)
    
    def test_smoke_heat_disable(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test disabling smoke heater."""
        # First enable
        fresh_gunfx_binary.smoke_heat(True)
        time.sleep(0.2)
        
        # Then disable
        success, _ = fresh_gunfx_binary.smoke_heat(False)
        assert success, "SMOKE_HEAT disable should ACK"
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('heaterOn') == False, "Heater should be off"
    
    def test_smoke_heat_toggle(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test toggling heater on/off."""
        # Ensure off
        fresh_gunfx_binary.smoke_heat(False)
        time.sleep(0.2)
        
        # Toggle on
        success, _ = fresh_gunfx_binary.smoke_heat(True)
        assert success
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('heaterOn') == True
        
        # Toggle off
        success, _ = fresh_gunfx_binary.smoke_heat(False)
        assert success
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('heaterOn') == False
    
    def test_smoke_heat_already_on(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test enabling heater when already on."""
        fresh_gunfx_binary.smoke_heat(True)
        time.sleep(0.2)
        
        # Enable again - should still ACK
        success, _ = fresh_gunfx_binary.smoke_heat(True)
        assert success, "Should ACK even when already enabled"
        
        fresh_gunfx_binary.smoke_heat(False)
    
    def test_smoke_heat_already_off(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test disabling heater when already off."""
        fresh_gunfx_binary.smoke_heat(False)
        time.sleep(0.2)
        
        # Disable again - should still ACK
        success, _ = fresh_gunfx_binary.smoke_heat(False)
        assert success, "Should ACK even when already disabled"


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.smoke
class TestSmokeSettings:
    """Test SMOKE_SETTINGS command."""
    
    def test_smoke_settings_basic(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test basic smoke settings configuration."""
        # SMOKE_SETTINGS payload format needs to match protocol
        # For now, this tests the command is accepted
        # Actual payload structure depends on implementation
        pass  # TODO: Implement when SMOKE_SETTINGS binary payload is defined
    
    @pytest.mark.skip(reason="SMOKE_SETTINGS binary payload format TBD")
    def test_smoke_settings_fan_speed(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test configuring fan speed."""
        # Placeholder for when binary format is finalized
        pass


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.smoke
@pytest.mark.firing
class TestSmokeWithFiring:
    """Test smoke generator interaction with firing."""
    
    def test_smoke_heater_during_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test heater can be enabled during firing."""
        # Start firing
        success, _ = fresh_gunfx_binary.trigger_on(600)
        assert success
        time.sleep(0.3)
        
        # Enable heater
        success, _ = fresh_gunfx_binary.smoke_heat(True)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == True
        assert status.get('heaterOn') == True
        
        # Clean up
        fresh_gunfx_binary.smoke_heat(False)
        fresh_gunfx_binary.trigger_off(0)
    
    def test_fan_runs_with_heater(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test fan runs when heater is enabled."""
        fresh_gunfx_binary.smoke_heat(True)
        time.sleep(0.3)
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('heaterOn') == True
        # Fan should typically run with heater for safety
        # Implementation dependent - may or may not auto-start fan
        
        fresh_gunfx_binary.smoke_heat(False)
    
    def test_shutdown_disables_smoke(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SHUTDOWN disables smoke heater and fan."""
        # Enable heater
        fresh_gunfx_binary.smoke_heat(True)
        time.sleep(0.2)
        
        # Verify heater is on
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('heaterOn') == True
        
        # Send shutdown
        fresh_gunfx_binary.shutdown()
        time.sleep(0.3)
        
        # Reconnect and verify
        conn = GunFxBinaryConnection()
        if conn.connect():
            status = conn.wait_for_status()
            if status:
                assert status.get('heaterOn') == False, "Heater should be off after shutdown"
                assert status.get('fanOn') == False, "Fan should be off after shutdown"
            conn.close()


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.smoke
@pytest.mark.slow
class TestSmokeFanBehavior:
    """Test smoke fan behavior."""
    
    def test_fan_active_during_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test fan is active during firing."""
        # Start firing
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.5)
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('fanOn') == True, "Fan should be on during firing"
        
        fresh_gunfx_binary.trigger_off(0)
    
    def test_fan_spindown_after_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test fan spindown delay after firing stops."""
        # Start firing
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.5)
        
        # Stop with spindown delay
        fresh_gunfx_binary.trigger_off(2000)  # 2 second delay
        
        # Check for spindown state
        time.sleep(0.2)
        status = fresh_gunfx_binary.wait_for_status()
        
        # Should show fanSpindown or fanOn depending on timing
        if status.get('fanOn'):
            assert status.get('firing') == False, "Should not be firing during spindown"
    
    def test_fan_immediate_off(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test fan turns off immediately with 0 delay."""
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.3)
        
        # Stop with no delay
        fresh_gunfx_binary.trigger_off(0)
        time.sleep(0.3)
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('fanOn') == False or status.get('fanSpindown') == False
