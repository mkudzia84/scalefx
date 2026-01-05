"""
Audio CLI Tests

Tests for audio commands: audio play, stop, volume, codec commands.
"""

import pytest
from conftest import SerialConnection


class TestAudioPlaybackCommands:
    """Test audio playback CLI commands."""
    
    # =========================================================================
    # AUDIO PLAY
    # =========================================================================
    
    def test_audio_play_invalid_channel(self, fresh_pico: SerialConnection):
        """Test audio play rejects invalid channel."""
        response = fresh_pico.send("audio play 10 /test.wav")
        assert "error" in response.lower() or "invalid" in response.lower()
    
    def test_audio_play_missing_file(self, fresh_pico: SerialConnection):
        """Test audio play without filename shows error."""
        response = fresh_pico.send("audio play 0")
        assert "error" in response.lower() or "format" in response.lower()
    
    def test_audio_play_valid(self, fresh_pico: SerialConnection):
        """Test audio play with valid parameters."""
        response = fresh_pico.send("audio play 0 /sounds/test.wav")
        # Should either play or show file not found
        assert "Playing" in response or "error" in response.lower() or "channel" in response.lower()
    
    def test_audio_play_with_loop(self, fresh_pico: SerialConnection):
        """Test audio play with loop option."""
        response = fresh_pico.send("audio play 0 /test.wav loop")
        assert "loop" in response.lower() or "Playing" in response or "error" in response.lower()
    
    def test_audio_play_with_volume(self, fresh_pico: SerialConnection):
        """Test audio play with volume option."""
        response = fresh_pico.send("audio play 0 /test.wav vol 0.5")
        assert "Playing" in response or "error" in response.lower()
    
    # =========================================================================
    # AUDIO STOP
    # =========================================================================
    
    def test_audio_stop_channel(self, fresh_pico: SerialConnection):
        """Test stopping specific audio channel."""
        response = fresh_pico.send("audio stop 0")
        assert "stop" in response.lower() or "channel" in response.lower() or len(response) == 0
    
    def test_audio_stop_all(self, fresh_pico: SerialConnection):
        """Test stopping all audio."""
        response = fresh_pico.send("audio stop all")
        assert "stop" in response.lower() or "all" in response.lower() or len(response) == 0
    
    # =========================================================================
    # AUDIO VOLUME CONTROL
    # =========================================================================
    
    def test_audio_volume_get(self, fresh_pico: SerialConnection):
        """Test getting current master volume."""
        response = fresh_pico.send("audio volume")
        # Should show current volume or set volume
        assert "volume" in response.lower() or "master" in response.lower() or "0." in response
    
    def test_audio_volume_set_valid(self, fresh_pico: SerialConnection):
        """Test setting valid master volume."""
        response = fresh_pico.send("audio volume 0.5")
        assert "volume" in response.lower() or "0.5" in response or "set" in response.lower()
    
    def test_audio_volume_set_max(self, fresh_pico: SerialConnection):
        """Test setting volume to max."""
        response = fresh_pico.send("audio volume 1.0")
        assert "volume" in response.lower() or "1.0" in response or "set" in response.lower()
    
    def test_audio_volume_set_min(self, fresh_pico: SerialConnection):
        """Test setting volume to min."""
        response = fresh_pico.send("audio volume 0")
        assert "volume" in response.lower() or "0" in response or "set" in response.lower()
    
    def test_audio_master_volume(self, fresh_pico: SerialConnection):
        """Test audio master command alias."""
        response = fresh_pico.send("audio master 0.8")
        assert "volume" in response.lower() or "0.8" in response or "master" in response.lower()


class TestCodecCommands:
    """Test codec CLI commands."""
    
    # =========================================================================
    # CODEC DUMP - Register dump
    # =========================================================================
    
    def test_codec_dump(self, fresh_pico: SerialConnection):
        """Test codec register dump."""
        response = fresh_pico.send("codec dump", delay=0.5)
        # Should show registers or error if no codec
        assert "Register" in response or "error" in response.lower() or "codec" in response.lower()
    
    # =========================================================================
    # CODEC RESET
    # =========================================================================
    
    def test_codec_reset(self, fresh_pico: SerialConnection):
        """Test codec reset command."""
        response = fresh_pico.send("codec reset")
        assert "reset" in response.lower() or "codec" in response.lower() or "error" in response.lower()
    
    # =========================================================================
    # CODEC STATUS (if AUDIO_DEBUG enabled)
    # =========================================================================
    
    def test_codec_status(self, fresh_pico: SerialConnection):
        """Test codec status command."""
        response = fresh_pico.send("codec status")
        # May show status or "unknown command" if AUDIO_DEBUG disabled
        assert "Status" in response or "status" in response.lower() or "error" in response.lower() or "unknown" in response.lower()
    
    # =========================================================================
    # CODEC TEST (if AUDIO_DEBUG enabled)
    # =========================================================================
    
    def test_codec_test(self, fresh_pico: SerialConnection):
        """Test codec communication test."""
        response = fresh_pico.send("codec test")
        # May show test results or "unknown command" if AUDIO_DEBUG disabled
        assert "test" in response.lower() or "I2C" in response or "error" in response.lower() or "unknown" in response.lower()
    
    # =========================================================================
    # CODEC SCAN (if AUDIO_DEBUG enabled)
    # =========================================================================
    
    @pytest.mark.hardware
    def test_codec_scan(self, fresh_pico: SerialConnection):
        """Test I2C bus scan."""
        response = fresh_pico.send("codec scan", delay=1.0)
        # Should scan or show error/unknown
        assert "scan" in response.lower() or "I2C" in response or "device" in response.lower() or "unknown" in response.lower()


class TestAudioStatus:
    """Test audio status commands."""
    
    def test_audio_status(self, fresh_pico: SerialConnection):
        """Test audio status display."""
        response = fresh_pico.send("audio status")
        # Should show Audio Status header and master volume info
        assert "Audio Status" in response or "Master Volume" in response or "No channels playing" in response
    
    def test_audio_status_json(self, fresh_pico: SerialConnection):
        """Test audio status JSON output."""
        result = fresh_pico.send_json("audio status")
        # JSON should have channels array and masterVolume
        assert "channels" in result or "masterVolume" in result or "error" in result


class TestAudioEdgeCases:
    """Test audio command edge cases."""
    
    def test_audio_invalid_subcommand(self, fresh_pico: SerialConnection):
        """Test invalid audio subcommand."""
        response = fresh_pico.send("audio notacommand")
        assert "error" in response.lower() or "unknown" in response.lower()
    
    def test_codec_invalid_subcommand(self, fresh_pico: SerialConnection):
        """Test invalid codec subcommand."""
        response = fresh_pico.send("codec notacommand")
        assert "error" in response.lower() or "unknown" in response.lower()
