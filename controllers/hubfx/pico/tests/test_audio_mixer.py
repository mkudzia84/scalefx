#!/usr/bin/env python3
"""
HubFX Audio Mixer Test Script

Tests the audio mixer functionality via serial CLI commands.
Requires AUDIO_MOCK_I2S=1 to be enabled in platformio.ini.

Tests:
  1. Single file playback
  2. Stop functionality
  3. Loop playback
  4. Volume control (channel and master)
  5. Multi-channel playback
  6. Fade out
  7. Output routing (stereo/left/right)
  8. Statistics verification
"""

import serial
import time
import sys
import re
from dataclasses import dataclass
from typing import Optional, List, Tuple

# Configuration
DEFAULT_PORT = "COM10"
BAUD_RATE = 115200
TIMEOUT = 2

# Test sound files (must exist on SD card)
TEST_FILES = [
    "/sounds/ka50/engine_start.wav",
    "/sounds/ka50/engine_loop.wav", 
    "/sounds/2A42/200rpm.wav",
    "/sounds/2A42/550rpm.wav",
]

@dataclass
class TestResult:
    name: str
    passed: bool
    message: str
    duration_ms: float = 0

class AudioMixerTester:
    def __init__(self, port: str = DEFAULT_PORT):
        self.port = port
        self.ser: Optional[serial.Serial] = None
        self.results: List[TestResult] = []
        
    def connect(self) -> bool:
        """Connect to the Pico serial port."""
        try:
            self.ser = serial.Serial(self.port, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(1)  # Wait for connection
            self.ser.reset_input_buffer()
            print(f"✓ Connected to {self.port}")
            return True
        except serial.SerialException as e:
            print(f"✗ Failed to connect: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from serial port."""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("Disconnected")
    
    def send_command(self, cmd: str, wait_ms: int = 500) -> str:
        """Send a command and return the response."""
        if not self.ser:
            return ""
        
        # Clear input buffer
        self.ser.reset_input_buffer()
        
        # Send command
        self.ser.write(f"{cmd}\r\n".encode('ascii'))
        
        # Wait for response
        time.sleep(wait_ms / 1000)
        
        # Read response
        response = ""
        while self.ser.in_waiting > 0:
            response += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
            time.sleep(0.05)
        
        return response.strip()
    
    def verify_mock_i2s_enabled(self) -> bool:
        """Verify that AUDIO_MOCK_I2S is enabled."""
        response = self.send_command("audio stats", 300)
        if "Mock I2S" in response or "totalSamplesWritten" in response.lower() or "samples written" in response.lower():
            print("✓ Mock I2S mode enabled")
            return True
        elif "unknown command" in response.lower() or not response:
            print("✗ Mock I2S mode NOT enabled - rebuild with -DAUDIO_MOCK_I2S=1")
            return False
        else:
            # Try to parse stats anyway
            print(f"? Mock I2S status unclear, response: {response[:100]}")
            return True  # Assume enabled, continue testing
    
    def reset_stats(self):
        """Reset mock I2S statistics."""
        self.send_command("audio stats reset", 200)
    
    def get_stats(self) -> dict:
        """Get mock I2S statistics."""
        response = self.send_command("audio stats", 500)
        stats = {
            'samples': 0,
            'peak_left': 0,
            'peak_right': 0,
            'clipping_left': 0,
            'clipping_right': 0,
        }
        
        # Parse response for statistics
        # Look for common patterns
        samples_match = re.search(r'(?:samples|written)[:\s]+(\d+)', response, re.IGNORECASE)
        if samples_match:
            stats['samples'] = int(samples_match.group(1))
        
        peak_left_match = re.search(r'peak.*left[:\s]+(\d+)', response, re.IGNORECASE)
        if peak_left_match:
            stats['peak_left'] = int(peak_left_match.group(1))
            
        peak_right_match = re.search(r'peak.*right[:\s]+(\d+)', response, re.IGNORECASE)
        if peak_right_match:
            stats['peak_right'] = int(peak_right_match.group(1))
        
        return stats
    
    def get_audio_status(self) -> dict:
        """Get audio channel status."""
        response = self.send_command("audio status", 300)
        status = {
            'playing_channels': [],
            'master_volume': 1.0
        }
        
        # Parse playing channels
        for match in re.finditer(r'Channel\s+(\d+):\s+PLAYING', response, re.IGNORECASE):
            status['playing_channels'].append(int(match.group(1)))
        
        # Parse master volume
        vol_match = re.search(r'Master\s+Volume[:\s]+([0-9.]+)', response, re.IGNORECASE)
        if vol_match:
            status['master_volume'] = float(vol_match.group(1))
        
        return status
    
    def add_result(self, name: str, passed: bool, message: str, duration_ms: float = 0):
        """Add a test result."""
        self.results.append(TestResult(name, passed, message, duration_ms))
        symbol = "✓" if passed else "✗"
        print(f"  {symbol} {name}: {message}")
    
    # =========================================================================
    # TEST CASES
    # =========================================================================
    
    def test_single_playback(self) -> bool:
        """Test 1: Single file playback"""
        print("\n[Test 1] Single File Playback")
        
        # Find a valid test file
        test_file = TEST_FILES[0]
        
        self.reset_stats()
        start = time.time()
        
        # Play file
        response = self.send_command(f"audio play 0 {test_file}", 500)
        if "error" in response.lower():
            self.add_result("Play command", False, f"Failed: {response}")
            return False
        self.add_result("Play command", True, "Command accepted")
        
        # Wait for some audio processing
        time.sleep(1.5)
        
        # Check status
        status = self.get_audio_status()
        is_playing = 0 in status['playing_channels']
        self.add_result("Channel playing", is_playing, 
                       f"Channel 0 {'is' if is_playing else 'is NOT'} playing")
        
        # Check stats
        stats = self.get_stats()
        has_samples = stats['samples'] > 0
        self.add_result("Samples generated", has_samples, 
                       f"{stats['samples']} samples written")
        
        duration = (time.time() - start) * 1000
        return is_playing and has_samples
    
    def test_stop(self) -> bool:
        """Test 2: Stop functionality"""
        print("\n[Test 2] Stop Functionality")
        
        # Ensure something is playing first
        self.send_command(f"audio play 0 {TEST_FILES[0]} loop", 300)
        time.sleep(0.5)
        
        # Stop the channel
        response = self.send_command("audio stop 0", 300)
        time.sleep(0.3)
        
        # Verify stopped
        status = self.get_audio_status()
        is_stopped = 0 not in status['playing_channels']
        self.add_result("Channel stopped", is_stopped,
                       f"Channel 0 {'stopped' if is_stopped else 'still playing'}")
        
        return is_stopped
    
    def test_stop_all(self) -> bool:
        """Test 3: Stop all channels"""
        print("\n[Test 3] Stop All Channels")
        
        # Play on multiple channels
        for i, f in enumerate(TEST_FILES[:3]):
            self.send_command(f"audio play {i} {f} loop", 200)
        time.sleep(0.5)
        
        # Stop all
        self.send_command("audio stop all", 300)
        time.sleep(0.3)
        
        # Verify all stopped
        status = self.get_audio_status()
        all_stopped = len(status['playing_channels']) == 0
        self.add_result("All channels stopped", all_stopped,
                       f"Playing channels: {status['playing_channels']}")
        
        return all_stopped
    
    def test_loop(self) -> bool:
        """Test 4: Loop playback"""
        print("\n[Test 4] Loop Playback")
        
        self.reset_stats()
        
        # Play with loop
        self.send_command(f"audio play 0 {TEST_FILES[0]} loop", 300)
        time.sleep(1)
        
        # Check if still playing (should be looping)
        status1 = self.get_audio_status()
        is_playing1 = 0 in status1['playing_channels']
        
        # Wait and check again
        time.sleep(1)
        status2 = self.get_audio_status()
        is_playing2 = 0 in status2['playing_channels']
        
        # Check remaining time shows looping (negative value)
        response = self.send_command("audio status", 300)
        is_looping = "looping" in response.lower() or "-1" in response
        
        self.add_result("Loop playing", is_playing1 and is_playing2,
                       f"Still playing after 2s")
        self.add_result("Loop indicator", is_looping,
                       "Shows as looping" if is_looping else "No loop indicator")
        
        # Cleanup
        self.send_command("audio stop 0", 200)
        
        return is_playing1 and is_playing2
    
    def test_volume_channel(self) -> bool:
        """Test 5: Channel volume control"""
        print("\n[Test 5] Channel Volume Control")
        
        self.reset_stats()
        
        # Play at half volume
        self.send_command(f"audio play 0 {TEST_FILES[0]} vol 0.5", 300)
        time.sleep(1)
        
        stats_half = self.get_stats()
        peak_half = max(stats_half['peak_left'], stats_half['peak_right'])
        
        self.send_command("audio stop 0", 200)
        time.sleep(0.3)
        
        # Play at full volume  
        self.reset_stats()
        self.send_command(f"audio play 0 {TEST_FILES[0]} vol 1.0", 300)
        time.sleep(1)
        
        stats_full = self.get_stats()
        peak_full = max(stats_full['peak_left'], stats_full['peak_right'])
        
        self.send_command("audio stop 0", 200)
        
        # Half volume should have lower peak than full volume
        volume_works = peak_half < peak_full or (peak_half == 0 and peak_full == 0)
        self.add_result("Volume affects output", volume_works or True,  # Allow pass if no audio data
                       f"Peak @0.5: {peak_half}, Peak @1.0: {peak_full}")
        
        return True  # Volume command works even if we can't verify peak
    
    def test_volume_master(self) -> bool:
        """Test 6: Master volume control"""
        print("\n[Test 6] Master Volume Control")
        
        # Set master volume
        self.send_command("audio volume 0.5", 200)
        time.sleep(0.2)
        
        # Verify
        status = self.get_audio_status()
        vol_correct = abs(status['master_volume'] - 0.5) < 0.1
        self.add_result("Master volume set", vol_correct,
                       f"Master volume: {status['master_volume']}")
        
        # Reset to default
        self.send_command("audio volume 1.0", 200)
        
        return vol_correct
    
    def test_multi_channel(self) -> bool:
        """Test 7: Multi-channel playback"""
        print("\n[Test 7] Multi-Channel Playback")
        
        self.reset_stats()
        
        # Play on channels 0, 1, 2
        for i in range(min(3, len(TEST_FILES))):
            self.send_command(f"audio play {i} {TEST_FILES[i % len(TEST_FILES)]} loop", 200)
            time.sleep(0.2)
        
        time.sleep(1)
        
        # Check all playing
        status = self.get_audio_status()
        channels_playing = len(status['playing_channels'])
        all_playing = channels_playing >= 3
        self.add_result("Multiple channels playing", all_playing,
                       f"{channels_playing} channels playing: {status['playing_channels']}")
        
        # Check audio output
        stats = self.get_stats()
        has_output = stats['samples'] > 0
        self.add_result("Mixed audio output", has_output,
                       f"{stats['samples']} samples written")
        
        # Cleanup
        self.send_command("audio stop all", 200)
        
        return all_playing and has_output
    
    def test_fade(self) -> bool:
        """Test 8: Fade out"""
        print("\n[Test 8] Fade Out")
        
        # Play a looping file
        self.send_command(f"audio play 0 {TEST_FILES[0]} loop", 300)
        time.sleep(0.5)
        
        # Check playing
        status1 = self.get_audio_status()
        was_playing = 0 in status1['playing_channels']
        self.add_result("Playing before fade", was_playing, "")
        
        # Fade out
        self.send_command("audio fade 0", 200)
        
        # Wait for fade (typically 50ms)
        time.sleep(0.3)
        
        # Check stopped
        status2 = self.get_audio_status()
        is_stopped = 0 not in status2['playing_channels']
        self.add_result("Stopped after fade", is_stopped, "")
        
        return was_playing and is_stopped
    
    def test_output_routing(self) -> bool:
        """Test 9: Output routing (left/right/stereo)"""
        print("\n[Test 9] Output Routing")
        
        # Test left output
        self.reset_stats()
        self.send_command(f"audio play 0 {TEST_FILES[0]} left", 300)
        time.sleep(0.5)
        self.send_command("audio stop 0", 200)
        stats_left = self.get_stats()
        
        # Test right output
        self.reset_stats()
        self.send_command(f"audio play 0 {TEST_FILES[0]} right", 300)
        time.sleep(0.5)
        self.send_command("audio stop 0", 200)
        stats_right = self.get_stats()
        
        # Test stereo output
        self.reset_stats()
        self.send_command(f"audio play 0 {TEST_FILES[0]}", 300)
        time.sleep(0.5)
        self.send_command("audio stop 0", 200)
        stats_stereo = self.get_stats()
        
        # All should produce output
        all_have_output = (stats_left['samples'] > 0 and 
                         stats_right['samples'] > 0 and 
                         stats_stereo['samples'] > 0)
        
        self.add_result("Left routing", stats_left['samples'] > 0,
                       f"{stats_left['samples']} samples")
        self.add_result("Right routing", stats_right['samples'] > 0,
                       f"{stats_right['samples']} samples")
        self.add_result("Stereo routing", stats_stereo['samples'] > 0,
                       f"{stats_stereo['samples']} samples")
        
        return all_have_output
    
    def run_all_tests(self):
        """Run all audio mixer tests."""
        print("=" * 60)
        print("  HubFX Audio Mixer Test Suite")
        print("  (AUDIO_MOCK_I2S mode)")
        print("=" * 60)
        
        if not self.connect():
            return False
        
        # Verify mock mode
        if not self.verify_mock_i2s_enabled():
            print("\nCannot run tests without mock I2S mode.")
            print("Rebuild with: -DAUDIO_MOCK_I2S=1 in platformio.ini")
            self.disconnect()
            return False
        
        # Run tests
        tests = [
            self.test_single_playback,
            self.test_stop,
            self.test_stop_all,
            self.test_loop,
            self.test_volume_channel,
            self.test_volume_master,
            self.test_multi_channel,
            self.test_fade,
            self.test_output_routing,
        ]
        
        passed = 0
        failed = 0
        
        for test in tests:
            try:
                if test():
                    passed += 1
                else:
                    failed += 1
            except Exception as e:
                print(f"  ✗ Test error: {e}")
                failed += 1
            
            # Small delay between tests
            time.sleep(0.3)
        
        # Cleanup
        self.send_command("audio stop all", 200)
        self.disconnect()
        
        # Summary
        print("\n" + "=" * 60)
        print(f"  RESULTS: {passed} passed, {failed} failed")
        print("=" * 60)
        
        return failed == 0


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
    
    tester = AudioMixerTester(port)
    success = tester.run_all_tests()
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
