# sfx_audio — ScaleFX Audio Engine

8-channel WAV mixer with I2S output, codec drivers, and a lock-free ring buffer for dual-core architectures (ESP32-S3, RP2040/RP2350).

## Directory Structure

```
sfx_audio/
├── library.json
├── README.md               ← this file
├── audio/
│   ├── audio_config.h      # Compile-time constants (sample rate, buffer sizes, pin defaults)
│   ├── audio_log.h         # Audio-specific logging macros (MIXER_LOG, TAS5825_LOG, MOCK_LOG)
│   ├── audio_mixer.h       # AudioMixer<TI2S, TCodec> template declaration
│   ├── audio_mixer.ipp     # AudioMixer template implementation (included by .h)
│   ├── audio_ring_buffer.h # Lock-free SPSC ring buffer (StereoFrame, Core 0 → Core 1)
│   ├── esp_i2s_output.h    # ESP-IDF legacy I2S driver (ESP32-S3)
│   ├── pico_i2s_output.h   # Arduino-Pico PIO I2S driver (RP2040/RP2350)
│   └── mock_i2s_sink.h/.cpp # Mock I2S output for testing (statistics, capture)
└── codec/
    ├── simple_i2s_codec.h/.cpp  # No-op codec for DACs with no I2C control
    └── tas5825_codec.h/.cpp     # TI TAS5825M digital amplifier driver (I2C)
```

## Architecture

### Template Mixer — Zero-Cost Abstraction

`AudioMixer` is parameterized on its I2S output backend and audio codec:

```cpp
template<typename TI2S, typename TCodec>
class AudioMixer { ... };
```

Both `TI2S` and `TCodec` are accessed as **singletons** — the mixer calls `TI2S::instance()` and `TCodec::instance()` internally. No dependency injection, no virtual dispatch, no vtable overhead. The compiler monomorphizes the template at instantiation, inlining calls to the concrete implementations.

**Usage (ESP32-S3 with generic DAC):**

```cpp
#include <audio/esp_i2s_output.h>
#include <codec/simple_i2s_codec.h>
#include <audio/audio_mixer.h>

using Mixer = AudioMixer<EspI2SOutput, SimpleI2SCodec>;

// Access the singleton:
Mixer::instance().begin(I2S_DATA_PIN, I2S_BCLK_PIN, I2S_LRCLK_PIN);
Mixer::instance().play(0, "/sounds/engine.wav");  // opens via SdCardModule directly
```

### Dual-Core SPSC Pipeline

```
Core 0 (Producer)                     Core 1 (Consumer)
┌─────────────────────┐              ┌──────────────────────┐
│ SD card → WAV decode│              │ Ring buffer → I2S DMA│
│ 8-ch float mixing   │──────────────▶│ via TI2S::instance() │
│ → StereoFrame ring  │  lock-free   │                      │
│                     │  SPSC queue  │                      │
└─────────────────────┘              └──────────────────────┘
```

- **Producer** (`produce()`): Reads WAV data from SD, decodes to float, mixes 8 channels with per-channel volume/pan/routing, converts to `StereoFrame` (int16 stereo), pushes to ring buffer.
- **Consumer** (`consume()`): Pops frames from ring buffer, writes to I2S hardware via `TI2S::instance().writeSamples()`.
- **Ring buffer** (`AudioRingBuffer`): Lock-free single-producer single-consumer queue using `std::atomic` indices with release/acquire ordering.

### Two-Phase Initialization

```cpp
// Core 0:
Mixer::instance().begin(dataPin, bclkPin, lrclkPin);  // Phase 1: channels, codec, buffers

// Core 1 (after Core 0 signals ready):
Mixer::instance().beginI2S();   // Phase 2: I2S hardware init
Mixer::instance().consume();    // Start consuming loop
```

## Codec Concept (Compile-Time Interface)

Any type used as `TCodec` in `AudioMixer<TI2S, TCodec>` must satisfy this interface. There is no abstract base class — the template enforces the interface at compile time via duck typing. If a required method is missing or has the wrong signature, the compiler will produce an error at the point of template instantiation.

### Required Methods

```cpp
class MyCodec {
public:
    // Singleton access (required by AudioMixer internals)
    static MyCodec& instance();

    // Initialize the codec hardware at the given sample rate.
    // Called by AudioMixer::begin(). Must be idempotent.
    bool begin(uint32_t sample_rate = 44100);

    // Reset the codec to power-on state.
    void reset();

    // Set output volume (0.0 = silent, 1.0 = full scale).
    void setVolume(float volume);

    // Mute/unmute the codec output.
    void setMute(bool mute);

    // Returns true after successful begin().
    bool isInitialized() const;

    // Human-readable model name for logging (e.g., "TAS5825M", "SimpleI2S").
    const char* getModelName() const;
};
```

### Optional Debug Methods (when `AUDIO_DEBUG=1`)

```cpp
    bool testCommunication();
    uint16_t readRegisterCache(uint8_t reg) const;
    bool writeRegisterDebug(uint8_t reg, uint16_t value);
    void printStatus();
    void reinitialize(uint32_t sample_rate = 44100);
    void* getCommunicationInterface();
    void dumpRegisters();
```

These are only called when `AUDIO_DEBUG` is enabled and are not required for production builds.

### Implementations

| Class | Header | Description |
|-------|--------|-------------|
| `SimpleI2SCodec` | `codec/simple_i2s_codec.h` | No-op codec for DACs without I2C control (e.g., PCM5102A). `begin()`/`setVolume()`/`setMute()` are stubs. |
| `TAS5825Codec` | `codec/tas5825_codec.h` | TI TAS5825M digital amplifier. Full I2C register control, volume via register 0x4C, hardware mute/unmute, diagnostic registers. |

## I2S Output Concept (Compile-Time Interface)

Any type used as `TI2S` in `AudioMixer<TI2S, TCodec>` must satisfy this interface. Like the codec concept, there is no abstract base class.

### Required Methods

```cpp
class MyI2SOutput {
public:
    // Singleton access (required by AudioMixer internals)
    static MyI2SOutput& instance();

    // Initialize I2S hardware with the given pin configuration, sample rate, and bit depth.
    // Called by AudioMixer::beginI2S(). Must be idempotent.
    bool begin(const I2SPinConfig& pins, uint32_t sampleRate, uint8_t bitDepth);

    // Shut down I2S hardware and release resources.
    void end();

    // Write an array of stereo frames to the I2S DMA buffer.
    // Returns the number of frames actually written.
    size_t writeSamples(const StereoFrame* frames, size_t count);

    // Write a single silent frame (used during underruns).
    void writeSilence();

    // Returns true if I2S hardware is initialized and running.
    bool isRunning() const;

    // Human-readable backend name for logging (e.g., "ESP-IDF-I2S", "Pico-PIO-I2S").
    const char* backendName() const;
};
```

### Supporting Types

```cpp
// Defined in audio_config.h
struct I2SPinConfig {
    uint8_t dataPin;
    uint8_t bclkPin;
    uint8_t lrclkPin;
};

// Defined in audio_ring_buffer.h
struct StereoFrame {
    int16_t left;
    int16_t right;
};
```

### Implementations

| Class | Header | Platform | Description |
|-------|--------|----------|-------------|
| `EspI2SOutput` | `audio/esp_i2s_output.h` | ESP32-S3 | ESP-IDF v5.x channel-based I2S driver (`driver/i2s_std.h`). Bulk DMA writes via `i2s_channel_write()`. Internal SRAM batch buffer (2 KB). |
| `PicoI2SOutput` | `audio/pico_i2s_output.h` | RP2040/RP2350 | Arduino-Pico PIO-based I2S. Per-sample `write16()`. LRCLK must be BCLK+1 (PIO constraint). |
| `MockI2SSink` | `audio/mock_i2s_sink.h` | Any | Mock output for testing. Captures statistics (peak levels, RMS, clipping, zero-crossings) and optional sample buffer. |

## Configuration

All constants are `#ifndef`-guarded — override via `-D` build flags in `platformio.ini`.

| Constant | Default | Description |
|----------|---------|-------------|
| `AUDIO_SAMPLE_RATE` | 48000 | Output sample rate (Hz) |
| `AUDIO_BIT_DEPTH` | 16 | Bits per sample |
| `AUDIO_NUM_CHANNELS` | 2 | Output channels (stereo) |
| `AUDIO_MAX_CHANNELS` | 8 | Max simultaneous mixer channels |
| `AUDIO_DEBUG` | 0 | Enable verbose debug logging |
| `AUDIO_MOCK_I2S` | 0 | Use MockI2SSink instead of real hardware |
| `RING_FRAMES` | 2048 (ESP32) / 512 (Pico) | SPSC ring buffer capacity (stereo frames) |
| `WAV_BUF_FRAMES` | 4096 (ESP32) / 1024 (Pico) | Float decode buffer per channel |
| `WAV_SD_READ_BYTES` | 16384 (ESP32) / 4096 (Pico) | SD card read batch size |

See [audio_config.h](audio/audio_config.h) for the full list.

## Build Flags

```ini
# platformio.ini
build_flags =
    -DSFX_HAS_AUDIO=1         ; Enable audio subsystem
    -DAUDIO_DEBUG=0            ; Verbose audio logging (0=off, 1=on)
    -DAUDIO_MOCK_I2S=0         ; Use mock I2S sink (0=real hardware, 1=mock)
```

## Dependencies

- **sfx_platform** — Cross-platform abstraction (`SfxMutex`, `SFX_DELAY_MS`, `SFX_PSRAM_ALLOC`, etc.)
- **sfx_storage** — SD card access via `SdCardModule` singleton (thread-safe open/read/close)
- **SdFat** (Pico) / **SD.h** (ESP32) — transitive via sfx_storage
