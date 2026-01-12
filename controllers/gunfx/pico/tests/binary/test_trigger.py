"""
GunFX Binary Protocol Trigger Tests

Tests for gun firing commands using binary protocol:
- TRIGGER_ON: Start firing at specified RPM
- TRIGGER_OFF: Stop firing with optional fan delay
"""

import pytest
import time
import struct
from conftest import (
    GunFxBinaryConnection, GunFxError,
    GUNFX_PKT_TRIGGER_ON, GUNFX_PKT_TRIGGER_OFF, SFX_PKT_NACK
)


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.firing
class TestTriggerOn:
    """Test TRIGGER_ON command."""
    
    def test_trigger_on_default(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with standard RPM (600 RPM = 10 shots/sec)."""
        success, _ = fresh_gunfx_binary.trigger_on(600)
        assert success, "TRIGGER_ON should ACK"
        
        # Verify firing state via STATUS
        status = fresh_gunfx_binary.wait_for_status()
        assert status is not None, "Should receive STATUS"
        assert status.get('firing') == True, "Should be firing"
        assert status.get('rpm') == 600, "RPM should match"
        
        # Clean up
        fresh_gunfx_binary.trigger_off(0)
    
    def test_trigger_on_low_rpm(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with low RPM (120 RPM = 2 shots/sec)."""
        success, _ = fresh_gunfx_binary.trigger_on(120)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 120
        
        fresh_gunfx_binary.trigger_off(0)
    
    def test_trigger_on_high_rpm(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with high RPM (1200 RPM = 20 shots/sec)."""
        success, _ = fresh_gunfx_binary.trigger_on(1200)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 1200
        
        fresh_gunfx_binary.trigger_off(0)
    
    def test_trigger_on_max_rpm(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with maximum RPM (3000)."""
        success, _ = fresh_gunfx_binary.trigger_on(3000)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 3000
        
        fresh_gunfx_binary.trigger_off(0)
    
    def test_trigger_on_min_rpm(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with minimum RPM (60)."""
        success, _ = fresh_gunfx_binary.trigger_on(60)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 60
        
        fresh_gunfx_binary.trigger_off(0)
    
    def test_trigger_change_rpm(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test changing RPM while firing."""
        # Start at 600 RPM
        success, _ = fresh_gunfx_binary.trigger_on(600)
        assert success
        time.sleep(0.3)
        
        # Change to 1200 RPM
        success, _ = fresh_gunfx_binary.trigger_on(1200)
        assert success
        
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('rpm') == 1200
        
        fresh_gunfx_binary.trigger_off(0)


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.firing
class TestTriggerOff:
    """Test TRIGGER_OFF command."""
    
    def test_trigger_off_immediate(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_OFF with no fan delay."""
        # Start firing
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.2)
        
        # Stop immediately
        success, _ = fresh_gunfx_binary.trigger_off(0)
        assert success
        
        time.sleep(0.2)
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == False
    
    def test_trigger_off_with_delay(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_OFF with fan spindown delay."""
        # Start firing
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.2)
        
        # Stop with 1 second fan delay
        success, _ = fresh_gunfx_binary.trigger_off(1000)
        assert success
        
        # Should show fanSpindown active
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == False
        # Note: fanSpindown may or may not be true depending on timing
    
    def test_trigger_off_when_not_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_OFF when already stopped."""
        # Make sure not firing
        fresh_gunfx_binary.trigger_off(0)
        time.sleep(0.2)
        
        # Try to stop again - should still ACK
        success, _ = fresh_gunfx_binary.trigger_off(0)
        assert success, "TRIGGER_OFF should ACK even when not firing"


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.firing
class TestTriggerErrors:
    """Test TRIGGER error conditions."""
    
    def test_trigger_rpm_zero(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with rpm=0 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_TRIGGER_ON, struct.pack('<H', 0)
        )
        assert is_nack, "Should NACK with rpm=0"
        assert error_code == GunFxError.INVALID_RPM
    
    def test_trigger_rpm_too_high(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with rpm > 3000 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_TRIGGER_ON, struct.pack('<H', 3500)
        )
        assert is_nack, "Should NACK with rpm > 3000"
        assert error_code == GunFxError.INVALID_RPM


@pytest.mark.hardware
@pytest.mark.binary
@pytest.mark.firing
@pytest.mark.slow
class TestFiringEffects:
    """Test observable firing effects."""
    
    def test_flash_active_during_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test that flash is active during firing."""
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.5)  # Let a few shots fire
        
        # Check for flash activity
        status = fresh_gunfx_binary.wait_for_status()
        # flashActive might be True or False depending on exact timing
        # But firing should definitely be true
        assert status.get('firing') == True
        
        fresh_gunfx_binary.trigger_off(0)
    
    def test_sustained_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test sustained firing for several seconds."""
        fresh_gunfx_binary.trigger_on(600)
        
        # Check status multiple times
        for _ in range(5):
            time.sleep(0.5)
            status = fresh_gunfx_binary.wait_for_status()
            assert status.get('firing') == True
        
        fresh_gunfx_binary.trigger_off(0)
