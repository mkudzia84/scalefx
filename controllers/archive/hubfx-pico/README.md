# HubFX Pico 2

> **⚠️ OBSOLETE — This codebase is frozen.** All new HubFX development targets `controllers/hubfx/esp32s3/`. This Pico variant is preserved as a reference implementation only. Do not add features, fix bugs, or extend the protocol here unless explicitly requested for hardware compatibility.

High-performance audio and effects controller for scale RC models, built on Raspberry Pi Pico 2 (RP2350).

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
Raspberry Pi Pico 2 → I2S DAC → Speakers
                    → SD Card Module
```

**Full Configuration:**
```
Raspberry Pi Pico 2 → Audio Codec (WM8960/TAS5825M) → Speakers
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
   python -m platformio device monitor -b 1000000
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

## Serial Protocol

Binary COBS protocol at 1Mbps baud. See `controllers/lib/sfx_common/serial/PROTOCOL.md` for
the full wire format specification.

### Packet Type Allocation (0x80-0xA8)

| Range | Domain | Handler |
|-------|--------|--------|
| 0x80-0x83 | Slave management | SlaveServer |
| 0x84-0x8B | Audio control | AudioServer |
| 0x8C-0x8F | Engine FX | EngineServer |
| 0x90-0x95 | Config & SD card | StorageServer |
| 0x96-0x98 | Slave routing (subcmd) | SlaveServer |
| 0x99 | Flash status | StorageServer |
| 0x9A-0xA3 | File operations | StorageServer |
| 0xA4-0xA6 | Streaming (library) | `core/stream.h` |
| 0xA7-0xA8 | USB host diagnostics | SlaveServer |
| 0xF0-0xFF | Core system | CoreCommandServer |

### Slave Management

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `SLAVE_LIST` | 0x80 | (none) | SLAVE_LIST_RESP |
| `SLAVE_LIST_RESP` | 0x81 | `[count:u8][entries...]` | — |
| `SLAVE_INIT` | 0x82 | `[slaveType:u8]` | ACK/NACK |
| `SLAVE_STATUS` | 0x83 | (none) | ACK (status via core callback) |

Slave types: 1=GunFX, 2=LightFX, 3=GearControl

### USB Host Diagnostics

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `USB_DEVICES_REQ` | 0xA7 | (none) | USB_DEVICES_RESP |
| `USB_DEVICES_RESP` | 0xA8 | see below | — |

**USB_DEVICES_RESP payload:**

| Field | Type | Description |
|-------|------|-------------|
| initialized | u8 | USB Host initialized (0/1) |
| taskRunning | u8 | USB Host task running (0/1) |
| backendLen | u8 | Backend name length |
| backend | str | Backend name ("PIO-USB" or "HW USB-OTG") |
| deviceCount | u8 | Number of CDC devices |
| *per device:* | | |
| addr | u8 | USB device address |
| vid | u16LE | Vendor ID |
| pid | u16LE | Product ID |
| state | u8 | 0=Disconnected, 1=Connected, 2=Mounted, 3=Ready |
| slaveType | u8 | Identified slave type (0=Unknown, 1=GunFX, 2=LightFX, 3=GearControl) |

### Audio Control

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `AUDIO_PLAY` | 0x84 | `[ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]` | ACK/NACK |
| `AUDIO_STOP` | 0x85 | `[ch:u8]` (0xFF=all) | ACK |
| `AUDIO_VOLUME` | 0x86 | `[ch:u8][vol:u8]` (0xFF=master, 0-100) | ACK |
| `AUDIO_FADE` | 0x87 | `[ch:u8]` | ACK |
| `AUDIO_QUEUE` | 0x88 | `[ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]` | ACK/NACK |
| `AUDIO_QUEUE_CLEAR` | 0x89 | `[ch:u8]` (0xFF=all) | ACK |
| `AUDIO_STATUS_REQ` | 0x8A | (none) | AUDIO_STATUS_RESP |
| `AUDIO_STATUS_RESP` | 0x8B | see below | — |

**AUDIO_STATUS_RESP (v2) payload:**

| Field | Type | Description |
|-------|------|-------------|
| masterVol | u8 | Master volume 0-100 |
| flags | u8 | bit0=initialized, bit1=i2sRunning, bit2=hasCodec |
| sampleRate_Hz | u16LE | I2S sample rate (e.g. 44100) |
| bitDepth | u8 | I2S bit depth (e.g. 16) |
| maxChannels | u8 | Maximum mixer channels (8) |
| codecNameLen | u8 | Length of codec name string |
| codecName | str | Codec model name (e.g. "TAS5825M") |
| activeMask | u8 | Bitmask of active channels |
| *per active channel:* | | |
| ch | u8 | Channel index 0-7 |
| vol | u8 | Channel volume 0-100 |
| playing | u8 | 1=playing, 0=queued only |
| looping | u8 | 1=looping, 0=one-shot |
| loopCount | u16LE | Remaining loops (0xFFFF=infinite) |
| remaining_ms | u32LE | Remaining playback time (ms, shown as s.ms) |
| queueLen | u8 | Number of queued sounds |
| output | u8 | 0=stereo, 1=left, 2=right |
| wavRate_Hz | u16LE | WAV sample rate |
| wavCh | u8 | WAV channels (1=mono, 2=stereo) |
| wavBits | u8 | WAV bits per sample (8/16) |
| filenameLen | u8 | Length of filename string |
| filename | str | Currently playing filename |

Audio outputs: 0=Stereo, 1=Left, 2=Right.
Loop modes: 0=None, 1=Finite(N), 2=Infinite.

### Engine FX Control

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `ENGINE_START` | 0x8C | (none) | ACK/NACK |
| `ENGINE_STOP` | 0x8D | (none) | ACK |
| `ENGINE_STATUS_REQ` | 0x8E | (none) | ENGINE_STATUS_RESP |
| `ENGINE_STATUS_RESP` | 0x8F | `[state:u8][toggleEngaged:u8][active:u8]` | — |

Engine states: 0=Stopped, 1=Starting, 2=Running, 3=Stopping.

### Config & SD Card

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `CONFIG_RELOAD` | 0x90 | (none) | ACK/NACK |
| `CONFIG_GET` | 0x91 | (none) | CONFIG_GET_RESP |
| `CONFIG_GET_RESP` | 0x92 | `[loaded:u8][size:u16LE][reserved:u8]` | — |
| `SD_INIT` | 0x93 | `[speed_mhz:u8]` (1-50, default 20) | ACK/NACK |
| `SD_STATUS_REQ` | 0x94 | (none) | SD_STATUS_RESP |
| `SD_STATUS_RESP` | 0x95 | `[initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]` | — |

### Slave Routing (Subcmd Pattern)

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `SLAVE_ROUTE_GUNFX` | 0x96 | `[subcmd:u8][original_payload...]` | Forwarded to GunFX |
| `SLAVE_ROUTE_LIGHTFX` | 0x97 | `[subcmd:u8][original_payload...]` | Forwarded to LightFX |
| `SLAVE_ROUTE_GEARCONTROL` | 0x98 | `[subcmd:u8][original_payload...]` | Forwarded to GearControl |

The `subcmd` byte is the original slave packet type. The hub extracts it, creates
a new packet with that type and the remaining payload, and forwards to the slave.

### Diagnostics

| Command | Value | Payload | Direction |
|---------|-------|---------|-----------|
| `LOG_MESSAGE` | 0x99 | `[level:u8][millis:u32LE][message:str]` | Server→Client (async) |

Log levels: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR. Sent with `TAG_ASYNC` (0x00).

### File Operations

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `FILE_LIST` | 0x9A | `[pathLen:u8][path:str]` | Streamed (STREAM_BEGIN/DATA/END) |
| `FILE_DELETE` | 0x9B | `[pathLen:u8][path:str]` | ACK/NACK |
| `FILE_MKDIR` | 0x9C | `[pathLen:u8][path:str]` | ACK/NACK |
| `FILE_INFO` | 0x9D | `[pathLen:u8][path:str]` | FILE_INFO_RESP |
| `FILE_INFO_RESP` | 0x9E | `[exists:u8][isDir:u8][size:u32LE]` | — |
| `FILE_DOWNLOAD` | 0x9F | `[pathLen:u8][path:str]` | Streamed (STREAM_BEGIN/DATA/END) |
| `FILE_UPLOAD_BEGIN` | 0xA0 | `[size:u32LE][pathLen:u8][path:str]` | ACK |
| `FILE_UPLOAD_DATA` | 0xA1 | `[seqNum:u16LE][crc16:u16LE][data:N]` | ACK/NACK(CRC_ERROR) |
| `FILE_UPLOAD_END` | 0xA2 | (none) | ACK/NACK |
| `FILE_UPLOAD_CANCEL` | 0xA3 | (none) | ACK |

Downloads use `StreamWriter` (fire-and-forget with end-of-stream CRC-16).
Uploads use per-chunk ACK/NACK with CRC-16 retry.
See `PROTOCOL.md § File Transfer Protocol` for detailed flow diagrams.

### Error Codes (0x80-0x8F)

| Code | Name | Description |
|------|------|-------------|
| 0x80 | `SLAVE_NOT_FOUND` | No slave of requested type |
| 0x81 | `SLAVE_NOT_CONNECTED` | Slave registered but not connected |
| 0x82 | `SLAVE_INIT_FAILED` | Slave INIT handshake failed |
| 0x83 | `NO_SLAVES` | No slaves registered |
| 0x84 | `SLAVE_COMM_ERROR` | Communication error with slave |
| 0x85 | `AUDIO_ERROR` | Audio system error |
| 0x86 | `SD_NOT_INITIALIZED` | SD card not initialized |
| 0x87 | `ENGINE_NOT_AVAILABLE` | Engine FX not configured |
| 0x88 | `CONFIG_ERROR` | Config load/reload failed |
| 0x89 | `INVALID_CHANNEL` | Audio channel out of range |
| 0x8A | `FILE_NOT_FOUND` | File or directory not found |
| 0x8B | `FILE_ALREADY_EXISTS` | Path exists but wrong type |
| 0x8C | `FILE_IO_ERROR` | SD card read/write error |
| 0x8D | `FILE_TOO_LARGE` | File exceeds size limit |
| 0x8E | `UPLOAD_IN_PROGRESS` | Another upload is active |
| 0x8F | `NO_UPLOAD_ACTIVE` | No upload in progress |

### CLI Commands

Connect via the Python interactive CLI:
```bash
python -m tests.cli.interactive --port COM5
```

## Code Organization

```
pico/
├── src/
│   ├── hubfx_pico.ino         # Main application
│   ├── audio/                 # Audio subsystem (HubFX-specific)
│   │   ├── audio_channels.h   # Channel assignment constants
│   │   ├── audio_server.h/cpp # HubFX audio command handler
│   │   └── system_sounds.h    # System sound path constants
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

> **Note:** The core audio mixer, codecs, ring buffer, and I2S backend live in the shared
> library at `controllers/lib/sfx_common/audio/`. HubFX `src/audio/` contains only
> controller-specific code (command handling, channel assignments, system sounds).

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
3. Tools → Board → Raspberry Pi Pico 2
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

- **Platform**: RP2350 @ 120MHz (dual-core Cortex-M33)
- **Audio**: I2S master, 44.1kHz stereo, 16-bit
- **Storage**: SPI SD card (FAT32), LittleFS flash
- **Communication**: USB CDC serial @ 1Mbps baud
- **I2C**: 100kHz standard mode for codec control
- **Build system**: PlatformIO with Arduino framework

## Credits

- **Audio mixer**: Dual-core DMA-based mixing engine
- **TAS5825M driver**: Based on [bassowl-hat](https://github.com/Darmur/bassowl-hat) initialization sequences
- **WM8960 driver**: Based on Cirrus Logic/Wolfson WM8960 datasheet

## License

MIT License - See main project LICENSE file.
