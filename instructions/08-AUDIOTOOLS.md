# AudioTools Library Reference — ScaleFX Audio Engine

> **Scope:** This document covers the [pschatzmann/arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) library as used in the ScaleFX audio test application on RP2350 (Pico 2) with Waveshare Pico-Audio (PCM5101A DAC) and SD card.

## Table of Contents

- [Library Overview](#library-overview)
- [Our Hardware Configuration](#our-hardware-configuration)
- [Pipeline Architecture](#pipeline-architecture)
- [Class Reference](#class-reference)
  - [AudioInfo](#audioinfo)
  - [SineWaveGenerator\<T\>](#sinewavegeneratort)
  - [GeneratedSoundStream\<T\>](#generatedsoundstreamt)
  - [GeneratorMixer\<T\>](#generatormixert)
  - [InputMixer\<T\>](#inputmixert)
  - [I2SStream](#i2sstream)
  - [StreamCopy](#streamcopy)
  - [WAVDecoder](#wavdecoder)
  - [EncodedAudioStream / EncodedAudioOutput](#encodedaudiostream--encodedaudiooutput)
  - [DecoderFloat / EncoderFloat](#decoderfloat--encoderfloat)
  - [NumberFormatConverterStream](#numberformatconverterstream)
  - [FormatConverterStream](#formatconverterstream)
  - [VolumeStream](#volumestream)
  - [FloatAudio](#floataudio)
- [Float vs int16_t Pipeline](#float-vs-int16_t-pipeline)
  - [Template Data Types](#template-data-types)
  - [Float Amplitude Convention](#float-amplitude-convention)
  - [Critical: InputMixer\<float\> is Broken](#critical-inputmixerfloat-is-broken)
  - [Critical: I2S Driver Has No Float Conversion](#critical-i2s-driver-has-no-float-conversion)
  - [Recommended Pipeline Architectures](#recommended-pipeline-architectures)
- [SD Card WAV Playback Patterns](#sd-card-wav-playback-patterns)
- [Dual-Core Architecture](#dual-core-architecture)
- [Performance Tuning](#performance-tuning)
- [RP2040/RP2350 Platform Configuration](#rp2040rp2350-platform-configuration)
- [Common Pitfalls](#common-pitfalls)
- [Links and References](#links-and-references)

---

## Library Overview

AudioTools is a header-only C++ library providing a comprehensive audio processing framework for Arduino-compatible microcontrollers. It follows the Arduino Stream paradigm — all audio data flows through `Stream`/`Print` objects using `readBytes()` and `write()`.

**Version:** v1.2.2 at commit `768ff90` (Mar 10, 2026)  
**Include:** `#include "AudioTools.h"` (umbrella header for all core functionality)  
**License:** LGPL

### Design Principles

| Principle | Detail |
|-----------|--------|
| **Header-only** | No compiled libraries required; all code resolves at compile time |
| **Stream-based** | Audio sources and sinks use Arduino `Stream`/`Print` interfaces |
| **Copy pipeline** | Data flows via `StreamCopy` from source to sink (pull model) |
| **Template types** | Sample type is a template parameter (`int16_t`, `int32_t`, `float`) |
| **Separate core/extended** | Core via `AudioTools.h`; codecs, disk, comms via separate includes |
| **Platform-agnostic** | Platform differences handled in `AudioConfig.h` / `rp2040hower.h` |

### Key Concept: Audio Data

Audio is represented as a stream of PCM samples. The format is described by `AudioInfo`:
- **bits_per_sample**: 16 (int16_t), 24 (int24_t), or 32 (int32_t)
- **channels**: 1 (mono) or 2 (stereo)
- **sample_rate**: e.g., 44100, 48000

Samples are interleaved by channel: `[L0][R0][L1][R1]...`

The byte stream is always `uint8_t*` at the transport level. To access samples, cast to the appropriate type:
```cpp
int16_t* samples = (int16_t*)buffer;
```

### Key Concept: Sources and Sinks

| Role | Interface | Operations | Examples |
|------|-----------|------------|----------|
| **Source** | `Stream` (read) | `readBytes()`, `available()` | `GeneratedSoundStream`, `InputMixer`, `File` |
| **Sink** | `Print` (write) | `write()`, `availableForWrite()` | `I2SStream`, `CsvOutput`, `EncodedAudioStream` |
| **Both** | `AudioStream` | Read + Write | `I2SStream` (TX/RX), `FilteredStream` |

Data flows from Source → Sink via `StreamCopy`:
```
Source.readBytes() → buffer → Sink.write()
```

---

## Our Hardware Configuration

### RP2350 (Pico 2)

| Feature | Value |
|---------|-------|
| CPU | Dual Cortex-M33 @ 150 MHz |
| RAM | 512 KB SRAM |
| Flash | 4 MB |
| FPU | Hardware single-precision IEEE 754 |
| DSP | Single-cycle MAC, SIMD half-word ops |

### Waveshare Pico-Audio (Initial Version)

| Signal | GPIO | Function |
|--------|------|----------|
| DIN | GP26 | I2S Data |
| BCK | GP27 | I2S Bit Clock |
| LRCLK | GP28 | I2S Word Select (auto BCK+1) |
| MCLK | — | Not used (PCM5101A auto-detects) |

- **DAC:** PCM5101A (3-wire I2S, no MCLK required)
- **Format:** I2S Standard (MSB-first, left-justified in 32-bit slot)

### SD Card Module

| Signal | GPIO | Bus |
|--------|------|-----|
| CS | GP17 | SPI0 CSn |
| SCK | GP18 | SPI0 SCK |
| MOSI | GP19 | SPI0 TX |
| MISO | GP16 | SPI0 RX |

- **Library:** Arduino `SD.h` (wraps SdFat internally on earlephilhower core)
- **Filesystem:** FAT32

---

## Pipeline Architecture

### Tone Generation Pipeline

```
SineWaveGenerator<int16_t>  ──┐
SineWaveGenerator<int16_t>  ──┤
SineWaveGenerator<int16_t>  ──┼──► InputMixer<int16_t> ──► StreamCopy ──► I2SStream ──► PCM5101A
SineWaveGenerator<int16_t>  ──┘         (weighted sum)       (copy())        (DMA)
       ↑                                    ↑
  GeneratedSoundStream<int16_t>         readBytes()
  wraps each generator                 pulls from all
```

### WAV File Playback Pipeline

```
SD Card File ──► StreamCopy ──► EncodedAudioOutput(WAVDecoder) ──► I2SStream ──► PCM5101A
  (File)          (copy())       (strips WAV header,               (DMA)
                                  outputs raw PCM)
```

### Audio Mode Switching

The application has three modes, sharing the single I2S output:

| Mode | Source | StreamCopy | Notes |
|------|--------|------------|-------|
| Generator | `InputMixer` | `genCopier` | Sine tones, unlimited source |
| FilePlay | `File` (SD) | `fileCopier` | WAV file, finite source |
| Idle | — | — | `delay(1)` in loop |

Only one mode is active at a time. Mode switches stop the current playback and configure the new pipeline.

---

## Class Reference

### AudioInfo

Describes the audio format. Used to configure all streams.

```cpp
AudioInfo info(48000, 2, 16);  // sample_rate, channels, bits_per_sample
// Or:
AudioInfo info;
info.sample_rate = 48000;
info.channels = 2;
info.bits_per_sample = 16;
```

**copyFrom()** — Copy format from another AudioInfo:
```cpp
auto config = i2sOut.defaultConfig(TX_MODE);
config.copyFrom(audioInfo);
```

---

### SineWaveGenerator\<T\>

Generates a sine wave at a specified frequency and amplitude.

```cpp
#include "AudioTools.h"

SineWaveGenerator<int16_t> gen(32000);     // amplitude = 32000
SineWaveGenerator<int16_t> gen;            // default amplitude = 0.9 × 32767 ≈ 29490
SineWaveGenerator<float>   gen(1.0f);      // ⚠️ MUST specify amplitude for float (see below)
```

**Key API:**

| Method | Description |
|--------|-------------|
| `begin(AudioInfo, float freq)` | Initialize with format and frequency |
| `setFrequency(float hz)` | Change frequency (real-time safe) |
| `setAmplitude(float amp)` | Change amplitude (real-time safe) |
| `readSample()` | Generate next sample value of type `T` |
| `isActive()` | Returns true if frequency > 0 |

**Sample generation algorithm:**
```cpp
T readSample() {
    float angle = 2π × m_cycles + m_phase;
    T result = m_amplitude * sinf(angle);
    m_cycles += m_frequency / sample_rate;
    if (m_cycles > 1.0) m_cycles -= 1.0;
    return result;
}
```

**Default amplitude by type:**

| Type | `maxValueT<T>()` | Default amplitude (0.9×max) | Notes |
|------|-------------------|----------------------------|-------|
| `int16_t` | 32767 | ~29490 | Good default |
| `int32_t` | 2147483647 | ~1.93e9 | Good default |
| `float` | **3.4e38** | **~3.06e38** | ⚠️ **BROKEN** — see [Float Pitfalls](#float-amplitude-convention) |

**Other generators** in the library: `SquareWaveGenerator`, `SawToothGenerator`, `WhiteNoiseGenerator`, `PinkNoiseGenerator`, and more. See [generator docs](https://pschatzmann.github.io/arduino-audio-tools/group__generator.html).

---

### GeneratedSoundStream\<T\>

Wraps a `SoundGenerator<T>` into an Arduino `Stream`, making it usable in pipelines.

```cpp
SineWaveGenerator<int16_t> gen(32000);
GeneratedSoundStream<int16_t> stream(gen);

stream.begin(audioInfo);          // Set format
gen.begin(audioInfo, N_A4);       // Set frequency (440 Hz)
```

**How it works:**
- `readBytes()` calls `generator.readSample()` in a loop, filling the buffer
- `available()` returns a large constant (~100 KB) — generators are conceptually unbounded
- Each sample is `sizeof(T)` bytes in the output buffer
- All channels receive the same sample value (mono content duplicated to stereo)

---

### GeneratorMixer\<T\>

Mixes multiple `SoundGenerator<T>` objects at the **sample level** (before stream wrapping).

```cpp
SineWaveGenerator<int16_t> gen1(16000), gen2(16000);
GeneratorMixer<int16_t> mixer;
mixer.add(gen1);
mixer.add(gen2);
GeneratedSoundStream<int16_t> stream(mixer);  // One stream from mixed generators
```

**Mixing algorithm:** Simple averaging — `total / count` (equal weight for all active generators).

| Feature | GeneratorMixer | InputMixer |
|---------|---------------|------------|
| **Input type** | `SoundGenerator<T>` only | Any `Stream` |
| **Mixing model** | Equal average (total/count) | Weighted sum (weight/total_weights) |
| **Weight control** | None | Per-stream (0–100) |
| **Skip inactive** | Yes (`isActive()`) | Via weight=0 |
| **Use case** | Pre-mixing generators | Mixing any audio streams |

**Pro:** Simpler, works correctly with `float`, avoids `InputMixer<float>` bug.  
**Con:** No per-generator volume control, generators only (not streams/files).

---

### InputMixer\<T\>

Combines multiple input `Stream` objects into a single mixed output stream.

```cpp
InputMixer<int16_t> mixer;
mixer.add(stream1);              // weight=100 (default)
mixer.add(stream2, 50);          // weight=50 (half volume)
mixer.begin(audioInfo);

StreamCopy copier(i2sOut, mixer);
copier.copy();                   // Pulls from all inputs, mixes, writes to output
```

**Key API:**

| Method | Description |
|--------|-------------|
| `add(Stream&, int weight=100)` | Add input stream with weight |
| `begin(AudioInfo)` | Initialize mixer |
| `setWeight(int idx, int weight)` | Change weight dynamically (0 = mute & skip) |
| `remove(int idx)` | Remove stream by index |
| `size()` | Number of input streams |
| `readBytes()` | Read mixed audio from all inputs |
| `setRetryCount(int)` | Max retries for reading from empty inputs |

**Mixing algorithm (weighted):**
```
For each sample position j:
    result[j] = Σ (input[i][j] × weight[i] / total_weights)
```

With equal weights (100, 100) and 2 inputs: each contributes 50%.  
With weights (100, 50): first contributes 67%, second 33%.

⚠️ **`InputMixer<float>` is broken** — see [critical issue](#critical-inputmixerfloat-is-broken) below.

---

### I2SStream

I2S input/output stream for DAC/ADC communication.

```cpp
I2SStream i2sOut;
auto config = i2sOut.defaultConfig(TX_MODE);
config.copyFrom(audioInfo);     // 48000, 2ch, 16-bit
config.pin_bck  = 27;
config.pin_ws   = 28;
config.pin_data = 26;
config.buffer_size  = 8192;     // DMA buffer size (bytes)
config.buffer_count = 8;        // Number of DMA buffers
i2sOut.begin(config);
```

**Configuration options (I2SConfigStd):**

| Field | Default | Description |
|-------|---------|-------------|
| `rx_tx_mode` | `TX_MODE` | `TX_MODE` (output) or `RX_MODE` (input) |
| `pin_bck` | 26 | Bit clock pin |
| `pin_ws` | pin_bck+1 | Word select (LRCLK) pin |
| `pin_data` | 28 | Data output pin (TX) |
| `pin_data_rx` | -1 | Data input pin (RX) |
| `pin_mck` | -1 | Master clock pin (-1 = disabled) |
| `buffer_size` | 512 | DMA buffer size in bytes |
| `buffer_count` | 6 | Number of DMA buffers |
| `i2s_format` | `I2S_STD_FORMAT` | I2S standard format |
| `is_master` | true | Master mode (generates clocks) |

**DMA Ring Buffer:** Total DMA memory = `buffer_size × buffer_count`. For our config: 8192 × 8 = 64 KB. Larger buffers add latency but prevent underruns. With 48 kHz stereo 16-bit: 64 KB ≈ 341 ms of audio.

**Rate-limited output:** `I2SStream.write()` blocks when all DMA buffers are full, throttling the pipeline to real-time. This is what prevents the generator from running ahead.

⚠️ **No float conversion** — the RP2040 I2S driver sends raw bytes. Float data will be interpreted as integer bit patterns, producing garbage. See [critical issue](#critical-i2s-driver-has-no-float-conversion).

---

### StreamCopy

Copies data from a source `Stream` to a destination `Print` in the `loop()`. The workhorse of every pipeline.

```cpp
StreamCopy copier(output, input, 16384);  // 16 KB buffer
// In loop:
copier.copy();   // Copy one buffer-full
```

**Key API:**

| Method | Description |
|--------|-------------|
| `copy()` | Copy one buffer-full from source to destination |
| `copyN(size_t pages)` | Copy `pages × buffer_size` bytes |
| `copyAll()` | Copy until source exhausted |
| `copyMs(size_t ms)` | Copy for the specified duration |

**How `copy()` works:**
1. Determine bytes to read: `min(available(), buffer_size)`
2. Align to frame boundary (`channels × bytes_per_sample`)
3. `source.readBytes(buffer, len)` — pull data from source
4. `destination.write(buffer, len)` — push data to sink (blocking with retry)
5. Up to 20 retries with 10 ms delay if write returns 0

**Buffer sizing:**
- Default: `DEFAULT_BUFFER_SIZE` (1024 bytes)
- Constructor parameter overrides: `StreamCopy copier(out, in, 16384)`
- Larger buffers = fewer copies per second = lower overhead
- Must be a multiple of frame size (e.g., 4 bytes for stereo 16-bit)

**Return value:** Number of bytes successfully copied. Returns 0 when source has no data.

---

### WAVDecoder

Decodes WAV file data (strips header, outputs raw PCM samples).

```cpp
#include "AudioTools/AudioCodecs/CodecWAV.h"

WAVDecoder decoder;
```

Used inside an `EncodedAudioStream` or `EncodedAudioOutput`:
```cpp
EncodedAudioOutput pipe(&i2sOut, &decoder);
pipe.begin();
// Write WAV file bytes to pipe → decoder strips header → raw PCM goes to I2S
```

The decoder automatically detects sample rate, channels, and bit depth from the WAV header and propagates this info to downstream components.

---

### EncodedAudioStream / EncodedAudioOutput

Wraps a decoder/encoder into the stream pipeline. Two variants:

| Class | Direction | Usage |
|-------|-----------|-------|
| `EncodedAudioStream` | Bidirectional | Encoder (output side) or Decoder (input side) |
| `EncodedAudioOutput` | Write-only | Decoder as output sink (write encoded data → decoded PCM forward) |

**WAV file playback (typical):**
```cpp
WAVDecoder decoder;
EncodedAudioOutput pipe(&i2sOut, &decoder);
pipe.begin();

File wavFile = SD.open("/sound.wav");
StreamCopy copier(pipe, wavFile);
copier.copy();  // WAV bytes → decoder → PCM → I2S
```

**Flow:** `File → write() → EncodedAudioOutput → WAVDecoder → raw PCM → I2SStream`

---

### DecoderFloat / EncoderFloat

Codec pair for converting between float [-1.0, +1.0] and int16_t samples.

```cpp
#include "AudioTools/AudioCodecs/CodecFloat.h"

DecoderFloat decoder;   // float → int16_t (multiply by 32767)
EncoderFloat encoder;   // int16_t → float (divide by 32768)
```

**DecoderFloat.write():**
```cpp
// For each sample: int16_t output = float_input × 32767
float* p_float = (float*)data;
for (int j = 0; j < samples; j++) {
    buffer[j] = p_float[j] * 32767;
}
p_print->write((uint8_t*)buffer.data(), samples * sizeof(int16_t));
```

**EncoderFloat.write():**
```cpp
// For each sample: float output = int16_t_input / 32768.0
int16_t* pt16 = (int16_t*)data;
for (size_t j = 0; j < samples; j++) {
    buffer[j] = static_cast<float>(pt16[j]) / 32768.0;
}
```

**Use case:** Insert `DecoderFloat` with `EncodedAudioStream` to convert a float pipeline to int16_t before I2S output. See [Recommended Pipeline Architectures](#recommended-pipeline-architectures).

---

### NumberFormatConverterStream

Converts between integer audio formats (bit depth conversion).

```cpp
I2SStream i2sOut;
NumberFormatConverterStream converter(i2sOut);
converter.begin(16, 32);  // Convert 16-bit input to 32-bit output

StreamCopy copier(converter, source);
copier.copy();
```

**Supported conversions:** 8↔16, 16↔24, 16↔32, 24↔16, 32↔16 (integer types only).

⚠️ **Does NOT support float.** For float↔int16_t conversion, use `DecoderFloat`/`EncoderFloat` or `NumberFormatConverterStreamT<FloatAudio, int16_t>`.

---

### FormatConverterStream

All-in-one converter for changing sample rate, channel count, and/or bit depth.

```cpp
AudioInfo from(44100, 1, 32);  // 44.1 kHz, mono, 32-bit
AudioInfo to(48000, 2, 16);    // 48 kHz, stereo, 16-bit

FormatConverterStream converter(output);
converter.begin(from, to);

StreamCopy copier(converter, input);
copier.copy();
```

More convenient than individual converters but slightly less efficient than dedicated classes.

---

### VolumeStream

Adjusts volume (amplitude) of audio data passing through.

```cpp
VolumeStream volume(output);
auto config = volume.defaultConfig();
config.copyFrom(audioInfo);
config.volume = 0.5;  // 50% volume
volume.begin(config);

StreamCopy copier(volume, input);
copier.copy();
```

Can be inserted anywhere in the pipeline chain. Supports per-channel volume and dynamic adjustment.

---

### FloatAudio

A wrapper class for float audio samples that provides automatic conversion to integer types.

```cpp
// From AudioTools/CoreAudio/AudioBasic/FloatAudio.h
class FloatAudio {
    FloatAudio(float in) { value = in; }
    operator float() { return value; }
    explicit operator int16_t() { return value * 32767; }
    explicit operator int32_t() { return value * 2147483647.0f; }
protected:
    float value = 0.0f;
};
```

**Convention:** `FloatAudio` stores values in the **-1.0 to +1.0** range and converts to the full integer range on demand.

Can be used as a template parameter: `SineWaveGenerator<FloatAudio>`, etc. However, `InputMixer<FloatAudio>` still has the `Vector<int>` accumulator bug.

---

## Float vs int16_t Pipeline

### Template Data Types

All generator, stream, and mixer classes are templated on the sample type `T`:

```cpp
SineWaveGenerator<int16_t>       // Samples: -32767 to +32767
SineWaveGenerator<float>         // Samples: -1.0 to +1.0 (ideally)
SineWaveGenerator<FloatAudio>    // Samples: FloatAudio(-1.0 to +1.0)

GeneratedSoundStream<int16_t>   // readBytes produces int16_t samples
GeneratedSoundStream<float>     // readBytes produces float samples (4 bytes each)

InputMixer<int16_t>              // ✅ Works correctly
InputMixer<float>                // ❌ BROKEN — see below
```

### Float Amplitude Convention

The library's convention for float audio is **-1.0 to +1.0** range:

```cpp
// NumberConverter (AudioTypes.h):
float toFloatT<int16_t>(int16_t value) { return value / 32767.0f; }
int16_t fromFloatT<float>(float value) { return value * 32767; }

// DecoderFloat (CodecFloat.h):
int16_t output = float_input * 32767;   // Float→int16_t

// EncoderFloat (CodecFloat.h):
float output = int16_t_input / 32768.0; // int16_t→Float
```

**⚠️ SineWaveGenerator\<float\> default amplitude is BROKEN:**

```cpp
// Default amplitude = 0.9 × maxValueT<float>()
// On RP2040/RP2350 with USE_TYPETRAITS defined:
//   maxValueT<float>() = std::numeric_limits<float>::max() ≈ 3.4e38
//   Default amplitude ≈ 3.06e38 — ASTRONOMICALLY wrong!
```

**ALWAYS explicitly set amplitude for float generators:**
```cpp
SineWaveGenerator<float> gen(1.0f);    // ✅ Correct: -1.0 to +1.0 range
SineWaveGenerator<float> gen;          // ❌ Broken: ±3.06e38 range
gen.setAmplitude(0.73f);               // ✅ 73% of full scale
```

### Critical: InputMixer\<float\> is Broken

**The `InputMixer<T>` class has a fundamental bug when `T = float`.**

The internal accumulator is hardcoded as `Vector<int>` regardless of the template parameter:

```cpp
// AudioStreams.h line 1312
template <typename T>
class InputMixer : public AudioStream {
protected:
    Vector<int> result_vect;    // ← Always int, even when T = float!
    Vector<T>   current_vect;   // ← Correctly typed

    void resultAdd(float fact) {
        for (int j = 0; j < current_vect.size(); j++) {
            current_vect[j] *= fact;        // float × float = float (OK)
            result_vect[j] += current_vect[j]; // int += float → TRUNCATION!
        }
    }

    void resultClear() {
        memset(result_vect.data(), 0, sizeof(int) * result_vect.size());
    }
};
```

**What happens with `InputMixer<float>` and samples in -1.0..+1.0:**

1. `current_vect[j]` = 0.36 (float sample × weight factor)
2. `result_vect[j] += 0.36` → `int(0) + float(0.36)` → truncated to `int(0)`
3. Final: `p_data[j] = result_vect[j]` → `float(0)`

**Result: Near-silence.** All float samples in the -1.0 to +1.0 range get truncated to 0 (or occasionally ±1). The output is effectively zero.

### Critical: I2S Driver Has No Float Conversion

The RP2040/RP2350 I2S driver (`I2SDriverRP2040`) writes raw bytes without any type awareness:

```cpp
// I2SRP2040.h — writeBytes for 2-channel mode:
while (size_bytes >= sizeof(int32_t)) {
    i2s.write(*(int32_t*)p, true);  // Raw 4-byte write
    size_bytes -= sizeof(int32_t);
    p += sizeof(int32_t);
}
```

If you feed `float` data (IEEE 754 bit patterns), it gets reinterpreted as `int32_t` and sent to the DAC. The bit pattern `0x3F800000` (float 1.0) becomes integer `1065353216` — garbage audio.

**The I2S output always expects the data format matching `AudioInfo.bits_per_sample`:**
- 16-bit: expects `int16_t` samples (2 bytes per sample per channel)
- 32-bit: expects `int32_t` samples (4 bytes per sample per channel)

### Recommended Pipeline Architectures

#### Option A: int16_t Pipeline (Recommended — Proven)

All official examples use this. Works correctly with all mixer types.

```cpp
// ✅ PROVEN — all examples use this pattern
AudioInfo info(48000, 2, 16);

SineWaveGenerator<int16_t> gen1(24000);  // ~73% of 32767
SineWaveGenerator<int16_t> gen2(24000);
GeneratedSoundStream<int16_t> stream1(gen1);
GeneratedSoundStream<int16_t> stream2(gen2);

InputMixer<int16_t> mixer;
mixer.add(stream1);
mixer.add(stream2);
mixer.begin(info);

I2SStream i2sOut;
auto config = i2sOut.defaultConfig(TX_MODE);
config.copyFrom(info);
i2sOut.begin(config);

StreamCopy copier(i2sOut, mixer, 16384);
// In loop: copier.copy();
```

**Pros:** Battle-tested, all library classes work correctly, no conversion needed.  
**Cons:** Integer arithmetic for mixing (but InputMixer does the weighting with `float` internally anyway).

#### Option B: GeneratorMixer\<float\> with DecoderFloat

Use `GeneratorMixer<float>` (which works correctly since it averages at sample level with float arithmetic) and convert to int16_t before I2S.

```cpp
// ✅ WORKS — float generation with explicit conversion
AudioInfo info(48000, 2, 16);

SineWaveGenerator<float> gen1(0.73f);
SineWaveGenerator<float> gen2(0.73f);
GeneratorMixer<float> genMix;
genMix.add(gen1);
genMix.add(gen2);
GeneratedSoundStream<float> floatStream(genMix);

DecoderFloat float2int;
EncodedAudioStream converter(&i2sOut, &float2int);  // float→int16_t
converter.begin();

StreamCopy copier(converter, floatStream, 16384);
// In loop: copier.copy();
```

**Pros:** Float arithmetic in generators, correct averaging, RP2350 FPU benefit.  
**Cons:** Extra conversion step, `GeneratorMixer` has no per-generator volume (equal average only), cannot mix non-generator streams (files).

#### Option C: Manual Float Mixing (Custom Code)

Write a custom mixer that operates in float and converts to int16_t at the output stage.

```cpp
// Custom approach — maximum control
// Generate float samples manually, mix, convert, write to I2S
```

This requires bypassing the library's mixer classes entirely.

#### Summary

| Pipeline | Generators | Mixing | File Playback | Complexity |
|----------|-----------|--------|---------------|------------|
| **A: int16_t** | ✅ Works | ✅ InputMixer | ✅ WAVDecoder→I2S | Low |
| **B: GeneratorMixer\<float\>** | ✅ FPU benefit | ⚠️ Equal weight only | ✅ Separate pipe | Medium |
| **C: Custom float** | ✅ FPU benefit | ✅ Custom weights | ✅ Custom | High |

**Recommendation for ScaleFX:** Use **Option A** (int16_t pipeline). The mixing weight calculations in `InputMixer` already use float arithmetic internally, and the generator's `sinf()` call is the FPU-intensive part regardless of the output sample type. The RP2350 FPU is used for `sinf()` in both int16_t and float pipelines.

---

## SD Card WAV Playback Patterns

### Pattern 1: Low-Level (Our Approach)

Direct file → decoder → I2S pipeline with explicit control.

```cpp
#include "AudioTools/AudioCodecs/CodecWAV.h"

WAVDecoder decoder;
EncodedAudioOutput pipe(&i2sOut, &decoder);
File wavFile;
StreamCopy fileCopier;

// Start playback:
wavFile = SD.open("/sounds/engine.wav");
if (wavFile) {
    pipe.begin();
    fileCopier.begin(pipe, wavFile, 8192);  // 8 KB copy buffer
}

// In loop:
size_t copied = fileCopier.copy();
if (copied == 0) {
    // End of file — stop playback
    wavFile.close();
    pipe.end();
}
```

**Pros:** Full control over file selection, start/stop, error handling.  
**Cons:** Manual file management, no auto-advance, no playlist.

### Pattern 2: AudioPlayer (High-Level)

Automatic file navigation with skip/previous/volume controls.

```cpp
#include "AudioTools/Disk/AudioSourceSD.h"

AudioSourceSD source("/sounds", ".wav", SD_PIN_CS);
I2SStream i2sOut;
WAVDecoder decoder;
AudioPlayer player(source, i2sOut, decoder);

void setup() {
    SD.begin(SD_PIN_CS);
    i2sOut.begin(config);
    player.begin();
}

void loop() {
    player.copy();
}

// Controls:
player.next();        // Next file
player.previous();    // Previous file
player.setVolume(0.5); // 50% volume
player.setActive(true/false); // Play/pause
```

**Pros:** Built-in file enumeration, skip/previous, volume.  
**Cons:** Less control over file selection, harder to integrate with custom command system.

### SD Card Init (Arduino SD.h)

```cpp
SPI.setRX(SD_PIN_MISO);
SPI.setTX(SD_PIN_MOSI);
SPI.setSCK(SD_PIN_SCK);
SPI.setCS(SD_PIN_CS);

if (SD.begin(SD_PIN_CS)) {
    // SD card ready — FAT32
}
```

On the earlephilhower core, `SD.h` wraps SdFat internally. No need for an external SdFat library.

---

## Dual-Core Architecture

### RP2040/RP2350 Dual-Core Pattern

The library provides FreeRTOS `Task` and `BufferRTOS` for multi-core/multi-task audio, but on the RP2040/RP2350 with the earlephilhower Arduino core, the simpler `setup1()`/`loop1()` pattern works well:

```
Core 0 (setup/loop):     Core 1 (setup1/loop1):
┌───────────────────┐    ┌───────────────────┐
│ Serial UI         │    │ I2S init          │
│ SD card init      │    │ Generator init    │
│ Command parsing   │──→ │ Mixer init        │
│ File selection    │    │ Audio pipeline    │
│ Mode switching    │    │ copier.copy()     │
└───────────────────┘    └───────────────────┘
         ↕ volatile struct AudioMsg
```

**Rules:**
1. **Serial and SD init on Core 0** (Serial is owned by Core 0 on earlephilhower)
2. **All audio objects initialized on Core 1** (I2S DMA, generators, mixer)
3. **Inter-core communication via volatile struct** — simple command passing
4. **No shared audio objects** — Core 0 sends commands, Core 1 owns all audio state
5. **`copier.copy()` on Core 1** — tight loop, no delay (I2S write blocks naturally)

### Why Not Use AudioTools' Task/BufferRTOS?

The library's `Task` and `BufferRTOS` classes are designed for FreeRTOS (ESP32). On the RP2040/RP2350 with earlephilhower core:
- FreeRTOS is not used by default (the core uses its own scheduler)
- `setup1()`/`loop1()` provides native dual-core support
- No queue/mutex overhead for the audio pipeline
- `volatile` struct is sufficient for simple command passing

### AudioTools' Multicore Pattern (FreeRTOS)

For reference, the library's approach uses `QueueStream` between tasks:

```cpp
// Source task writes to queue:
BufferRTOS<uint8_t> buffer(1024 * 10);
QueueStream<uint8_t> queue(buffer);
StreamCopy copierSource(queue, source);
Task writeTask("write", 3000, 10, 0);  // Core 0
writeTask.begin([]() { copierSource.copy(); });

// Sink task reads from queue:
StreamCopy copierSink(output, queue);
Task readTask("read", 3000, 10, 1);    // Core 1
readTask.begin([]() { copierSink.copy(); });
```

This is more robust for complex scenarios but adds latency and memory overhead.

---

## Performance Tuning

### Buffer Sizing

| Buffer | Purpose | Default | Our Value | Impact |
|--------|---------|---------|-----------|--------|
| I2S `buffer_size` | DMA buffer | 512 B | 8192 B | Larger = fewer interrupts, higher latency |
| I2S `buffer_count` | DMA ring | 6 | 8 | More = more tolerance for jitter |
| StreamCopy buffer | Copy chunk | 1024 B | 16384 B | Larger = fewer copy() calls |
| Total I2S DMA | Ring total | 3 KB | 64 KB | Total buffered audio time |

**Our config:** 8192 × 8 = 64 KB DMA ring ≈ 341 ms at 48 kHz stereo 16-bit.

### Timing Budget

At 48 kHz, stereo, 16-bit:
- **Byte rate:** 48000 × 2 × 2 = 192,000 bytes/sec
- **Per copy (16 KB buffer):** 16384 / 192000 = 85.3 ms of audio per `copy()` call
- **Minimum loop rate:** ~12 copies/sec to keep up
- **Margin:** At 150 MHz, the RP2350 has ample headroom

### Logging Impact

> **Warning:** AudioTools logging at `Info` or `Debug` level significantly impacts audio quality. Use `Warning` or `Error` in production:

```cpp
AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);
```

### StreamCopy Optimization

```cpp
StreamCopy copier(out, in, 16384);  // 16 KB buffer
copier.copy();                       // Copy one chunk
copier.copyN(4);                     // Copy 4 chunks (64 KB)
copier.copyAll();                    // Copy until source exhausted
```

---

## RP2040/RP2350 Platform Configuration

The library's platform config for RP2040 (also used for RP2350) is in `rp2040hower.h`:

```cpp
// Platform defines (rp2040hower.h)
#define USE_I2S                         // Enable I2S support
#define USE_PWM                         // Enable PWM audio output
#define USE_TYPETRAITS                  // Use std::numeric_limits<T>
#define USE_SD_NO_NS                    // SD library without namespace
#define USE_SD_SUPPORTS_SPI             // SD.begin() accepts SPI settings

// Default pin assignments (can be overridden in sketch)
#define PIN_I2S_BCK       26            // I2S bit clock
#define PIN_I2S_WS        (PIN_I2S_BCK + 1)  // I2S word select (auto BCK+1)
#define PIN_I2S_DATA_OUT  28            // I2S data out (TX)
#define PIN_I2S_DATA_IN   28            // I2S data in (RX)
#define PIN_I2S_MCK       -1            // Master clock (disabled)

// SD card
#define PIN_CS            PIN_SPI0_SS   // SD chip select (from board variant)

// Audio defaults
#define DEFAULT_BUFFER_SIZE   1024
#define I2S_BUFFER_SIZE       512
#define I2S_BUFFER_COUNT      6
```

**⚠️ Pin name collision:** The platform config defines `PIN_I2S_BCK`, `PIN_I2S_WS`, etc. which can collide with your own constants. Use different names in your sketch (e.g., `I2S_PIN_BCLK`) or `#undef` them.

---

## Common Pitfalls

### 1. Float Default Amplitude

```cpp
// ❌ WRONG — default amplitude ≈ 3.06e38
SineWaveGenerator<float> gen;

// ✅ CORRECT — explicit amplitude in 0.0–1.0 range
SineWaveGenerator<float> gen(1.0f);
gen.setAmplitude(0.73f);
```

### 2. InputMixer\<float\> Silent Output

```cpp
// ❌ BROKEN — internal Vector<int> truncates float samples to 0
InputMixer<float> mixer;

// ✅ USE int16_t instead
InputMixer<int16_t> mixer;
```

### 3. Float Data Sent to I2S

```cpp
// ❌ BROKEN — raw float bits sent as int32_t
GeneratedSoundStream<float> stream(gen);
StreamCopy copier(i2sOut, stream);  // Float bytes → I2S = garbage

// ✅ Convert first
DecoderFloat float2int;
EncodedAudioStream converter(&i2sOut, &float2int);
StreamCopy copier(converter, stream);  // Float → int16_t → I2S = correct
```

### 4. DMA Buffer Allocation Failure

If `buffer_size × buffer_count` exceeds available RAM, `I2SStream.begin()` may fail silently. Check the return value and free RAM:

```cpp
bool ok = i2sOut.begin(config);
Serial.printf("I2S begin: %s, Free RAM: %lu\n", ok ? "OK" : "FAIL", rp2040.getFreeHeap());
```

On RP2350 with 512 KB RAM, 64 KB for DMA is fine. On RP2040 with 264 KB, keep DMA ≤ 32 KB.

### 5. bits_per_sample vs Template Type Mismatch

The `AudioInfo.bits_per_sample` must match the template type's size:

| Template Type | bits_per_sample | Bytes per Sample |
|---------------|-----------------|------------------|
| `int16_t` | 16 | 2 |
| `int32_t` | 32 | 4 |
| `float` | 32 | 4 |

If using `SineWaveGenerator<float>` with `bits_per_sample = 16`, the frame alignment in `StreamCopy` will be wrong — it reads 2 bytes per sample but the generator produces 4, causing sample misalignment and distortion.

### 6. Generator Not Initialized

```cpp
// ❌ Generates silence — no frequency set
gen.begin(info);
genStream.begin(info);

// ✅ Set frequency before or after begin
gen.begin(info, N_A4);
// Or:
gen.begin(info, 0);
gen.setFrequency(440.0f);
gen.setAmplitude(24000);
```

### 7. Missing `#include` for Codecs

```cpp
// Core classes included by "AudioTools.h"
#include "AudioTools.h"

// Codecs need separate includes:
#include "AudioTools/AudioCodecs/CodecWAV.h"    // WAVDecoder, WAVEncoder
#include "AudioTools/AudioCodecs/CodecFloat.h"  // DecoderFloat, EncoderFloat
#include "AudioTools/AudioCodecs/CodecMP3Helix.h" // MP3 (requires library)

// SD card sources:
#include "AudioTools/Disk/AudioSourceSD.h"      // AudioSourceSD
#include "AudioTools/Disk/AudioSourceSDFAT.h"   // AudioSourceSDFAT
```

### 8. Logging Causes Audio Glitches

```cpp
// ❌ Info/Debug logging disrupts audio at high sample rates
AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);

// ✅ Use Warning or Error for production
AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);
```

---

## Links and References

### Official Documentation

- **Wiki Home:** https://github.com/pschatzmann/arduino-audio-tools/wiki
- **API Reference:** https://pschatzmann.github.io/arduino-audio-tools/
- **Examples:** https://github.com/pschatzmann/arduino-audio-tools/tree/main/examples
- **GitHub Discussions:** https://github.com/pschatzmann/arduino-audio-tools/discussions

### Key Wiki Pages

| Page | Relevance |
|------|-----------|
| [Introduction](https://github.com/pschatzmann/arduino-audio-tools/wiki/Introduction) | Core concepts, StreamCopy pattern |
| [Audio Sources and Sinks](https://github.com/pschatzmann/arduino-audio-tools/wiki/Audio-Sources-and-Sinks) | All available stream types |
| [Splitting and Merging Audio](https://github.com/pschatzmann/arduino-audio-tools/wiki/Splitting-and-Merging-Audio) | InputMixer, OutputMixer, InputMerge |
| [Encoding and Decoding](https://github.com/pschatzmann/arduino-audio-tools/wiki/Encoding-and-Decoding-of-Audio) | WAV, MP3, codec patterns |
| [Converting the Data Format](https://github.com/pschatzmann/arduino-audio-tools/wiki/Converting-the-Data-Format) | FormatConverterStream, bit depth |
| [Multicore Processing](https://github.com/pschatzmann/arduino-audio-tools/wiki/Multicore-Processing) | Task, BufferRTOS, QueueStream |
| [Performance](https://github.com/pschatzmann/arduino-audio-tools/wiki/Performance) | Buffer tuning, StreamCopy sizing |
| [Volume Control](https://github.com/pschatzmann/arduino-audio-tools/wiki/Volume-Control) | VolumeStream, panning |
| [Design Principles](https://github.com/pschatzmann/arduino-audio-tools/wiki/Design-Principles) | Library architecture |
| [Working with PlatformIO](https://github.com/pschatzmann/arduino-audio-tools/wiki/Working-with-PlatformIO) | Build configuration |

### API Reference (Key Classes)

| Class | Docs |
|-------|------|
| SineWaveGenerator | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_sine_wave_generator.html) |
| GeneratedSoundStream | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_generated_sound_stream.html) |
| InputMixer | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_input_mixer.html) |
| I2SStream | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_i2_s_stream.html) |
| StreamCopy | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_stream_copy.html) |
| EncodedAudioStream | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_encoded_audio_stream.html) |
| WAVDecoder | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_w_a_v_decoder.html) |
| AudioPlayer | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_audio_player.html) |
| VolumeStream | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_volume_stream.html) |
| FormatConverterStream | [API](https://pschatzmann.github.io/arduino-audio-tools/classaudio__tools_1_1_format_converter_stream.html) |

### Platform Config

- **RP2040 config:** `src/AudioTools/PlatformConfig/rp2040hower.h`
- **Global defaults:** `src/AudioToolsConfig.h`
