# HubFX Pico - Documentation Index

Comprehensive guide to all project documentation, organized by topic.

## 📚 Documentation Structure

### Hardware
- [WIRING.md](WIRING.md) - Complete pin assignments, I2S signal integrity requirements
- [CODECS.md](CODECS.md) - Audio codec architecture and selection guide  
- [TAS5825M.md](TAS5825M.md) - High-power amplifier (30W+) setup guide
- [PIN_VERIFICATION.md](PIN_VERIFICATION.md) - GPIO testing procedures

### Software
- [AUDIO_CONFIGURATION.md](AUDIO_CONFIGURATION.md) - Compile-time configuration (sample rate, I2C speed, buffers)
- [../README.md](../README.md) - Main project documentation and API reference
- [../tests/README.md](../tests/README.md) - Testing and verification scripts
- [../scripts/README.md](../scripts/README.md) - Build and flash utilities

## 🚀 Quick Navigation

### By Task

**Setting Up Hardware:**
1. [WIRING.md](WIRING.md) - Pin connections
2. [../README.md](../README.md) - Quick start guide

**Configuring Audio:**
1. [AUDIO_CONFIGURATION.md](AUDIO_CONFIGURATION.md) - Configuration options
2. [../src/audio/audio_config.h](../src/audio/audio_config.h) - Edit settings here

**Building & Testing:**
1. [../scripts/README.md](../scripts/README.md) - Build scripts
2. [../tests/README.md](../tests/README.md) - Test scripts

**Troubleshooting:**
- I2C errors → [AUDIO_CONFIGURATION.md](AUDIO_CONFIGURATION.md#i2c-speed-troubleshooting)
- Wiring issues → [WIRING.md](WIRING.md)
- Boot problems → Run `python ../tests/check_boot.py`

## 📖 Document Summaries

### WIRING.md
Hardware connection guide with:
- WM8960 Audio HAT connections (I2C, I2S, power)
- SD card module wiring (SPI)
- Pin assignment tables
- **Signal integrity critical info**: I2S wire length must be < 6 inches for 2.8 MHz BCLK
- Troubleshooting hardware issues

### CODECS.md
Audio codec architecture:
- Generic `AudioCodec` interface design
- WM8960 low-power codec (Waveshare HAT)
- TAS5825M high-power amplifier
- SimpleI2SCodec for basic I2S DACs
- Codec comparison table
- How to create custom codec drivers

### TAS5825M.md
TAS5825M amplifier reference:
- Hardware setup and power requirements (12-24V)
- I2C control and DSP programming
- Digital volume control (-100dB to +24dB)
- Supply voltage configuration
- Register reference and troubleshooting

### AUDIO_CONFIGURATION.md
Comprehensive configuration guide:
- Compile-time settings (`audio_config.h`)
- Sample rate selection (44.1k, 48k, 96k) with PLL calculations
- I2C speed tuning (50kHz default, adjustable 10k-100k)
- Buffer size optimization (latency vs stability trade-offs)
- I2S timing calculations (BCLK, LRCLK, SYSCLK)
- Wiring requirements for high-speed signals
- Debug flags and build examples

### PIN_VERIFICATION.md
GPIO testing procedures:
- Pin function verification methodology
- Expected signal characteristics
- Debugging pin assignment issues

## 🎯 Common Scenarios

### Scenario 1: First Time Setup
```
1. Read: README.md → Hardware overview
2. Read: WIRING.md → Connect hardware
3. Run:  scripts/build_and_flash.ps1
4. Test: tests/check_boot.py
```

### Scenario 2: Changing to 48kHz
```
1. Read: AUDIO_CONFIGURATION.md → Understand PLL settings
2. Edit: src/audio/audio_config.h → Set AUDIO_SAMPLE_RATE=48000
3. Run:  scripts/build_and_flash.ps1
4. Test: tests/test_codec.py
```

### Scenario 3: I2C Timeout Errors
```
1. Test: tests/test_i2c_scan.py → Find device address
2. Test: tests/test_codec.py → Check communication
3. Read: AUDIO_CONFIGURATION.md → I2C speed section
4. Edit: src/audio/audio_config.h → Lower WM8960_I2C_SPEED to 10000
5. Run:  scripts/build_and_flash.ps1
```

### Scenario 4: Adding TAS5825M
```
1. Read: TAS5825M.md → Hardware requirements
2. Read: CODECS.md → Codec architecture
3. Read: WIRING.md → Pin connections
4. Edit: src/hubfx_pico.ino → Switch codec type
5. Run:  scripts/build_and_flash.ps1
```

## 🔧 Configuration Quick Reference

| Setting | File | Default | Common Values |
|---------|------|---------|---------------|
| Sample Rate | `audio_config.h` | 44100 | 44100, 48000, 22050 |
| I2C Speed | `audio_config.h` | 50000 | 10000 (slow), 50000 (default), 100000 (fast) |
| Mix Channels | `audio_config.h` | 8 | 4-8 |
| Buffer Size | `audio_config.h` | 512 | 128 (low latency), 512 (balanced), 1024 (efficient) |

See [AUDIO_CONFIGURATION.md](AUDIO_CONFIGURATION.md) for complete reference.

## 🔍 Search Index

| Term | Location |
|------|----------|
| BCLK frequency | AUDIO_CONFIGURATION.md |
| I2C timeout | AUDIO_CONFIGURATION.md |
| I2S DATA pin | WIRING.md (GP6 → Pin 38) |
| PLL K value | AUDIO_CONFIGURATION.md |
| Sample rate | AUDIO_CONFIGURATION.md, audio_config.h |
| TAS5825M volume | TAS5825M.md |
| WM8960 power | WIRING.md |
| Wire length | WIRING.md, AUDIO_CONFIGURATION.md |

## 📦 External Links

- [Raspberry Pi Pico Documentation](https://www.raspberrypi.com/documentation/microcontrollers/raspberry-pi-pico.html)
- [WM8960 Audio HAT Wiki](https://www.waveshare.com/wiki/WM8960_Audio_HAT)
- [TAS5825M Product Page](https://www.ti.com/product/TAS5825M)
- [PlatformIO Documentation](https://docs.platformio.org/)
- [Arduino-Pico Core](https://arduino-pico.readthedocs.io/)

---

**Last Updated:** After documentation consolidation (Build 100+)
