# Mock I2S Testing Guide

## Overview

Mock I2S mode replaces the physical I2S hardware output with an in-memory buffer that captures audio data and collects statistics. This allows you to test the complete audio pipeline (SD card loading, WAV parsing, mixing, volume control) without requiring working codec hardware or speakers.

**Build 107+**

## When to Use Mock Mode

✅ **Use mock mode to test:**
- SD card audio file loading
- WAV file parsing and format support
- Multi-channel mixing logic
- Volume control (master and per-channel)
- Channel routing (stereo/left/right)
- Loop and fade functionality
- Sample rate handling

❌ **Mock mode does NOT test:**
- Physical I2S signal generation
- Codec I2C communication
- Speaker/headphone output
- Real-time audio playback timing

## Enabling Mock Mode

### Option 1: Edit audio_config.h

Edit [src/audio/audio_config.h](../src/audio/audio_config.h) and set:

```cpp
#define AUDIO_MOCK_I2S  1  // Enable mock mode
```

Then rebuild:
```bash
pio run --target upload
```

### Option 2: PlatformIO Build Flag

Add to `platformio.ini`:

```ini
[env:pico]
build_flags = 
    -D AUDIO_MOCK_I2S=1
```

## Available Commands

When `AUDIO_MOCK_I2S=1`, additional commands become available:

### Statistics Commands

```bash
audio stats           # Print detailed mock I2S statistics
audio stats reset     # Reset all statistics counters
```

### Regular Audio Commands (still work)

```bash
audio play 0 /sounds/test.wav     # Load and "play" file (to mock buffer)
audio play 1 /sounds/music.wav loop
audio stop 0
audio volume 0 0.5                # Test volume control
audio master 0.8                  # Test master volume
audio status                      # Show channel status
```

## Example Testing Session

```bash
> audio stats reset
[AudioMixer] Mock I2S statistics reset

> audio play 0 /sounds/startup.wav
[MAIN] Playing audio file: /sounds/startup.wav

> audio play 1 /sounds/engine_idle.wav loop vol 0.5

> audio master 0.8

# Wait a few seconds for audio to process

> audio stats

========================================
    MOCK I2S STATISTICS
========================================
Sample Rate:       44100 Hz
Running:           YES

Write Calls:       256
Total Samples:     131072 stereo pairs
Duration:          2.97 seconds
Real Time:         2.98 seconds
Realtime Ratio:    1.00x

--- LEVELS ---
Peak Left:         18924 (-4.7 dB)
Peak Right:        19102 (-4.6 dB)
RMS Left:          0.312 (-10.1 dB)
RMS Right:         0.319 (-10.0 dB)

--- QUALITY ---
Clipping (L/R):    0 / 0 events
Silent Samples:    2048 (1.6%)
Est. Freq (L/R):   440 / 440 Hz

--- CAPTURE ---
Captured Samples:  4096 / 4096
Buffer Full:       YES
========================================

> audio stop 0
> audio stop 1
> audio stats

# Review final statistics
```

## Statistics Explained

### Basic Info
- **Write Calls**: Number of times mixer wrote to I2S buffer
- **Total Samples**: Total stereo sample pairs processed
- **Duration**: Audio duration based on sample count
- **Real Time**: Actual elapsed time
- **Realtime Ratio**: How fast audio processed (1.0x = real-time)

### Levels
- **Peak**: Maximum sample value reached (-∞ to 0 dB)
- **RMS**: Root mean square level (average loudness)
- Values in dB relative to full scale (0 dB = maximum)

### Quality Indicators
- **Clipping**: Samples that hit maximum (±32767)
  - **0 events = good** (no distortion)
  - **>0 events = bad** (audio clipping, reduce volume)
  
- **Silent Samples**: Samples at exactly zero
  - High % may indicate gaps or quiet audio
  
- **Est. Freq**: Estimated frequency from zero crossings
  - Useful for verifying test tones (should match expected Hz)

### Capture Buffer
- First 4096 samples captured to memory buffer
- Use for detailed waveform inspection (future feature)

## Testing Scenarios

### Test 1: Single File Playback

```bash
audio stats reset
audio play 0 /sounds/test.wav
# Wait for completion
audio stats
```

**Verify:**
- Total samples match file duration
- No clipping events
- Peak levels reasonable (-6 to -1 dB)
- Est. frequency matches content

### Test 2: Multi-Channel Mixing

```bash
audio stats reset
audio play 0 /sounds/music.wav loop
audio play 1 /sounds/effects.wav
audio master 0.7
# Wait a few seconds
audio stats
```

**Verify:**
- Realtime ratio stays near 1.0x
- No clipping (if clipping occurs, reduce volumes)
- Peak levels don't exceed 0 dB

### Test 3: Volume Control

```bash
audio stats reset
audio play 0 /sounds/tone_440hz.wav loop
audio volume 0 0.1
# Wait 1 second
audio stats
# Note peak level

audio volume 0 0.5
# Wait 1 second
audio stats
# Peak should be ~14 dB higher

audio volume 0 1.0
# Wait 1 second
audio stats
# Peak should be near maximum
```

### Test 4: Channel Routing

```bash
# Test stereo
audio play 0 /sounds/stereo_test.wav

# Test left only
audio play 1 /sounds/test.wav left

# Test right only
audio play 2 /sounds/test.wav right

audio stats
# Check left vs right peak/RMS differences
```

## Performance Notes

With `AUDIO_MOCK_I2S=1`:
- **Realtime ratio should be ≥ 1.0x** for proper operation
- **<1.0x means mixer can't keep up** (increase buffer size or reduce load)
- **>1.0x is normal** (mixer processes faster than real-time)
- Mock mode adds minimal overhead (~1-2% CPU)

## Troubleshooting

### "Realtime Ratio: 0.8x" (Too Slow)

Audio mixer can't process fast enough:

**Solutions:**
1. Increase `AUDIO_MIX_BUFFER_SIZE` in audio_config.h (try 1024 or 2048)
2. Reduce number of simultaneous channels
3. Check SD card read speed (try different cards)
4. Verify Core 1 is running audio task

### "Clipping: 10000+ events"

Audio is distorting due to too much volume:

**Solutions:**
1. Reduce master volume: `audio master 0.5`
2. Reduce channel volumes: `audio volume 0 0.7`
3. Mix fewer channels simultaneously
4. Use audio files with lower peak levels

### "No Statistics After Playing"

Possible causes:
1. **AUDIO_MOCK_I2S not enabled** - Check build output for "⚠️ MOCK MODE" message
2. **Audio never started** - Check `audio status` shows channels playing
3. **Core 1 not running** - Mixer process() not being called

## Switching Back to Real Hardware

1. Edit `audio_config.h`:
   ```cpp
   #define AUDIO_MOCK_I2S  0  // Disable mock mode
   ```

2. Rebuild and upload:
   ```bash
   pio run --target upload
   ```

3. Boot message will no longer show "⚠️ MOCK MODE"

4. Mock statistics commands will not be available

## Files Modified

- [src/audio/audio_config.h](../src/audio/audio_config.h) - Added `AUDIO_MOCK_I2S` flag
- [src/audio/mock_i2s_sink.h](../src/audio/mock_i2s_sink.h) - Mock I2S interface
- [src/audio/mock_i2s_sink.cpp](../src/audio/mock_i2s_sink.cpp) - Implementation with statistics
- [src/audio/audio_mixer.cpp](../src/audio/audio_mixer.cpp) - Conditional compilation for mock mode
- [src/cli/audio_cli.cpp](../src/cli/audio_cli.cpp) - Added `audio stats` commands

## See Also

- [AUDIO_TROUBLESHOOTING.md](AUDIO_TROUBLESHOOTING.md) - Hardware audio debugging
- [WIRING.md](WIRING.md) - Physical wiring guide
- [audio_config.h](../src/audio/audio_config.h) - All audio configuration options
