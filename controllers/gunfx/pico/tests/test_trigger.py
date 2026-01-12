"""
GunFX Trigger Tests

Tests for TRIGGER_ON and TRIGGER_OFF commands.
Controls muzzle flash and firing rate.
"""

import pytest
import time
from conftest import GunFxConnection


class TestTriggerOn:
    """Test TRIGGER_ON command."""
    
    @pytest.mark.firing
    def test_trigger_on_default(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with default RPM."""
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=600")
        assert success, f"TRIGGER_ON should return ACK, got: {response}"
        
        # Verify firing started
        time.sleep(0.3)
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        assert status.get('firing') == True, "Should be firing"
        assert status.get('rpm') == 600, f"RPM should be 600, got {status.get('rpm')}"
        
        # Clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    @pytest.mark.firing
    def test_trigger_on_various_rpm(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with various RPM values."""
        test_rpms = [100, 300, 600, 1000, 1500, 2000, 3000]
        
        for rpm in test_rpms:
            success, response = fresh_gunfx.send_and_expect_ack(f"TRIGGER_ON rpm={rpm}")
            assert success, f"TRIGGER_ON rpm={rpm} should ACK, got: {response}"
            
            time.sleep(0.2)
            status = fresh_gunfx.wait_for_status()
            assert status is not None, f"Should receive STATUS for rpm={rpm}"
            assert status.get('firing') == True, f"Should be firing at rpm={rpm}"
            assert status.get('rpm') == rpm, f"RPM should be {rpm}, got {status.get('rpm')}"
        
        # Clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    @pytest.mark.firing
    def test_trigger_on_min_rpm(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with minimum valid RPM (1)."""
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=1")
        assert success, f"TRIGGER_ON rpm=1 should ACK, got: {response}"
        
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    @pytest.mark.firing
    def test_trigger_on_max_rpm(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with maximum valid RPM (3000)."""
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=3000")
        assert success, f"TRIGGER_ON rpm=3000 should ACK, got: {response}"
        
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    def test_trigger_on_invalid_rpm_zero(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with invalid RPM (0) returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("TRIGGER_ON rpm=0")
        assert success, f"TRIGGER_ON rpm=0 should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == 0x40, f"Error code should be INVALID_RPM (0x40), got 0x{code:02x}"
    
    def test_trigger_on_invalid_rpm_too_high(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with RPM > 3000 returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("TRIGGER_ON rpm=3001")
        assert success, f"TRIGGER_ON rpm=3001 should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == 0x40, f"Error code should be INVALID_RPM (0x40), got 0x{code:02x}"


class TestTriggerOff:
    """Test TRIGGER_OFF command."""
    
    @pytest.mark.firing
    def test_trigger_off_immediate(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_OFF with no fan delay."""
        # Start firing first
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        
        # Stop firing
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_OFF fanDelayMs=0")
        assert success, f"TRIGGER_OFF should ACK, got: {response}"
        
        # Verify stopped
        time.sleep(0.2)
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        assert status.get('firing') == False, "Should not be firing"
    
    @pytest.mark.firing
    @pytest.mark.slow
    def test_trigger_off_with_fan_delay(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_OFF with fan spindown delay."""
        # Start firing with fan
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.3)
        
        # Stop firing with 2 second fan delay
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_OFF fanDelayMs=2000")
        assert success, f"TRIGGER_OFF should ACK, got: {response}"
        
        # Check status immediately - fan should still be on
        time.sleep(0.2)
        status = fresh_gunfx.wait_for_status()
        assert status is not None, "Should receive STATUS"
        assert status.get('firing') == False, "Should not be firing"
        # Fan may or may not be on depending on smoke config
        
        # Wait for fan to spin down
        time.sleep(2.5)
        status = fresh_gunfx.wait_for_status()
        if status:
            assert status.get('fanSpindown') == False, "Fan spindown should be complete"
    
    @pytest.mark.firing
    def test_trigger_off_when_not_firing(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_OFF when not firing is accepted."""
        # Make sure not firing
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
        time.sleep(0.1)
        
        # Send again - should still work
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_OFF fanDelayMs=0")
        assert success, f"TRIGGER_OFF when not firing should ACK, got: {response}"


class TestTriggerSequence:
    """Test firing sequences and state transitions."""
    
    @pytest.mark.firing
    def test_fire_stop_fire(self, fresh_gunfx: GunFxConnection):
        """Test firing, stopping, then firing again."""
        # Fire
        fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        
        # Stop
        fresh_gunfx.send_and_expect_ack("TRIGGER_OFF fanDelayMs=0")
        time.sleep(0.2)
        
        # Fire again
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=800")
        assert success, "Should be able to fire again after stopping"
        
        status = fresh_gunfx.wait_for_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 800
        
        # Clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    @pytest.mark.firing
    def test_change_rpm_while_firing(self, fresh_gunfx: GunFxConnection):
        """Test changing RPM while already firing."""
        # Start at 600 RPM
        fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        
        # Change to 1200 RPM
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=1200")
        assert success, "Should be able to change RPM while firing"
        
        status = fresh_gunfx.wait_for_status()
        assert status.get('firing') == True
        assert status.get('rpm') == 1200, f"RPM should be 1200, got {status.get('rpm')}"
        
        # Clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    @pytest.mark.firing
    @pytest.mark.slow
    def test_burst_fire(self, fresh_gunfx: GunFxConnection):
        """Test rapid fire/stop sequences (burst fire)."""
        burst_count = 5
        burst_duration = 0.2  # seconds
        
        for i in range(burst_count):
            fresh_gunfx.send_command("TRIGGER_ON rpm=1000")
            time.sleep(burst_duration)
            fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
            time.sleep(0.1)
        
        # Verify stopped
        status = fresh_gunfx.wait_for_status()
        assert status.get('firing') == False, "Should not be firing after bursts"
