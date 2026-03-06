"""
LightFX LED Command Tests

Tests for LED_SET, LED_OFF, LED sequence commands, and query responses.
"""

import pytest
import time

from tests.framework import (
    ScaleFXConnection, LightFxCommands,
    LightFxError, LightFxPacket
)


@pytest.mark.hardware
@pytest.mark.lightfx
class TestLightFxLed:
    """LED command tests."""
    
    def test_led_set(self, lightfx: ScaleFXConnection):
        """LED_SET should set channel brightness."""
        try:
            success, response = lightfx.send_expect_ack(
                LightFxCommands.led_set(1, 128)
            )
            assert success, f"LED_SET failed: {response}"
        finally:
            lightfx.send(LightFxCommands.led_off(0))
    
    def test_led_set_all_channels(self, lightfx: ScaleFXConnection):
        """LED_SET for all 8 channels."""
        try:
            for ch in range(1, 9):
                success, response = lightfx.send_expect_ack(
                    LightFxCommands.led_set(ch, 100)
                )
                assert success, f"LED_SET channel {ch} failed"
        finally:
            lightfx.send(LightFxCommands.led_off(0))
    
    def test_led_set_max_brightness(self, lightfx: ScaleFXConnection):
        """LED_SET with maximum brightness (255)."""
        try:
            success, response = lightfx.send_expect_ack(
                LightFxCommands.led_set(1, 255)
            )
            assert success
        finally:
            lightfx.send(LightFxCommands.led_off(0))
    
    def test_led_set_zero_brightness(self, lightfx: ScaleFXConnection):
        """LED_SET with zero brightness."""
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_set(1, 0)
        )
        assert success
    
    def test_led_set_invalid_channel(self, lightfx: ScaleFXConnection):
        """LED_SET with invalid channel should fail."""
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_set(9, 100)
        )
        assert not success, "Should reject channel > 8"
        assert response.error_code == LightFxError.INVALID_CHANNEL
    
    def test_led_off_single(self, lightfx: ScaleFXConnection):
        """LED_OFF for single channel."""
        # Turn on first
        lightfx.send(LightFxCommands.led_set(1, 200))
        time.sleep(0.1)
        
        # Turn off
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_off(1)
        )
        assert success
    
    def test_led_off_all(self, lightfx: ScaleFXConnection):
        """LED_OFF with channel=0 turns off all LEDs."""
        # Turn on several
        for ch in range(1, 5):
            lightfx.send(LightFxCommands.led_set(ch, 150))
        time.sleep(0.1)
        
        # Turn off all
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_off(0)
        )
        assert success


@pytest.mark.hardware
@pytest.mark.lightfx
class TestLightFxLedSequence:
    """LED sequence command tests."""
    
    def test_seq_clear(self, lightfx: ScaleFXConnection):
        """LED_SEQ_CLEAR should clear channel sequence."""
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_seq_clear(1)
        )
        assert success
    
    def test_seq_add_on(self, lightfx: ScaleFXConnection):
        """LED_SEQ_ADD with ON event."""
        # Clear first
        lightfx.send(LightFxCommands.led_seq_clear(1))
        
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_seq_add_on(1, 1000, 200)
        )
        assert success
    
    def test_seq_add_flash(self, lightfx: ScaleFXConnection):
        """LED_SEQ_ADD with FLASH event."""
        lightfx.send(LightFxCommands.led_seq_clear(1))
        
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_seq_add_flash(1, 100, 2000, 255, 50)
        )
        assert success
    
    def test_seq_add_fade_in(self, lightfx: ScaleFXConnection):
        """LED_SEQ_ADD with FADE_IN event."""
        lightfx.send(LightFxCommands.led_seq_clear(1))
        
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_seq_add_fade_in(1, 1000, 255)
        )
        assert success
    
    def test_seq_add_fade_out(self, lightfx: ScaleFXConnection):
        """LED_SEQ_ADD with FADE_OUT event."""
        lightfx.send(LightFxCommands.led_seq_clear(1))
        
        success, response = lightfx.send_expect_ack(
            LightFxCommands.led_seq_add_fade_out(1, 1000, 255)
        )
        assert success
    
    def test_seq_start_stop(self, lightfx: ScaleFXConnection):
        """LED_SEQ_START and LED_SEQ_STOP."""
        try:
            # Build a simple sequence
            lightfx.send(LightFxCommands.led_seq_clear(1))
            lightfx.send(LightFxCommands.led_seq_add_on(1, 500, 200))
            lightfx.send(LightFxCommands.led_seq_add_off(1, 500))
            
            # Start
            success, response = lightfx.send_expect_ack(
                LightFxCommands.led_seq_start(1)
            )
            assert success, "SEQ_START failed"
            
            time.sleep(0.5)
            
            # Stop
            success, response = lightfx.send_expect_ack(
                LightFxCommands.led_seq_stop(1)
            )
            assert success, "SEQ_STOP failed"
        finally:
            lightfx.send(LightFxCommands.led_off(0))
    
    @pytest.mark.slow
    def test_seq_breathing_effect(self, lightfx: ScaleFXConnection):
        """Test breathing LED effect sequence."""
        try:
            lightfx.send(LightFxCommands.led_seq_clear(1))
            
            # Fade in, hold, fade out, hold
            lightfx.send(LightFxCommands.led_seq_add_fade_in(1, 1000, 255))
            lightfx.send(LightFxCommands.led_seq_add_on(1, 500, 255))
            lightfx.send(LightFxCommands.led_seq_add_fade_out(1, 1000, 255))
            lightfx.send(LightFxCommands.led_seq_add_off(1, 500))
            
            success, _ = lightfx.send_expect_ack(LightFxCommands.led_seq_start(1))
            assert success
            
            # Let it run a cycle
            time.sleep(3.5)
            
            lightfx.send(LightFxCommands.led_seq_stop(1))
        finally:
            lightfx.send(LightFxCommands.led_off(0))


@pytest.mark.hardware
@pytest.mark.lightfx
class TestLightFxLedQueryResponses:
    """
    LED query command tests — verify typed data responses.
    
    These test the query response pattern where the server sends a typed
    data response packet (implicit ACK) instead of a plain ACK/NACK.
    """
    
    def test_led_status_response(self, lightfx: ScaleFXConnection):
        """LED_STATUS should return LED_STATUS_RESP with channel data."""
        response = lightfx.send_and_receive(LightFxCommands.led_status())
        assert response is not None, "No response to LED_STATUS"
        assert response.packet_type == LightFxPacket.LED_STATUS_RESP, (
            f"Expected LED_STATUS_RESP (0x{LightFxPacket.LED_STATUS_RESP:02X}), "
            f"got 0x{response.packet_type:02X}"
        )
        # 8 channels × 4 bytes each = 32 bytes
        assert len(response.payload) >= 32, (
            f"Expected ≥32 bytes, got {len(response.payload)}"
        )
    
    def test_led_status_shows_brightness(self, lightfx: ScaleFXConnection):
        """LED_STATUS should reflect the current brightness of set channels."""
        try:
            # Set a known brightness
            success, _ = lightfx.send_expect_ack(LightFxCommands.led_set(1, 200))
            assert success, "LED_SET failed"
            
            response = lightfx.send_and_receive(LightFxCommands.led_status())
            assert response is not None
            assert response.packet_type == LightFxPacket.LED_STATUS_RESP
            
            # Channel 1 brightness should be 200
            # Payload format: [ch:u8][brightness:u8][seq_playing:u8][seq_count:u8] per channel
            if len(response.payload) >= 8:
                ch1_brightness = response.payload[1]
                assert ch1_brightness == 200, (
                    f"Expected brightness 200, got {ch1_brightness}"
                )
        finally:
            lightfx.send(LightFxCommands.led_off(0))
    
    def test_led_seq_status_response(self, lightfx: ScaleFXConnection):
        """LED_SEQ_STATUS should return LED_SEQ_STATUS_RESP."""
        # Clear and add a sequence first
        lightfx.send_expect_ack(LightFxCommands.led_seq_clear(1))
        lightfx.send_expect_ack(LightFxCommands.led_seq_add_on(1, 1000, 200))
        
        response = lightfx.send_and_receive(LightFxCommands.led_seq_status(1))
        assert response is not None, "No response to LED_SEQ_STATUS"
        assert response.packet_type == LightFxPacket.LED_SEQ_STATUS_RESP, (
            f"Expected LED_SEQ_STATUS_RESP (0x{LightFxPacket.LED_SEQ_STATUS_RESP:02X}), "
            f"got 0x{response.packet_type:02X}"
        )
        # Minimum 8 bytes: [ch:u8][playing:u8][count:u8][index:u8][loopCount:u32]
        assert len(response.payload) >= 8, (
            f"Expected ≥8 bytes, got {len(response.payload)}"
        )
    
    def test_led_seq_status_shows_event_count(self, lightfx: ScaleFXConnection):
        """LED_SEQ_STATUS should report correct event count."""
        lightfx.send_expect_ack(LightFxCommands.led_seq_clear(1))
        lightfx.send_expect_ack(LightFxCommands.led_seq_add_on(1, 500, 200))
        lightfx.send_expect_ack(LightFxCommands.led_seq_add_off(1, 500))
        
        response = lightfx.send_and_receive(LightFxCommands.led_seq_status(1))
        assert response is not None
        assert response.packet_type == LightFxPacket.LED_SEQ_STATUS_RESP
        
        # payload[0] = channel, payload[2] = event_count
        event_count = response.payload[2]
        assert event_count == 2, f"Expected 2 events, got {event_count}"
    
    def test_led_seq_status_playing_flag(self, lightfx: ScaleFXConnection):
        """LED_SEQ_STATUS playing flag should reflect start/stop state."""
        try:
            lightfx.send_expect_ack(LightFxCommands.led_seq_clear(1))
            lightfx.send_expect_ack(LightFxCommands.led_seq_add_on(1, 5000, 200))
            
            # Not playing yet
            response = lightfx.send_and_receive(LightFxCommands.led_seq_status(1))
            assert response is not None
            assert response.payload[1] == 0, "Should not be playing before start"
            
            # Start sequence
            lightfx.send_expect_ack(LightFxCommands.led_seq_start(1))
            time.sleep(0.1)
            
            # Now playing
            response = lightfx.send_and_receive(LightFxCommands.led_seq_status(1))
            assert response is not None
            assert response.payload[1] != 0, "Should be playing after start"
            
            # Stop
            lightfx.send_expect_ack(LightFxCommands.led_seq_stop(1))
        finally:
            lightfx.send(LightFxCommands.led_off(0))
    
    def test_led_seq_queue_response(self, lightfx: ScaleFXConnection):
        """LED_SEQ_QUEUE should return LED_SEQ_QUEUE_RESP with event list."""
        lightfx.send_expect_ack(LightFxCommands.led_seq_clear(1))
        lightfx.send_expect_ack(LightFxCommands.led_seq_add_on(1, 1000, 200))
        lightfx.send_expect_ack(LightFxCommands.led_seq_add_off(1, 500))
        lightfx.send_expect_ack(LightFxCommands.led_seq_add_flash(1, 100, 2000, 255, 50))
        
        response = lightfx.send_and_receive(LightFxCommands.led_seq_queue(1))
        assert response is not None, "No response to LED_SEQ_QUEUE"
        assert response.packet_type == LightFxPacket.LED_SEQ_QUEUE_RESP, (
            f"Expected LED_SEQ_QUEUE_RESP (0x{LightFxPacket.LED_SEQ_QUEUE_RESP:02X}), "
            f"got 0x{response.packet_type:02X}"
        )
        # Header (4) + 3 events × 4 bytes = 16 bytes minimum
        assert len(response.payload) >= 16, (
            f"Expected ≥16 bytes, got {len(response.payload)}"
        )
        # Verify event count
        event_count = response.payload[1]
        assert event_count == 3, f"Expected 3 events, got {event_count}"
    
    def test_led_seq_queue_empty(self, lightfx: ScaleFXConnection):
        """LED_SEQ_QUEUE on cleared channel should return 0 events."""
        lightfx.send_expect_ack(LightFxCommands.led_seq_clear(2))
        
        response = lightfx.send_and_receive(LightFxCommands.led_seq_queue(2))
        assert response is not None
        assert response.packet_type == LightFxPacket.LED_SEQ_QUEUE_RESP
        
        event_count = response.payload[1]
        assert event_count == 0, f"Expected 0 events after clear, got {event_count}"
    
    def test_led_seq_status_invalid_channel(self, lightfx: ScaleFXConnection):
        """LED_SEQ_STATUS with invalid channel should NACK."""
        response = lightfx.send_and_receive(LightFxCommands.led_seq_status(9))
        assert response is not None
        assert response.is_nack, "Expected NACK for invalid channel"
        assert response.error_code == LightFxError.INVALID_CHANNEL
