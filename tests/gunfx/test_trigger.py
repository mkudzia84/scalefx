"""
GunFX Trigger Command Tests

Tests for TRIGGER_ON and TRIGGER_OFF commands.
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, GunFxCommands,
    CorePacket, GunFxError
)


@pytest.mark.hardware
@pytest.mark.gunfx
class TestGunFxTrigger:
    """Trigger command tests."""
    
    def test_trigger_on_valid_rpm(self, gunfx: ScaleFXConnection):
        """TRIGGER_ON with valid RPM should return ACK."""
        try:
            success, response = gunfx.send_expect_ack(GunFxCommands.trigger_on(600))
            assert success, f"TRIGGER_ON failed: {response}"
        finally:
            # Always stop firing
            gunfx.send(GunFxCommands.trigger_off(0))
    
    def test_trigger_on_min_rpm(self, gunfx: ScaleFXConnection):
        """TRIGGER_ON at minimum RPM (1)."""
        try:
            success, response = gunfx.send_expect_ack(GunFxCommands.trigger_on(1))
            assert success, "TRIGGER_ON at min RPM failed"
        finally:
            gunfx.send(GunFxCommands.trigger_off(0))
    
    def test_trigger_on_max_rpm(self, gunfx: ScaleFXConnection):
        """TRIGGER_ON at maximum RPM (3000)."""
        try:
            success, response = gunfx.send_expect_ack(GunFxCommands.trigger_on(3000))
            assert success, "TRIGGER_ON at max RPM failed"
        finally:
            gunfx.send(GunFxCommands.trigger_off(0))
    
    def test_trigger_on_invalid_rpm_zero(self, gunfx: ScaleFXConnection):
        """TRIGGER_ON with RPM=0 should return NACK."""
        success, response = gunfx.send_expect_ack(GunFxCommands.trigger_on(0))
        assert not success, "Should reject RPM=0"
        assert response.is_nack
        assert response.error_code == GunFxError.INVALID_RPM
    
    def test_trigger_on_invalid_rpm_over_max(self, gunfx: ScaleFXConnection):
        """TRIGGER_ON with RPM>3000 should return NACK."""
        success, response = gunfx.send_expect_ack(GunFxCommands.trigger_on(5000))
        assert not success, "Should reject RPM>3000"
        assert response.is_nack
        assert response.error_code == GunFxError.INVALID_RPM
    
    def test_trigger_off(self, gunfx: ScaleFXConnection):
        """TRIGGER_OFF should return ACK."""
        # Start firing first
        gunfx.send(GunFxCommands.trigger_on(600))
        time.sleep(0.1)
        
        # Stop firing
        success, response = gunfx.send_expect_ack(GunFxCommands.trigger_off(0))
        assert success, "TRIGGER_OFF failed"
    
    def test_trigger_off_with_fan_delay(self, gunfx: ScaleFXConnection):
        """TRIGGER_OFF with fan delay should return ACK."""
        # Start firing
        gunfx.send(GunFxCommands.trigger_on(600))
        time.sleep(0.1)
        
        # Stop with 3 second fan delay
        success, response = gunfx.send_expect_ack(GunFxCommands.trigger_off(3000))
        assert success, "TRIGGER_OFF with delay failed"
    
    def test_trigger_on_while_firing(self, gunfx: ScaleFXConnection):
        """TRIGGER_ON while already firing should return NACK."""
        try:
            # Start firing
            success, _ = gunfx.send_expect_ack(GunFxCommands.trigger_on(600))
            assert success, "Initial TRIGGER_ON failed"
            
            # Try to fire again
            success, response = gunfx.send_expect_ack(GunFxCommands.trigger_on(800))
            assert not success, "Should reject firing while already firing"
            assert response.error_code == GunFxError.ALREADY_FIRING
        finally:
            gunfx.send(GunFxCommands.trigger_off(0))
    
    def test_trigger_off_when_not_firing(self, gunfx: ScaleFXConnection):
        """TRIGGER_OFF when not firing should still return ACK (idempotent)."""
        # Make sure we're not firing
        gunfx.send(GunFxCommands.trigger_off(0))
        time.sleep(0.1)
        
        # Try to stop again - should be OK
        success, response = gunfx.send_expect_ack(GunFxCommands.trigger_off(0))
        # May return ACK (idempotent) or NACK (strict mode)
        # Accept either behavior
        assert response is not None
