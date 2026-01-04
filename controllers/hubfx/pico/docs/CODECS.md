# Audio Codec Architecture

## Overview

The HubFX audio system uses a **generic codec interface** that allows different audio DAC/codec chips to be used interchangeably. This provides flexibility for hardware selection without changing the core audio mixer code.

---

## Architecture

```
┌─────────────────────┐
│   HubFX Main App    │
│   (hubfx_pico.ino)  │
└──────────┬──────────┘
           │
           │ uses
           ▼
┌─────────────────────┐       ┌─────────────────────┐
│   AudioMixer        │──────▶│   AudioCodec        │ (abstract)
│   (audio mixing)    │       │   (base interface)  │
└─────────────────────┘       └──────────┬──────────┘
                                         │
                        ┌────────────────┼────────────────┐
                        │                │                │
                        ▼                ▼                ▼
              ┌─────────────────┐ ┌──────────────┐ ┌──────────────┐
              │  WM8960Codec    │ │ SimpleI2S    │ │  YourCodec   │
              │  (I2S + I2C)    │ │ (I2S only)   │ │  (custom)    │
              └─────────────────┘ └──────────────┘ └──────────────┘
```

---

## AudioCodec Base Class

Located in [audio_codec.h](src/audio_codec.h)

### Required Methods

All codec implementations must provide:

```cpp
virtual bool begin(uint32_t sample_rate = 44100) = 0;
virtual void reset() = 0;
virtual void setVolume(float volume) = 0;  // 0.0 to 1.0
virtual void setMute(bool mute) = 0;
virtual bool isInitialized() const = 0;
virtual const char* getModelName() const = 0;
```

### Optional Methods

Codecs with advanced features can override:

```cpp
virtual void enableSpeakers(bool enable);
virtual void enableHeadphones(bool enable);
virtual void setHeadphoneVolume(uint8_t volume);
virtual void setSpeakerVolume(uint8_t volume);
virtual void dumpRegisters();
```

---

## Included Codec Implementations

### 1. WM8960Codec

**File:** [wm8960_codec.h](src/wm8960_codec.h), [wm8960_codec.cpp](src/wm8960_codec.cpp)

**Hardware:** Waveshare WM8960 Audio HAT, Cirrus Logic WM8960

**Features:**
- I2S audio interface (slave mode)
- I2C control (address 0x1A)
- Stereo DAC with 98dB SNR
- Separate speaker and headphone amplifiers
- Class D speaker output (1W per channel)
- Headphone output (40mW per channel)
- Independent volume controls
- Hardware mute

**Wiring:**
- I2S: GP6 (DATA), GP7 (BCLK), GP8 (LRCLK)
- I2C: GP4 (SDA), GP5 (SCL)

**Initialization:**
```cpp
WM8960Codec wm8960;
wm8960.begin(Wire, 4, 5, 44100);  // I2C on GP4/GP5
wm8960.enableSpeakers(true);
wm8960.setVolume(0.7f);
```

### 2. SimpleI2SCodec

**File:** [simple_i2s_codec.h](src/simple_i2s_codec.h), [simple_i2s_codec.cpp](src/simple_i2s_codec.cpp)

**Hardware:** Simple I2S DACs without control interface
- PCM5102 (TI) - 32-bit 384kHz
- PT8211 (Princeton) - 16-bit 48kHz
- UDA1334 (NXP) - 24-bit 96kHz
- MAX98357 (Maxim) - I2S Class D amp

**Features:**
- I2S audio interface only
- Auto-configuration from I2S signals
- Optional GPIO mute/gain control
- No I2C/SPI required

**Wiring:**
- I2S: GP6 (DATA/DIN), GP7 (BCLK), GP8 (LRCLK/WS)
- Optional: mute/gain control pins

**Initialization:**
```cpp
SimpleI2SCodec pcm5102("PCM5102");
pcm5102.begin(44100);  // Auto-configure from I2S
```

### 3. TAS5825Codec

**File:** [tas5825_codec.h](src/tas5825_codec.h), [tas5825_codec.cpp](src/tas5825_codec.cpp)

**Hardware:** TI TAS5825M Digital Audio Amplifier
- Used in Bassowl HAT and similar projects

**Features:**
- I2S audio interface (slave mode)
- I2C control (address 0x4C)
- Stereo Class-D amplifier with DSP
- Digital volume control (0.5dB steps, -100dB to +24dB)
- Built-in EQ/DRC via DSP
- Multiple supply voltage support (12V, 15V, 20V, 24V)
- Fault monitoring and protection
- Book/Page register architecture

**Wiring:**
- I2S: GP6 (DATA), GP7 (BCLK), GP8 (LRCLK)
- I2C: GP4 (SDA), GP5 (SCL)

**Initialization:**
```cpp
TAS5825Codec tas5825;
tas5825.begin(Wire, 4, 5, 44100, TAS5825M_20V);  // I2C on GP4/GP5, 20V supply
tas5825.setVolumeDB(0.0f);  // 0dB
```

**Advanced Features:**
```cpp
tas5825.setVolumeDB(-6.0f);   // Set volume in dB
tas5825.clearFaults();        // Clear fault status
tas5825.dumpRegisters();      // Debug register dump
```

**Note:** The TAS5825M has extensive DSP capabilities. The current implementation provides basic initialization. For full DSP programming with custom EQ/DRC, see the [bassowl-hat project](https://github.com/Darmur/bassowl-hat) which includes PPC3-generated coefficient files.

---

## Using Different Codecs

### In hubfx_pico.ino

**Step 1: Choose codec** (uncomment ONE):
```cpp
#define USE_WM8960_CODEC      // Waveshare WM8960 Audio HAT
// #define USE_TAS5825_CODEC     // TI TAS5825M Digital Amp
// #define USE_SIMPLE_I2S_CODEC  // Simple I2S DAC
```

**Step 2: Codec automatically selected:**
```cpp
#ifdef USE_WM8960_CODEC
WM8960Codec audioCodec;
AudioCodec* codec = &audioCodec;
#elif defined(USE_TAS5825_CODEC)
TAS5825Codec audioCodec;
AudioCodec* codec = &audioCodec;
#elif defined(USE_SIMPLE_I2S_CODEC)
SimpleI2SCodec audioCodec("PCM5102");
AudioCodec* codec = &audioCodec;
#else
AudioCodec* codec = nullptr;  // No codec
#endif
```

**Step 3: Initialize in init_audio():**
```cpp
if (codec) {
    #ifdef USE_WM8960_CODEC
    audioCodec.begin(Wire, 4, 5, 44100);
    audioCodec.enableSpeakers(true);
    
    #elif defined(USE_TAS5825_CODEC)
    audioCodec.begin(Wire, 4, 5, 44100, TAS5825M_20V);  // 20V supply
    audioCodec.setVolumeDB(0.0f);  // 0dB
    
    #elif defined(USE_SIMPLE_I2S_CODEC)
    audioCodec.begin(44100);
    #endif
}

mixer.begin(&sdCard.getSd(), I2S_DATA, I2S_BCLK, I2S_LRCLK, codec);
```

---

## Creating a Custom Codec

### Example: CS4344 Codec

```cpp
// cs4344_codec.h
#include "audio_codec.h"

class CS4344Codec : public AudioCodec {
public:
    bool begin(uint32_t sample_rate = 44100) override {
        // Initialize CS4344-specific hardware
        pinMode(MUTE_PIN, OUTPUT);
        digitalWrite(MUTE_PIN, HIGH);  // Unmute
        
        initialized = true;
        return true;
    }
    
    void reset() override {
        // Reset CS4344
    }
    
    void setVolume(float volume) override {
        // CS4344 has no volume control - handled by mixer
        currentVolume = volume;
    }
    
    void setMute(bool mute) override {
        digitalWrite(MUTE_PIN, !mute);  // Active-high unmute
    }
    
    bool isInitialized() const override { return initialized; }
    const char* getModelName() const override { return "CS4344"; }
    
private:
    static constexpr int MUTE_PIN = 10;
    bool initialized = false;
    float currentVolume = 1.0f;
};
```

### Usage:
```cpp
// In hubfx_pico.ino
#define USE_CS4344_CODEC

#ifdef USE_CS4344_CODEC
CS4344Codec audioCodec;
AudioCodec* codec = &audioCodec;
#endif
```

---

## Codec Comparison

| Feature | WM8960 | SimpleI2S | TAS5825M | Custom |
|---------|--------|-----------|----------|--------|
| Control Interface | I2C | None | I2C | Varies |
| Volume Control | Hardware | Software | Hardware (DSP) | Varies |
| Mute Control | Hardware | Optional GPIO | Hardware | Varies |
| Speaker Amp | Yes (1W) | No | Yes (High Power) | Varies |
| Headphone Amp | Yes (40mW) | No | No | Varies |
| DSP/EQ | No | No | Yes (Advanced) | Varies |
| Supply Voltage | 3.3V | 3.3V | 12-24V | Varies |
| Power Output | 1W | N/A | 30W+ (depends) | Varies |
| Complexity | High | Low | Very High | Varies |
| Cost | $$$ | $ | $$$$ | Varies |
| Best For | All-in-one | Development | High-power audio | - |

---

## Benefits of Generic Interface

1. **Flexibility:** Swap codecs without changing mixer code
2. **Testing:** Use simple codec for development, advanced for production
3. **Cost Optimization:** Choose codec based on budget/features
4. **Modularity:** Easy to add new codec support
5. **Abstraction:** Main code doesn't need codec-specific details

---

## AudioMixer Integration

The AudioMixer accepts an optional `AudioCodec*` pointer:

```cpp
bool AudioMixer::begin(SdFat* sd, 
                       uint8_t i2s_data_pin, 
                       uint8_t i2s_bclk_pin, 
                       uint8_t i2s_lrclk_pin,
                       AudioCodec* codec = nullptr);
```

- **With codec:** Mixer reports codec name and can use codec features
- **Without codec (nullptr):** I2S-only mode, software volume only

---

## Future Codec Support

Potential codecs to add:

- **PCM5122** (TI) - I2C control, 112dB SNR
- **ES8388** (Everest) - Stereo codec with ADC
- **TLV320AIC3254** (TI) - Programmable miniDSP
- **WM8731** (Cirrus) - Classic audio codec
- **CS43L22** (Cirrus) - Low-power headphone codec

Each requires implementing the `AudioCodec` interface with codec-specific initialization and control.

---

## Additional Documentation

- **TAS5825M Details:** See [TAS5825M_README.md](TAS5825M_README.md) for comprehensive TAS5825M usage, DSP programming, and troubleshooting
- **Wiring Guide:** See [WIRING.md](../../../docs/WIRING.md) for complete hardware connection diagrams

---

## References

- [audio_codec.h](src/audio_codec.h) - Base interface
- [wm8960_codec.h](src/wm8960_codec.h) - WM8960 implementation
- [simple_i2s_codec.h](src/simple_i2s_codec.h) - Simple I2S implementation
- [audio_mixer.h](src/audio_mixer.h) - Mixer integration
