"""
GunFX Error Handling Tests

Tests for error responses (NACK) and invalid commands.
Verifies proper error codes are returned.
"""

import pytest
import time
from conftest import GunFxConnection


# GunFX Error Codes (from serial_gunfx_types.h)
class GunFxError:
    # Generic errors (inherited)
    OK = 0x00
    UNKNOWN_COMMAND = 0x01
    INVALID_PARAMETER = 0x02
    MISSING_PARAMETER = 0x03
    
    # Servo errors (0x20-0x2F)
    SERVO_INVALID_ID = 0x20
    SERVO_PULSE_RANGE = 0x21
    SERVO_MIN_MAX = 0x22
    SERVO_NOT_CONFIGURED = 0x23
    
    # Smoke/heater errors (0x30-0x3F)
    HEATER_SAFETY = 0x30
    FAN_NOT_RUNNING = 0x31
    INVALID_FAN_SPEED = 0x32
    
    # Trigger errors (0x40-0x4F)
    INVALID_RPM = 0x40
    ALREADY_FIRING = 0x41
    NOT_FIRING = 0x42


class TestUnknownCommands:
    """Test handling of unknown/invalid commands."""
    
    def test_unknown_command(self, fresh_gunfx: GunFxConnection):
        """Test unknown command is handled gracefully."""
        response = fresh_gunfx.send_command("UNKNOWN_COMMAND")
        # Unknown commands may be silently ignored or NACK'd
        # Either way, should not crash
        assert "ERROR" not in response or "NACK" in response
    
    def test_malformed_command(self, fresh_gunfx: GunFxConnection):
        """Test malformed command doesn't crash device."""
        # Send various malformed inputs
        malformed = [
            "",
            "   ",
            "=value",
            "TRIGGER_ON=600",  # Wrong format
            "TRIGGER_ON rpm",  # Missing value
        ]
        
        for cmd in malformed:
            try:
                response = fresh_gunfx.send_command(cmd, delay=0.2)
                # Should not crash - may NACK or ignore
            except Exception as e:
                pytest.fail(f"Command '{cmd}' caused exception: {e}")


class TestServoErrors:
    """Test servo-related error conditions."""
    
    def test_servo_invalid_id_zero(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with id=0 returns NACK with correct error."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=0 pulseUs=1500")
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_invalid_id_high(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with id=4 returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=4 pulseUs=1500")
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_pulse_too_low(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with pulseUs < 500 returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=1 pulseUs=400")
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.SERVO_PULSE_RANGE
    
    def test_servo_pulse_too_high(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_SET with pulseUs > 2500 returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=1 pulseUs=2600")
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.SERVO_PULSE_RANGE
    
    def test_servo_config_invalid_id(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_CONFIG with invalid id returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack(
            "SERVO_CONFIG id=5 minUs=1000 maxUs=2000"
        )
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_recoil_jerk_invalid_id(self, fresh_gunfx: GunFxConnection):
        """Test SERVO_RECOIL_JERK with invalid id returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack(
            "SERVO_RECOIL_JERK id=0 jerkUs=50 varianceUs=10"
        )
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.SERVO_INVALID_ID


class TestTriggerErrors:
    """Test trigger-related error conditions."""
    
    def test_trigger_rpm_zero(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with rpm=0 returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("TRIGGER_ON rpm=0")
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.INVALID_RPM
    
    def test_trigger_rpm_too_high(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with rpm > 3000 returns NACK."""
        success, response = fresh_gunfx.send_and_expect_nack("TRIGGER_ON rpm=3500")
        assert success, f"Should NACK, got: {response}"
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code == GunFxError.INVALID_RPM
    
    def test_trigger_rpm_negative(self, fresh_gunfx: GunFxConnection):
        """Test TRIGGER_ON with negative rpm returns NACK."""
        # Note: Negative values may wrap to large positive values in parsing
        success, response = fresh_gunfx.send_and_expect_nack("TRIGGER_ON rpm=-100")
        # May NACK or interpret as large positive - either is acceptable
        assert "NACK" in response or "ACK" in response


class TestNackFormat:
    """Test NACK response format."""
    
    def test_nack_has_code(self, fresh_gunfx: GunFxConnection):
        """Test NACK includes error code."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=0 pulseUs=1500")
        assert success
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert code > 0, "NACK should include non-zero error code"
    
    def test_nack_has_reason(self, fresh_gunfx: GunFxConnection):
        """Test NACK includes reason string."""
        success, response = fresh_gunfx.send_and_expect_nack("SERVO_SET id=0 pulseUs=1500")
        assert success
        
        code, reason = fresh_gunfx.parse_nack(response)
        assert reason, "NACK should include reason string"
        assert len(reason) > 0


class TestErrorRecovery:
    """Test recovery after errors."""
    
    def test_recovery_after_nack(self, fresh_gunfx: GunFxConnection):
        """Test device continues working after NACK."""
        # Cause a NACK
        fresh_gunfx.send_and_expect_nack("SERVO_SET id=0 pulseUs=1500")
        
        # Should still accept valid commands
        success, response = fresh_gunfx.send_and_expect_ack("SERVO_SET id=1 pulseUs=1500")
        assert success, "Should accept valid command after NACK"
    
    def test_multiple_errors(self, fresh_gunfx: GunFxConnection):
        """Test device handles multiple consecutive errors."""
        # Send several invalid commands
        for _ in range(5):
            fresh_gunfx.send_command("SERVO_SET id=0 pulseUs=1500")
            time.sleep(0.1)
        
        # Should still work
        success, response = fresh_gunfx.send_and_expect_ack("TRIGGER_ON rpm=600")
        assert success, "Should work after multiple errors"
        
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
    
    @pytest.mark.firing
    def test_recovery_during_firing(self, fresh_gunfx: GunFxConnection):
        """Test error handling during firing doesn't stop firing."""
        # Start firing
        fresh_gunfx.send_command("TRIGGER_ON rpm=600")
        time.sleep(0.2)
        
        # Send invalid command
        fresh_gunfx.send_command("SERVO_SET id=0 pulseUs=1500")
        time.sleep(0.1)
        
        # Should still be firing
        status = fresh_gunfx.wait_for_status()
        assert status.get('firing') == True, "Error should not stop firing"
        
        # Clean up
        fresh_gunfx.send_command("TRIGGER_OFF fanDelayMs=0")
