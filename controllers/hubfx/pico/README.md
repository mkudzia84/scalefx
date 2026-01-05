# HubFX Pico

High-performance audio and effects controller for scale RC models, built on Raspberry Pi Pico.

## Overview

HubFX Pico is a dual-core audio system that provides:
- **8-channel audio mixing** with glitch-free playback
- **Engine sound effects** with automatic state management
- **Gun effects coordination** via USB host
- **Generic codec support** (WM8960, TAS5825M, or simple I2S DACs)
- **SD card storage** for audio files and configuration
- **Serial command interface** for real-time control

## Quick Start

### Hardware Setup

**Minimum Configuration:**
```
Raspberry Pi Pico → I2S DAC → Speakers
                  → SD Card Module
```

**Full Configuration:**
```
Raspberry Pi Pico → Audio Codec (WM8960/TAS5825M) → Speakers
                  → SD Card Module → WAV files
                  → USB Hub → GunFX Controllers
```

**Important:** See [docs/WIRING.md](docs/WIRING.md) for:
- Complete pinout diagrams
- I2S signal integrity requirements (wire length < 6 inches for BCLK @ 2.8 MHz)
- Power connection details

### Pin Connections

| Function | Pico Pin | Notes |
|----------|----------|-------|
| **I2S Audio** |
| DATA     | GP6      | I2S DIN to codec Pin 38 (ADC) |
| BCLK     | GP7      | Bit clock (2.8 MHz @ 44.1kHz) |
| LRCLK    | GP8      | Word select (44.1 kHz) |
| **I2C Control** (codecs only) |
| SDA      | GP4      | For WM8960/TAS5825M |
| SCL      | GP5      | For WM8960/TAS5825M |
| **SD Card (SPI0)** |
| CS       | GP17     | Chip select |
| SCK      | GP18     | SPI clock (20 MHz) |
| MOSI     | GP19     | Master out |
| MISO     | GP16     | Master in |

### Software Setup

1. **Install PlatformIO:**
   ```bash
   pip install platformio
   ```

2. **Build and upload:**
   ```bash
   cd controllers/hubfx/pico
   python -m platformio run -t upload
   ```
   Or use automated script:
   ```powershell
   .\scripts\build_and_flash.ps1
   ```

3. **Prepare SD card:**
   - Format as FAT32
   - Copy `config.yaml` to root
   - Create `/sounds/` folder with WAV files (44.1kHz, 16-bit, stereo recommended)

4. **Connect serial monitor:**
   ```bash
   python -m platformio device monitor -b 115200
   ```

## Features

### Dual-Core Architecture

| Core | Responsibility | Frequency |
|------|---------------|-----------|
| **Core 0** | Main loop, serial commands, configuration, effects state machines | As needed |
| **Core 1** | **Dedicated audio processing** - mixing, I2S DMA, codec control | Real-time |

This architecture ensures **glitch-free audio** even during heavy processing on Core 0.

### Audio System

- **8 independent channels** with individual volume control
- **Stereo routing**: Left, Right, or Stereo output per channel
- **Looping support** with seamless playback
- **Fade in/out** with configurable duration
- **Format support**: 16-bit/8-bit WAV, mono/stereo, 8-48kHz sample rates
- **Thread-safe API** for safe cross-core operation

### Audio Codec Support

HubFX supports multiple audio codec types via a generic interface:

| Codec | Type | Power | I2C Required | Best For |
|-------|------|-------|--------------|----------|
| **WM8960** | All-in-one | 1W | Yes | Development, low-power |
| **TAS5825M** | High-power | 30W+ | Yes | Production, high volume |
| **PCM5102/PT8211** | Simple I2S | Line-level | No | Testing, external amps |

See [docs/CODECS.md](docs/CODECS.md) for codec architecture and [docs/TAS5825M.md](docs/TAS5825M.md) for high-power amplifier setup.

### Effects Modules

**Engine FX:**
- Automatic startup/running/shutdown sounds
- PWM throttle monitoring
- Crossfade transitions
- Configurable audio offsets

**Gun FX** (via USB host):
- Multiple GunFX controller support
- USB CDC communication
- Coordinated firing effects
- Recoil and smoke control

### Configuration

YAML-based configuration stored in flash (LittleFS) or SD card:

```yaml
device_name: "Tiger Tank"
master_volume: 0.8

engine_fx:
  enabled: true
  engine_toggle:
    input_channel: 1
    threshold_us: 1500
  sounds:
    starting: "/sounds/engine_start.wav"
    running: "/sounds/engine_loop.wav"
    stopping: "/sounds/engine_stop.wav"
```

## Serial Commands

Connect at 115200 baud for interactive control:

### Audio Commands
```bash
play 0 /sounds/engine.wav loop vol 0.5    # Play looping on channel 0
play 1 /sounds/fire.wav                   # Play one-shot on channel 1
stop 0                                    # Stop channel 0
fade 2                                    # Fade out channel 2
volume 0.8                                # Set master volume to 80%
status                                    # Show all channel states
```

### Configuration Commands
```bash
config                  # Show loaded configuration
config download         # Download config file
config upload <size>    # Upload new config
config reload           # Reload config without restart
config restart          # Reload and restart
```

### SD Card Commands
```bash
ls /                    # List root directory
ls /sounds              # List sounds directory
tree                    # Show directory tree
cat /config.yaml        # Display file contents
sdinfo                  # Show SD card information
```

### Engine Control
```bash
engine                  # Show engine state
engine start            # Force engine start
engine stop             # Force engine stop
```

Type `help` for full command list.

## Code Organization

```
pico/
├── src/
│   ├── hubfx_pico.ino         # Main application
│   ├── audio/                 # Audio subsystem
│   │   ├── audio_codec.h      # Generic codec interface
│   │   ├── audio_mixer.h/cpp  # 8-channel mixer (Core 1)
│   │   ├── wm8960_codec.*     # WM8960 I2C driver
│   │   ├── tas5825_codec.*    # TAS5825M I2C driver
│   │   └── simple_i2s_codec.* # Simple I2S DACs
│   ├── storage/               # Storage & configuration
│   │   ├── sd_card.h/cpp      # SD card module
│   │   └── config_reader.*    # YAML parser
│   └── effects/               # Special effects
│       ├── engine_fx.h/cpp    # Engine sound effects
│       └── gun_fx.h/cpp       # Gun effects coordinator
├── docs/                      # Technical documentation
│   ├── WIRING.md             # Hardware connections
│   ├── CODECS.md             # Codec architecture
│   └── TAS5825M.md           # High-power amplifier guide
├── platformio.ini            # Build configuration
└── config.yaml               # Example configuration
```

## Building

### PlatformIO (Recommended)

```bash
# Build for production
pio run -e pico

# Build for debugging
pio run -e pico_debug

# Upload (Pico in BOOTSEL mode)
pio run -t upload

# Clean build
pio run -t clean
```

### Arduino IDE

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software)
2. Add board support: File → Preferences → Additional Board URLs:
   ```
   https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
   ```
3. Tools → Board → Raspberry Pi Pico
4. Tools → CPU Speed → 120 MHz (for USB host support)
5. Open `src/hubfx_pico.ino` and upload

## Documentation

| Document | Description |
|----------|-------------|
| [docs/WIRING.md](docs/WIRING.md) | Complete hardware wiring diagrams and pin assignments |
| [docs/CODECS.md](docs/CODECS.md) | Audio codec architecture and driver implementation |
| [docs/TAS5825M.md](docs/TAS5825M.md) | TAS5825M high-power amplifier setup guide |
| [docs/AUDIO_CONFIGURATION.md](docs/AUDIO_CONFIGURATION.md) | Compile-time audio configuration and tuning |
| [tests/README.md](tests/README.md) | Automated testing and verification scripts |
| [scripts/README.md](scripts/README.md) | Build, flash, and file transfer utilities |

## API Reference

### Audio Mixer

```cpp
// Initialize (done in setup())
mixer.begin(&sdCard.getSd(), i2s_data, i2s_bclk, i2s_lrclk, codec);

// Play audio (thread-safe from Core 0)
AudioPlaybackOptions opts;
opts.loop = true;
opts.volume = 0.8f;
opts.output = AudioOutput::Stereo;
mixer.playAsync(0, "/sounds/engine.wav", opts);

// Stop playback
mixer.stopAsync(0, AudioStopMode::Fade);  // Fade out
mixer.stopAsync(1, AudioStopMode::Immediate);  // Immediate stop

// Volume control
mixer.setVolumeAsync(0, 0.5f);   // Channel 0 to 50%
mixer.setVolumeAsync(-1, 0.8f);  // Master to 80%

// Query status (safe to read from Core 0)
bool playing = mixer.isPlaying(0);
int remaining = mixer.remainingMs(0);
```

### Audio Codec

```cpp
// WM8960 example
WM8960Codec codec;
codec.begin(Wire, sda_pin, scl_pin, 44100);
codec.setVolume(0.7f);
codec.enableSpeakers(true);

// TAS5825M example
TAS5825Codec codec;
codec.begin(Wire, sda_pin, scl_pin, 44100, TAS5825M_20V);
codec.setVolumeDB(-6.0f);  // -6dB digital volume
codec.setMute(false);

// Pass to mixer
mixer.begin(&sd, data, bclk, lrclk, &codec);
```

### Engine FX

```cpp
EngineFX engineFx;

// Initialize with settings and mixer reference
engineFx.begin(settings, &mixer);

// Call in loop() to update state machine
engineFx.process();

// Manual control
engineFx.forceStart();
engineFx.forceStop();

// Query state
EngineState state = engineFx.state();
```

## Troubleshooting

### No Audio Output

1. **Check I2S wiring** - Verify GP6/7/8 connections
2. **Check codec initialization** - Look for I2C errors in serial output
3. **Verify SD card** - Use `ls /sounds` to confirm files exist
4. **Test playback** - Use `play 0 /sounds/test.wav`
5. **Check volume** - Try `volume 1.0` for maximum

### SD Card Not Detected

1. **Check wiring** - Verify SPI connections (GP16/17/18/19)
2. **Format check** - Must be FAT32
3. **Card speed** - Try different SD card if issues persist
4. **Power** - Ensure 3.3V to SD module

### Distorted Audio

1. **Lower volume** - Try `volume 0.5` or codec `setVolume(0.5f)`
2. **Check WAV format** - Must be uncompressed PCM
3. **Sample rate mismatch** - Prefer 44.1kHz files
4. **Clipping** - Reduce individual channel volumes

### Configuration Issues

1. **YAML syntax** - Use spaces (not tabs), check colons
2. **File location** - Must be `/config.yaml` on SD or in flash
3. **Reload config** - Use `config reload` to reapply
4. **Check output** - Use `config` command to see loaded values

### Build Errors

1. **Missing libraries** - PlatformIO auto-installs, but check `lib_deps`
2. **Include paths** - Files moved to subdirectories (audio/, storage/, effects/)
3. **Clean build** - Try `pio run -t clean` then rebuild
4. **Core mismatch** - Ensure earlephilhower/arduino-pico core is used

## Performance Notes

- **RAM usage**: ~15% (39KB) - 8 channels with 512-sample buffers
- **Flash usage**: ~9% (204KB) - includes dual-core audio engine
- **Audio latency**: <12ms - Double-buffered I2S DMA
- **Mixing overhead**: ~3-6ms per frame - Core 1 dedicated
- **SD read speed**: ~25MHz SPI - Fast enough for 8 simultaneous channels

## Technical Specifications

- **Platform**: RP2040 @ 120MHz (dual-core Cortex-M0+)
- **Audio**: I2S master, 44.1kHz stereo, 16-bit
- **Storage**: SPI SD card (FAT32), LittleFS flash
- **Communication**: USB CDC serial @ 115200 baud
- **I2C**: 100kHz standard mode for codec control
- **Build system**: PlatformIO with Arduino framework

## Credits

- **Audio mixer**: Dual-core DMA-based mixing engine
- **TAS5825M driver**: Based on [bassowl-hat](https://github.com/Darmur/bassowl-hat) initialization sequences
- **WM8960 driver**: Based on Cirrus Logic/Wolfson WM8960 datasheet

## License

MIT License - See main project LICENSE file.
