# HubFX Audio System Configuration

## Overview

The HubFX audio system uses **compile-time configuration** for optimal performance and flexibility. All configurable parameters are centralized in [`../src/audio/audio_config.h`](../src/audio/audio_config.h).

## Quick Start

### Method 1: Edit audio_config.h (Recommended)

Modify the defines directly in `src/audio/audio_config.h`:

```cpp
#define AUDIO_SAMPLE_RATE           48000   // Change to 48 kHz
#define WM8960_I2C_SPEED            100000  // Try 100 kHz I2C
#define AUDIO_MIX_BUFFER_SIZE       256     // Lower latency
```

### Method 2: Override in platformio.ini

Add compile flags to override specific settings without editing headers:

```ini
build_flags = 
    ...existing flags...
    -DAUDIO_SAMPLE_RATE=48000
    -DWM8960_I2C_SPEED=100000
    -DAUDIO_DEBUG_TIMING=1
```

## Configurable Parameters

### I2S Audio Settings

| Parameter | Default | Description | Safe Range |
|-----------|---------|-------------|------------|
| `AUDIO_SAMPLE_RATE` | 44100 | Sample rate (Hz) | 8000-192000 |
| `AUDIO_BIT_DEPTH` | 16 | Bits per sample | 16, 24, 32 |
| `I2S_BITS_PER_CHANNEL` | 32 | I2S frame size | 32 (fixed) |

**Sample Rate Notes:**
- **44100 Hz:** CD quality, most compatible
- **48000 Hz:** Pro audio standard, slightly higher quality
- **22050 Hz:** Half CD, for lower bitrate files
- **96000+ Hz:** High-res audio, requires more CPU/RAM

⚠️ **Warning:** Changing sample rate requires recalculating `WM8960_PLL_K_VALUE`! Supported rates in `audio_config.h`: 44.1k, 48k, 22.05k

### WM8960 Codec Settings

| Parameter | Default | Description | When to Change |
|-----------|---------|-------------|----------------|
| `WM8960_I2C_SPEED` | 50000 | I2C bus speed (Hz) | Good wiring: 100000<br>Poor wiring: 10000 |
| `WM8960_PLL_K_VALUE` | 0x0C93E9 | PLL fractional multiplier | Auto-calculated for sample rate |

**I2C Speed Troubleshooting:**
- **Error 5 (timeout):** Lower speed to 50000 or 10000
- **Error 4 (no device):** Check power connections (not an I2C speed issue)
- **Good wiring** (PCB, <3" wires): Try 100000
- **Poor wiring** (breadboard, >6" wires): Use 50000 or lower

### Audio Mixer Settings

| Parameter | Default | Description | Trade-off |
|-----------|---------|-------------|-----------|
| `AUDIO_MAX_CHANNELS` | 8 | Simultaneous sounds | More = higher CPU/RAM |
| `AUDIO_MIX_BUFFER_SIZE` | 512 | DMA buffer size (samples) | Smaller = lower latency, more CPU |
| `AUDIO_STREAM_BUFFER_SIZE` | 2048 | SD read buffer (bytes) | Larger = fewer SD accesses |

**Buffer Size Guidelines:**
- **Low latency** (game sounds): 128-256 samples
- **Balanced** (general use): 512-1024 samples
- **High efficiency** (ambient loops): 2048-4096 samples

At 44.1 kHz:
- 256 samples = 5.8 ms latency
- 512 samples = 11.6 ms latency
- 1024 samples = 23.2 ms latency

### Debug Settings

| Parameter | Default | Description |
|-----------|---------|-------------|
| `AUDIO_DEBUG_TIMING` | 0 | Print I2S clock frequencies at boot |
| `AUDIO_DEBUG_CODEC_REGS` | 0 | Dump codec registers after init |
| `I2S_WIRE_LENGTH_WARNING` | 1 | Warn about timing sensitivity |

Enable for troubleshooting:
```ini
build_flags = 
    -DAUDIO_DEBUG_TIMING=1
    -DAUDIO_DEBUG_CODEC_REGS=1
```

## I2S Timing Reference

### Clock Relationships

```
LRCLK  = AUDIO_SAMPLE_RATE
BCLK   = AUDIO_SAMPLE_RATE × 32 × 2
SYSCLK = AUDIO_SAMPLE_RATE × 256 (from WM8960 PLL)
```

### Example: 44.1 kHz Configuration

| Clock | Frequency | Period |
|-------|-----------|--------|
| LRCLK | 44,100 Hz | 22.7 µs |
| BCLK | 2,822,400 Hz | 355 ns |
| SYSCLK | 11,289,600 Hz | 88.6 ns |

### Example: 48 kHz Configuration

| Clock | Frequency | Period |
|-------|-----------|--------|
| LRCLK | 48,000 Hz | 20.8 µs |
| BCLK | 3,072,000 Hz | 326 ns |
| SYSCLK | 12,288,000 Hz | 81.4 ns |

## Wiring Requirements

### Critical for BCLK > 2 MHz

| Requirement | Spec | Reason |
|-------------|------|--------|
| **Wire length** | < 6 inches (150 mm) | Each inch adds ~5 ns delay |
| **Wire matching** | ±1 inch between signals | Prevents clock skew |
| **Ground return** | Parallel to each signal | Reduces EMI, improves signal integrity |
| **Wire gauge** | 22-26 AWG solid core | Lower capacitance vs stranded |
| **Separation** | Away from power/servo | Prevents noise coupling |

At 2.8 MHz BCLK, each bit is only **355 ns**. Long or mismatched wires cause:
- Clock/data skew → bit errors → **audio clicks/pops**
- High capacitance → slow edges → **jitter**
- Poor ground return → **EMI pickup**

### Breadboard Warning

⚠️ Breadboards add significant capacitance (~30-100 pF per inch). For production:
- Use twisted pairs or ribbon cable
- Consider a proper PCB for I2S signals
- Keep codec physically close to MCU

## PLL Calculation (Advanced)

If you need a custom sample rate not in `audio_config.h`, calculate the K value:

```
K = (SYSCLK × 2^24) / (BCLK × prescale)
```

Where:
- `SYSCLK = sample_rate × 256`
- `BCLK = sample_rate × 64` (for 32-bit stereo)
- `prescale = 4` (WM8960 default)

Example for 32 kHz:
```
SYSCLK = 32000 × 256 = 8,192,000 Hz
BCLK   = 32000 × 64  = 2,048,000 Hz
K      = (8192000 × 16777216) / (2048000 × 4)
       = 0x0C0000 (same as 48k family)
```

Add to `audio_config.h`:
```cpp
#elif AUDIO_SAMPLE_RATE == 32000
    #define WM8960_PLL_K_VALUE      0x0C0000
```

## Build Examples

### Production Build (optimized for quality)
```ini
build_flags = 
    -DAUDIO_SAMPLE_RATE=48000          ; Pro audio standard
    -DAUDIO_MAX_CHANNELS=6              ; Reduce channels for headroom
    -DAUDIO_MIX_BUFFER_SIZE=1024        ; Larger buffer for efficiency
    -DWM8960_I2C_SPEED=100000           ; Fast I2C (good wiring assumed)
```

### Low-Latency Build (gaming)
```ini
build_flags = 
    -DAUDIO_MIX_BUFFER_SIZE=128         ; Minimum latency (2.9ms @ 44.1k)
    -DAUDIO_MAX_CHANNELS=4              ; Fewer channels for CPU headroom
```

### Debug Build
```ini
build_flags = 
    -DAUDIO_DEBUG_TIMING=1              ; Show clock frequencies
    -DAUDIO_DEBUG_CODEC_REGS=1          ; Dump registers
    -DWM8960_I2C_SPEED=10000            ; Slowest speed for troubleshooting
```

## Compile-Time Validation

The configuration header includes automatic validation:

✅ **Checks:**
- Sample rate in valid range (8-192 kHz)
- Bit depth is 16, 24, or 32
- Buffer sizes are reasonable
- PLL K value defined for sample rate

⚠️ **Warnings:**
- BCLK > 5 MHz (wiring critical)
- BCLK > 10 MHz (requires PCB)

❌ **Errors:**
- Unsupported sample rate (missing PLL K value)
- Invalid bit depth
- Buffer size out of range

## See Also

- [`../src/audio/audio_config.h`](../src/audio/audio_config.h) - Full configuration header with comments
- [`../src/audio/wm8960_codec.cpp`](../src/audio/wm8960_codec.cpp) - Codec driver implementation
- [`../src/audio/audio_mixer.cpp`](../src/audio/audio_mixer.cpp) - Software mixer implementation
- [WIRING.md](WIRING.md) - Hardware wiring guide
- [CODECS.md](CODECS.md) - Codec architecture
