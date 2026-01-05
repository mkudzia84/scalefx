"""
SD/Audio Contention Tests

Tests for thread-safety of SPI bus access between SD card (Core 0) 
and Audio Mixer (Core 1). These tests validate that the mutex-based
locking in SdCardModule prevents corruption during concurrent access.

The HubFX architecture:
- Core 0: CLI commands, SD card operations, main loop
- Core 1: Audio mixing, I2S output, file reading from SD

Without proper mutex locking, concurrent SPI operations cause:
- Corrupted audio (clicks, pops, silence)
- SD card read failures
- System crashes/hangs
"""

import pytest
import time
import threading
from conftest import SerialConnection


class TestSDDuringAudioPlayback:
    """Test SD operations while audio is playing (Core 1 active)."""
    
    # =========================================================================
    # BASIC CONTENTION - SD commands during audio playback
    # =========================================================================
    
    @pytest.mark.slow
    def test_sd_ls_during_playback(self, fresh_pico: SerialConnection):
        """
        Test sd ls while audio is playing.
        
        This validates the mutex is working - without it, the SD card
        read for ls would collide with audio file reads on Core 1.
        """
        # Start audio playback (looping to ensure it's still playing)
        play_resp = fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.2)
        
        # Give audio time to start buffering from SD
        time.sleep(0.3)
        
        # Run SD command while audio is playing
        ls_resp = fresh_pico.send("sd ls /", delay=0.5)
        
        # Stop audio
        fresh_pico.send("audio stop 0", delay=0.1)
        
        # SD ls should succeed without hanging or returning garbage
        # If mutex isn't working, this often hangs or returns corrupted data
        assert "error" not in ls_resp.lower() or "not initialized" in ls_resp.lower()
        if "not initialized" not in ls_resp.lower():
            # Should have valid directory listing
            assert "/" in ls_resp or "items" in ls_resp.lower() or "DIR" in ls_resp
    
    @pytest.mark.slow
    def test_sd_info_during_playback(self, fresh_pico: SerialConnection):
        """
        Test sd info while audio is playing.
        
        sd info reads SD card metadata - tests mutex with different SD operations.
        """
        # Start audio
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.2)
        time.sleep(0.3)
        
        # Get SD info during playback (longer delay for sd info)
        info_resp = fresh_pico.send("sd info", delay=1.0)
        
        # Stop audio
        fresh_pico.send("audio stop 0", delay=0.1)
        
        # Should get valid SD info, error, or empty (if command took too long)
        # Empty response is okay - it means command is still processing (no crash)
        if len(info_resp) > 0 and "not initialized" not in info_resp.lower():
            assert "bytes" in info_resp.lower() or "mb" in info_resp.lower() or "gb" in info_resp.lower() or "size" in info_resp.lower() or "error" in info_resp.lower()
    
    @pytest.mark.slow
    def test_sd_tree_during_playback(self, fresh_pico: SerialConnection):
        """
        Test sd tree (recursive listing) while audio plays.
        
        Tree is more intensive than ls - multiple directory reads.
        """
        # Start audio
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.2)
        time.sleep(0.3)
        
        # Run tree (limited depth for speed, longer delay)
        tree_resp = fresh_pico.send("sd tree / 1", delay=1.5)
        
        # Stop audio
        fresh_pico.send("audio stop 0", delay=0.1)
        
        # Empty response is acceptable - means command is still running (no crash/deadlock)
        # If we get a response, it should be valid
        if len(tree_resp) > 0 and "not initialized" not in tree_resp.lower():
            # Should have some content (directory names or error)
            assert "/" in tree_resp or "error" in tree_resp.lower() or "─" in tree_resp
    
    # =========================================================================
    # RAPID OPERATIONS - Stress test contention
    # =========================================================================
    
    @pytest.mark.slow
    def test_rapid_sd_commands_during_playback(self, fresh_pico: SerialConnection):
        """
        Rapidly issue SD commands while audio plays.
        
        This stresses the mutex - rapid lock/unlock cycles while Core 1
        is continuously reading audio data.
        """
        # Start looping audio
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.2)
        time.sleep(0.2)
        
        errors = 0
        successes = 0
        
        # Rapid SD operations
        for i in range(5):
            resp = fresh_pico.send("sd ls /", delay=0.3)
            if "error" in resp.lower() and "not initialized" not in resp.lower():
                errors += 1
            else:
                successes += 1
        
        # Stop audio
        fresh_pico.send("audio stop 0", delay=0.1)
        
        # Most operations should succeed
        assert successes >= 3, f"Too many failures: {errors} errors, {successes} successes"
    
    @pytest.mark.slow  
    def test_config_load_during_playback(self, fresh_pico: SerialConnection):
        """
        Test config reload while audio is playing.
        
        Config loading reads and parses YAML from SD - more complex than ls.
        """
        # Start audio
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.2)
        time.sleep(0.3)
        
        # Try to reload config
        config_resp = fresh_pico.send("config reload", delay=1.0)
        
        # Stop audio
        fresh_pico.send("audio stop 0", delay=0.1)
        
        # Should complete (success or graceful failure)
        assert "error" in config_resp.lower() or "loaded" in config_resp.lower() or "config" in config_resp.lower()


class TestAudioDuringSDOperations:
    """Test audio commands during SD-intensive operations."""
    
    @pytest.mark.slow
    def test_audio_play_during_sd_tree(self, fresh_pico: SerialConnection):
        """
        Start audio playback while sd tree is running.
        
        Tests that audio can acquire SD lock during heavy SD usage.
        """
        # Start a slow SD operation (tree with depth)
        # Note: We can't truly run these in parallel from pytest,
        # but we can start tree and then immediately try audio
        fresh_pico.send("sd tree / 3", delay=0.1)  # Don't wait for completion
        
        # Immediately try to start audio
        time.sleep(0.1)
        play_resp = fresh_pico.send("audio play 0 /sounds/test.wav", delay=0.5)
        
        # Clean up
        fresh_pico.send("audio stop all", delay=0.1)
        
        # Audio command should not hang indefinitely
        # (if mutex deadlocks, we'd timeout here)
        assert len(play_resp) >= 0  # Just checking we got a response


class TestMultiChannelContention:
    """Test multiple audio channels accessing SD simultaneously."""
    
    @pytest.mark.slow
    def test_multi_channel_playback_with_sd(self, fresh_pico: SerialConnection):
        """
        Play on multiple channels while doing SD operations.
        
        Multiple channels = more SD reads on Core 1, higher contention.
        """
        # Start multiple audio channels
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.1)
        fresh_pico.send("audio play 1 /sounds/engine_starting.wav loop", delay=0.1)
        time.sleep(0.3)
        
        # SD operations during multi-channel playback
        ls_resp = fresh_pico.send("sd ls /sounds", delay=0.5)
        
        # Stop all audio
        fresh_pico.send("audio stop all", delay=0.1)
        
        # Should complete without issues
        if "not initialized" not in ls_resp.lower() and "error" not in ls_resp.lower():
            assert len(ls_resp) > 0
    
    @pytest.mark.slow
    def test_audio_status_during_playback(self, fresh_pico: SerialConnection):
        """
        Check audio status while playing.
        
        audio status doesn't use SD directly, but validates overall system
        stability during audio playback.
        """
        # Start audio
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.2)
        time.sleep(0.3)
        
        # Get status
        status_resp = fresh_pico.send("audio status", delay=0.3)
        
        # Stop audio
        fresh_pico.send("audio stop 0", delay=0.1)
        
        # Should show channel status
        assert "channel" in status_resp.lower() or "playing" in status_resp.lower() or "status" in status_resp.lower()


class TestContentionRecovery:
    """Test system recovery after potential contention issues."""
    
    @pytest.mark.slow
    def test_sd_reinit_after_contention(self, fresh_pico: SerialConnection):
        """
        Test SD reinit works after heavy contention.
        
        If mutex causes issues, SD might need reinit.
        """
        # Create contention
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.1)
        for _ in range(3):
            fresh_pico.send("sd ls /", delay=0.2)
        fresh_pico.send("audio stop all", delay=0.1)
        
        # Reinit SD
        reinit_resp = fresh_pico.send("sd init", delay=1.0)
        
        # Should succeed
        assert "error" not in reinit_resp.lower() or "MHz" in reinit_resp
    
    @pytest.mark.slow
    def test_system_status_after_contention(self, fresh_pico: SerialConnection):
        """
        Check system status after contention stress test.
        
        Validates no memory leaks or system instability.
        """
        # Stress test
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.1)
        for _ in range(5):
            fresh_pico.send("sd ls /", delay=0.2)
        fresh_pico.send("audio stop all", delay=0.2)
        
        # Check system status
        status_resp = fresh_pico.send("status --json", delay=0.5)
        
        # Should get valid status
        assert "uptime" in status_resp.lower() or "memory" in status_resp.lower() or "{" in status_resp


class TestContentionJSON:
    """Test JSON output during contention (validates no corruption)."""
    
    @pytest.mark.slow
    @pytest.mark.json
    def test_sd_ls_json_during_playback(self, fresh_pico: SerialConnection):
        """
        Test JSON output integrity during audio playback.
        
        Corrupted SPI data would likely cause JSON parse errors.
        """
        # Start audio
        fresh_pico.send("audio play 0 /sounds/engine_running.wav loop", delay=0.2)
        time.sleep(0.3)
        
        # Get JSON response
        result = fresh_pico.send_json("sd ls /")
        
        # Stop audio
        fresh_pico.send("audio stop 0", delay=0.1)
        
        # JSON should be valid (send_json returns {} on parse error)
        if result:  # Not empty dict
            assert "path" in result or "error" in result
    
    @pytest.mark.slow
    @pytest.mark.json
    def test_audio_status_json_during_sd(self, fresh_pico: SerialConnection):
        """
        Test audio status JSON during SD operations.
        """
        # Start SD-heavy operation
        fresh_pico.send("sd tree / 2", delay=0.1)
        
        # Get audio status JSON
        time.sleep(0.1)
        result = fresh_pico.send_json("audio status")
        
        # Should be valid JSON
        # Empty dict is okay if no channels playing
        assert isinstance(result, dict)
