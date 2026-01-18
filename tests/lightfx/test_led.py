"""
LightFX LED Command Tests

Tests for LED_SET, LED_OFF, and LED sequence commands.
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
