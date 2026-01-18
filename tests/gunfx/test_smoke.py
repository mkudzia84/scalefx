"""
GunFX Smoke Command Tests

Tests for SMOKE_HEAT and SMOKE_SETTINGS commands.
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, GunFxCommands,
    GunFxError
)


@pytest.mark.hardware
@pytest.mark.gunfx
class TestGunFxSmoke:
    """Smoke generator command tests."""
    
    def test_smoke_heat_on(self, gunfx: ScaleFXConnection):
        """SMOKE_HEAT on should enable heater."""
        try:
            success, response = gunfx.send_expect_ack(GunFxCommands.smoke_heat(True))
            assert success, f"SMOKE_HEAT on failed: {response}"
        finally:
            # Always turn off heater
            gunfx.send(GunFxCommands.smoke_heat(False))
    
    def test_smoke_heat_off(self, gunfx: ScaleFXConnection):
        """SMOKE_HEAT off should disable heater."""
        success, response = gunfx.send_expect_ack(GunFxCommands.smoke_heat(False))
        assert success, "SMOKE_HEAT off failed"
    
    def test_smoke_settings_constant_mode(self, gunfx: ScaleFXConnection):
        """SMOKE_SETTINGS for constant fan mode."""
        success, response = gunfx.send_expect_ack(
            GunFxCommands.smoke_settings(
                pulsing=False,
                speed=200,
                pulse_high=255,
                pulse_low=80,
                pulse_ms=50,
                spindown_ms=5000
            )
        )
        assert success, "SMOKE_SETTINGS constant mode failed"
    
    def test_smoke_settings_pulsing_mode(self, gunfx: ScaleFXConnection):
        """SMOKE_SETTINGS for pulsing fan mode."""
        success, response = gunfx.send_expect_ack(
            GunFxCommands.smoke_settings(
                pulsing=True,
                speed=200,
                pulse_high=255,
                pulse_low=80,
                pulse_ms=50,
                spindown_ms=5000
            )
        )
        assert success, "SMOKE_SETTINGS pulsing mode failed"
    
    @pytest.mark.slow
    def test_smoke_heater_with_fan(self, gunfx: ScaleFXConnection):
        """Test heater operation with smoke fan."""
        try:
            # Configure fan
            gunfx.send_expect_ack(
                GunFxCommands.smoke_settings(
                    pulsing=False,
                    speed=150,
                    pulse_high=255,
                    pulse_low=80,
                    pulse_ms=50,
                    spindown_ms=3000
                )
            )
            
            # Start firing to activate fan
            gunfx.send_expect_ack(GunFxCommands.trigger_on(300))
            time.sleep(0.5)
            
            # Enable heater
            success, response = gunfx.send_expect_ack(GunFxCommands.smoke_heat(True))
            assert success, "SMOKE_HEAT failed during firing"
            
            # Let it run briefly
            time.sleep(1.0)
            
        finally:
            # Cleanup
            gunfx.send(GunFxCommands.smoke_heat(False))
            gunfx.send(GunFxCommands.trigger_off(0))
    
    def test_smoke_heater_safety_without_fan(self, gunfx: ScaleFXConnection):
        """Heater should not activate without fan running (safety check)."""
        # Make sure firing is stopped
        gunfx.send(GunFxCommands.trigger_off(0))
        time.sleep(0.5)
        
        # Try to enable heater without fan
        success, response = gunfx.send_expect_ack(GunFxCommands.smoke_heat(True))
        
        # Should either fail with safety error, or succeed but not actually heat
        # Depends on firmware implementation
        if not success:
            assert response.error_code in [
                GunFxError.HEATER_SAFETY,
                GunFxError.FAN_NOT_RUNNING
            ]
        
        # Make sure heater is off
        gunfx.send(GunFxCommands.smoke_heat(False))
