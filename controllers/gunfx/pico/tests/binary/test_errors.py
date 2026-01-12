"""
GunFX Binary Protocol Error Tests

Tests for error responses (NACK) using binary protocol.
Verifies proper error codes are returned.
"""

import pytest
import time
import struct
from conftest import (
    GunFxBinaryConnection, GunFxError,
    GUNFX_PKT_TRIGGER_ON, GUNFX_PKT_SRV_SET, SFX_PKT_NACK,
    build_packet, cobs_encode
)


@pytest.mark.hardware
@pytest.mark.binary
class TestServoErrors:
    """Test servo-related error conditions."""
    
    def test_servo_invalid_id_zero(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with id=0 returns NACK with correct error."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 0, 1500)
        )
        assert is_nack, "Should NACK"
        assert error_code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_invalid_id_high(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with id=4 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 4, 1500)
        )
        assert is_nack
        assert error_code == GunFxError.SERVO_INVALID_ID
    
    def test_servo_pulse_too_low(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with pulseUs < 500 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 1, 400)
        )
        assert is_nack
        assert error_code == GunFxError.SERVO_PULSE_RANGE
    
    def test_servo_pulse_too_high(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test SERVO_SET with pulseUs > 2500 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 1, 2600)
        )
        assert is_nack
        assert error_code == GunFxError.SERVO_PULSE_RANGE


@pytest.mark.hardware
@pytest.mark.binary
class TestTriggerErrors:
    """Test trigger-related error conditions."""
    
    def test_trigger_rpm_zero(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with rpm=0 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_TRIGGER_ON, struct.pack('<H', 0)
        )
        assert is_nack
        assert error_code == GunFxError.INVALID_RPM
    
    def test_trigger_rpm_too_high(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test TRIGGER_ON with rpm > 3000 returns NACK."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_TRIGGER_ON, struct.pack('<H', 3500)
        )
        assert is_nack
        assert error_code == GunFxError.INVALID_RPM


@pytest.mark.hardware
@pytest.mark.binary
class TestNackFormat:
    """Test NACK packet format."""
    
    def test_nack_has_error_code(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test NACK includes error code."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 0, 1500)
        )
        assert is_nack
        assert error_code > 0, "NACK should include non-zero error code"
    
    def test_nack_may_have_reason(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test NACK may include reason string."""
        is_nack, error_code, reason = fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 0, 1500)
        )
        assert is_nack
        # Reason may or may not be present in binary protocol


@pytest.mark.hardware
@pytest.mark.binary
class TestErrorRecovery:
    """Test recovery after errors."""
    
    def test_recovery_after_nack(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test device continues working after NACK."""
        # Cause a NACK
        fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 0, 1500)
        )
        
        # Should still accept valid commands
        success, _ = fresh_gunfx_binary.servo_set(1, 1500)
        assert success, "Should accept valid command after NACK"
    
    def test_multiple_errors(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test device handles multiple consecutive errors."""
        # Send several invalid commands
        for _ in range(5):
            fresh_gunfx_binary.send_and_expect_nack(
                GUNFX_PKT_SRV_SET, struct.pack('<BH', 0, 1500)
            )
            time.sleep(0.1)
        
        # Should still work
        success, _ = fresh_gunfx_binary.trigger_on(600)
        assert success, "Should work after multiple errors"
        
        fresh_gunfx_binary.trigger_off(0)
    
    @pytest.mark.firing
    def test_recovery_during_firing(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test error handling during firing doesn't stop firing."""
        # Start firing
        fresh_gunfx_binary.trigger_on(600)
        time.sleep(0.2)
        
        # Send invalid command
        fresh_gunfx_binary.send_and_expect_nack(
            GUNFX_PKT_SRV_SET, struct.pack('<BH', 0, 1500)
        )
        time.sleep(0.1)
        
        # Should still be firing
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == True, "Error should not stop firing"
        
        # Clean up
        fresh_gunfx_binary.trigger_off(0)


@pytest.mark.hardware
@pytest.mark.binary
class TestMalformedPackets:
    """Test handling of malformed binary packets."""
    
    def test_empty_payload(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test command with empty payload when payload expected."""
        # TRIGGER_ON expects u16 payload
        resp_type, resp_payload = fresh_gunfx_binary.send_and_receive(
            GUNFX_PKT_TRIGGER_ON, b''
        )
        # Should NACK or be ignored
        assert resp_type == SFX_PKT_NACK or resp_type is None
    
    def test_truncated_payload(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test command with truncated payload."""
        # TRIGGER_ON expects 2 bytes (u16), send only 1
        resp_type, resp_payload = fresh_gunfx_binary.send_and_receive(
            GUNFX_PKT_TRIGGER_ON, b'\x58'
        )
        # Should NACK or be ignored
        assert resp_type == SFX_PKT_NACK or resp_type is None
    
    def test_extra_payload(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test command with extra payload data."""
        # TRIGGER_ON expects 2 bytes, send 4
        resp_type, resp_payload = fresh_gunfx_binary.send_and_receive(
            GUNFX_PKT_TRIGGER_ON, struct.pack('<HH', 600, 0xDEAD)
        )
        # Should still work (extra bytes ignored) or NACK
        assert resp_type is not None
        
        # Clean up if it started firing
        fresh_gunfx_binary.trigger_off(0)


@pytest.mark.hardware
@pytest.mark.binary
class TestCrcErrors:
    """Test CRC error handling."""
    
    def test_invalid_crc_rejected(self, fresh_gunfx_binary: GunFxBinaryConnection):
        """Test packets with invalid CRC are rejected."""
        # Build a packet manually with wrong CRC
        pkt_type = GUNFX_PKT_TRIGGER_ON
        payload = struct.pack('<H', 600)
        length = len(payload)
        
        # Create packet with deliberately wrong CRC
        wrong_crc = 0xFF
        raw_packet = bytes([pkt_type, length]) + payload + bytes([wrong_crc])
        encoded = cobs_encode(raw_packet)
        
        # Clear buffer and send
        fresh_gunfx_binary._clear_buffer()
        fresh_gunfx_binary.ser.write(encoded)
        time.sleep(0.3)
        
        # Should receive no valid response (packet rejected)
        resp_type, _ = fresh_gunfx_binary.receive_packet(timeout=0.5)
        
        # Device should not be firing
        status = fresh_gunfx_binary.wait_for_status()
        assert status.get('firing') == False, "Invalid CRC packet should be rejected"
