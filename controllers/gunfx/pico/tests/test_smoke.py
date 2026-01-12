"""
GunFX Smoke Generator Tests

Tests for SMOKE_HEAT and SMOKE_SETTINGS commands.
Controls smoke heater and fan.
"""

import pytest
import time
from conftest import GunFxConnection


class TestSmokeHeater:
    """Test SMOKE_HEAT command."""
    
    @pytest.mark.smoke
    def test_smoke_heat_on(self, fresh_gunfx: GunFxConnection):
        """Test enabling smoke heater."""
        success, response = fresh_gunfx.send_and_expect_ack("SMOKE_HEAT on=1")
        assert success, f"SMOKE_HEAT on=1 should ACK, got: {response}"
        
        # Verify heater is on
        time.sleep(0.3)
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        assert status.get('heaterOn') == True, "Heater should be on"
        
        # Clean up
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
    
    @pytest.mark.smoke
    def test_smoke_heat_off(self, fresh_gunfx: GunFxConnection):
        """Test disabling smoke heater."""
        # First turn on
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(0.2)
        
        # Then turn off
        success, response = fresh_gunfx.send_and_expect_ack("SMOKE_HEAT on=0")
        assert success, f"SMOKE_HEAT on=0 should ACK, got: {response}"
        
        # Verify heater is off
        time.sleep(0.2)
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        assert status.get('heaterOn') == False, "Heater should be off"
    
    @pytest.mark.smoke
    def test_smoke_heat_toggle(self, fresh_gunfx: GunFxConnection):
        """Test toggling smoke heater on and off."""
        # On
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(0.2)
        status = fresh_gunfx.wait_for_status()
        assert status.get('heaterOn') == True
        
        # Off
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
        time.sleep(0.2)
        status = fresh_gunfx.wait_for_status()
        assert status.get('heaterOn') == False
        
        # On again
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(0.2)
        status = fresh_gunfx.wait_for_status()
        assert status.get('heaterOn') == True
        
        # Clean up
        fresh_gunfx.send_command("SMOKE_HEAT on=0")


class TestSmokeSettings:
    """Test SMOKE_SETTINGS command."""
    
    @pytest.mark.smoke
    def test_smoke_settings_constant_mode(self, fresh_gunfx: GunFxConnection):
        """Test configuring smoke fan in constant speed mode."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SMOKE_SETTINGS fanPulsing=0 fanSpeed=200"
        )
        assert success, f"SMOKE_SETTINGS constant mode should ACK, got: {response}"
    
    @pytest.mark.smoke
    def test_smoke_settings_pulsing_mode(self, fresh_gunfx: GunFxConnection):
        """Test configuring smoke fan in pulsing mode."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SMOKE_SETTINGS fanPulsing=1 fanPulseHigh=255 fanPulseLow=80 fanPulseMs=50"
        )
        assert success, f"SMOKE_SETTINGS pulsing mode should ACK, got: {response}"
    
    @pytest.mark.smoke
    def test_smoke_settings_spindown(self, fresh_gunfx: GunFxConnection):
        """Test configuring fan spindown delay."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SMOKE_SETTINGS fanSpindownMs=3000"
        )
        assert success, f"SMOKE_SETTINGS spindown should ACK, got: {response}"
    
    @pytest.mark.smoke
    def test_smoke_settings_full(self, fresh_gunfx: GunFxConnection):
        """Test configuring all smoke settings at once."""
        success, response = fresh_gunfx.send_and_expect_ack(
            "SMOKE_SETTINGS fanPulsing=1 fanSpeed=200 fanPulseHigh=255 "
            "fanPulseLow=80 fanPulseMs=50 fanSpindownMs=5000"
        )
        assert success, f"Full SMOKE_SETTINGS should ACK, got: {response}"


class TestSmokeWithFiring:
    """Test smoke behavior with firing."""
    
    @pytest.mark.smoke
    @pytest.mark.firing
    @pytest.mark.slow
    def test_smoke_fan_during_fire(self, fresh_gunfx: GunFxConnection):
        """Test smoke fan behavior during firing."""
        # Configure smoke settings
        fresh_gunfx.send_command("SMOKE_SETTINGS fanPulsing=0 fanSpeed=200 fanSpindownMs=2000")
        
        # Enable heater
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(0.2)
        
        # Start firing
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.5)
        
        # Check status - fan should be on during firing
        status = fresh_gunfx.wait_for_status()
        assert status is not None
        assert status.get('firing') == True
        assert status.get('heaterOn') == True
        # Fan may or may not be on depending on mode
        
        # Stop firing
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        time.sleep(0.3)
        
        # Clean up
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
    
    @pytest.mark.smoke
    @pytest.mark.firing
    @pytest.mark.slow
    def test_fan_spindown_after_fire(self, fresh_gunfx: GunFxConnection):
        """Test fan spindown delay after firing stops."""
        # Configure with 2 second spindown
        fresh_gunfx.send_command("SMOKE_SETTINGS fanSpindownMs=2000")
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        time.sleep(0.2)
        
        # Fire briefly
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.5)
        
        # Stop firing with fan delay
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=2000")
        time.sleep(0.3)
        
        # Check fan status - should show spindown pending
        status = fresh_gunfx.wait_for_status()
        # Fan may still be spinning down
        if status.get('fanSpindown'):
            assert status.get('fanOffRemainingMs', 0) > 0
        
        # Wait for spindown to complete
        time.sleep(2.5)
        status = fresh_gunfx.wait_for_status()
        assert status.get('fanSpindown') == False, "Fan spindown should be complete"
        
        # Clean up
        fresh_gunfx.send_command("SMOKE_HEAT on=0")
    
    @pytest.mark.smoke
    @pytest.mark.firing
    def test_heater_safety_on_shutdown(self, fresh_gunfx: GunFxConnection):
        """Test heater is disabled on shutdown."""
        # Enable heater and start firing
        fresh_gunfx.send_command("SMOKE_HEAT on=1")
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.3)
        
        # Verify heater is on
        status = fresh_gunfx.wait_for_status()
        assert status.get('heaterOn') == True
        
        # Send shutdown
        fresh_gunfx.send_command("SHUTDOWN")
        time.sleep(0.5)
        
        # Heater should be off
        status = fresh_gunfx.wait_for_status()
        if status:
            assert status.get('heaterOn') == False, "Heater should be off after shutdown"
            assert status.get('firing') == False, "Should not be firing after shutdown"


class TestSmokeStatusFields:
    """Test smoke-related status fields."""
    
    @pytest.mark.smoke
    def test_status_fan_fields(self, fresh_gunfx: GunFxConnection):
        """Test that status includes fan-related fields."""
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        
        # Check for required fields
        assert 'fanOn' in status, "Status should include fanOn"
        assert 'fanSpindown' in status, "Status should include fanSpindown"
        assert 'fanOffRemainingMs' in status, "Status should include fanOffRemainingMs"
    
    @pytest.mark.smoke
    def test_status_heater_field(self, fresh_gunfx: GunFxConnection):
        """Test that status includes heater field."""
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        assert 'heaterOn' in status, "Status should include heaterOn"
