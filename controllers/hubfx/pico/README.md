# HubFX Pico

Scale model special effects controller for Raspberry Pi Pico.

## Dual-Core Architecture

HubFX leverages the RP2040's dual-core processor for glitch-free audio:

| Core | Responsibility |
|------|---------------|
| **Core 0** | Main loop, serial commands, config, engine/gun FX logic |
| **Core 1** | Dedicated audio processing AND USB host task |

This ensures audio playback is never interrupted by other processing tasks.

## Features

- **8-Channel Audio Mixer**: Software mixer with I2S output
- **WAV Playback**: Load and play 16-bit/8-bit WAV files from SD card
- **Stereo Output Routing**: Route each channel to Left, Right, or Stereo
- **Normalized Mixing**: Soft clipping prevents harsh digital distortion
- **USB Host**: Connect to GunFX boards via USB (PIO-USB)
- **YAML Configuration**: Human-readable config file on SD card
- **Thread-Safe API**: Command queue for safe cross-core communication
- **Modular Design**: Separate modules for audio, config, engine FX, gun FX

## Hardware Requirements

- Raspberry Pi Pico (or Pico W)
- I2S DAC module (e.g., PCM5102A, MAX98357A)
- MicroSD card module (SPI)
- MicroSD card (FAT32 formatted)
- USB connector for USB host port (optional, for GunFX connection)

## Pin Connections

### USB Host (PIO-USB) - Single Port with Hub Support

**IMPORTANT**: Only ONE USB host port is available!
- PIO0 is used by I2S audio
- PIO1 is used by USB host

To connect multiple GunFX boards, use a **USB hub**.

| Function | Pico Pin | Notes |
|----------|----------|-------|
| USB D+   | GP2      | 22Ω series resistor recommended |
| USB D-   | GP3      | 22Ω series resistor recommended |
| 5V       | VBUS     | For USB device/hub power |
| GND      | GND      | Common ground |

**Note**: CPU must run at 120MHz or 240MHz for USB to work properly.

### I2S Audio (Default - avoid GP2/GP3 for USB)
| Function | Pico Pin |
|----------|----------|
| I2S DIN  | GP6      |
| I2S BCLK | GP7      |
| I2S LRCLK| GP8      |

### SD Card SPI (Default)
| Function | Pico Pin |
|----------|----------|
| SD CS    | GP17     |
| SD SCK   | GP18     |
| SD MOSI  | GP19     |
| SD MISO  | GP16     |

### PCM5102A DAC Wiring

```
PCM5102A       Pico
---------      ----
VCC ---------> 3V3
GND ---------> GND
BCK ---------> GP7 (BCLK)
DIN ---------> GP6 (DATA)
LCK ---------> GP8 (LRCLK)
SCK ---------> GND (use internal clock)
FMT ---------> GND (I2S format)
XSMT --------> 3V3 (unmute)
```

### USB Host Wiring (for GunFX connection)

```
USB Connector   Pico
-------------   ----
VBUS ---------> VBUS (5V out to hub/device)
D+ -----------> GP2 (via 22Ω resistor)
D- -----------> GP3 (via 22Ω resistor)
GND ---------> GND
```

**Hub Setup for Multiple GunFX Boards**:
```
Pico USB Host --> USB Hub --> GunFX Board 1
                         --> GunFX Board 2
                         --> GunFX Board 3
                         --> GunFX Board 4
```

**Important**: The 22Ω series resistors on D+/D- are recommended for signal integrity.

## File Structure

Place these files on the SD card:

```
/
├── config.yaml          # Configuration file
└── sounds/              # Audio files directory
    ├── startup.wav
    ├── engine_start.wav
    ├── engine_idle.wav
    ├── fire.wav
    └── ...
```

## Audio File Requirements

- Format: WAV (uncompressed PCM)
- Sample Rate: 44100 Hz recommended (other rates supported)
- Bit Depth: 16-bit or 8-bit
- Channels: Mono or Stereo

## Building

### Using PlatformIO

```bash
# Install PlatformIO
pip install platformio

# Build
cd controllers/hubfx/pico
pio run

# Upload (with Pico in BOOTSEL mode)
pio run -t upload

# Monitor serial output
pio device monitor
```

### Using Arduino IDE

1. Install Arduino IDE 2.x
2. Add Raspberry Pi Pico board support (earlephilhower core)
3. Install libraries: SdFat, I2S
4. Open `hubfx_pico.ino`
5. Select "Raspberry Pi Pico" board
6. Upload

## Serial Commands

Connect via serial (115200 baud) for interactive control:

| Command | Description |
|---------|-------------|
| `play <ch> <file> [loop] [vol X.X] [left\|right]` | Play audio file |
| `stop <ch\|all>` | Stop channel or all |
| `fade <ch>` | Fade out channel |
| `volume <0.0-1.0>` | Set master volume |
| `status` | Show channel status |
| `usb` | Show USB host status |
| `usend <dev> <msg>` | Send message to USB CDC device |
| `urecv <dev>` | Read data from USB CDC device |
| `engine` | Show engine state |
| `engine start` | Force engine start |
| `engine stop` | Force engine stop |
| `ls` | List SD card files |
| `config` | Show configuration |
| `help` | Show commands |

### Examples

```
play 0 /sounds/engine.wav loop vol 0.5
play 1 /sounds/fire.wav
play 2 /sounds/ambient.wav loop left
stop 0
fade 1
volume 0.8

# USB Host commands
usb
usend 0 FIRE
urecv 0
```

## Module Architecture

```
hubfx_pico.ino          Main application
├── audio_mixer.h/cpp   8-channel audio mixer (Core 1)
├── config_reader.h/cpp YAML configuration parser
├── pwm_input.h/cpp     Generic PWM input monitoring with averaging
├── serial_bus.h/cpp    COBS-framed serial communication over USB
├── usb_host.h/cpp      USB host for GunFX boards (PIO-USB)
├── tusb_config.h       TinyUSB configuration
├── engine_fx.h/cpp     Engine sound effects with state machine
└── gun_fx.h/cpp        Gun FX communication wrapper for slave boards
```

### Common Modules

**pwm_input** - Generic PWM/analog/serial input monitoring:
- Moving average filter for noise reduction  
- Hysteresis for threshold detection
- Abstract input channels (1-10) mapped to GPIO pins (GP10-GP19)
- Rate selection helper for multi-rate triggers

**serial_bus** - COBS-framed serial communication:
- CRC-8 (polynomial 0x07) error detection
- Automatic keepalive management
- Packet callback system for received data

## Engine FX Module

The `engine_fx` module provides engine startup/running/shutdown sound effects with a state machine.

### State Machine

```
STOPPED <--> STARTING --> RUNNING --> STOPPING --> STOPPED
```

- **STOPPED**: Engine off, no sounds playing
- **STARTING**: Startup sound playing, transitions to RUNNING when done
- **RUNNING**: Running sound loops continuously
- **STOPPING**: Shutdown sound playing, transitions to STOPPED when done

### Features

- PWM or analog input monitoring with hysteresis
- Crossfade between startup and running sounds (500ms overlap)
- Configurable transition offsets for seamless audio
- Manual control via serial commands

### Configuration

Configuration uses the same format as the top-level ScaleFX config.yaml:

```yaml
engine_fx:
  type: turbine
  engine_toggle:
    input_channel: 1           # Input channel 1-10
    threshold_us: 1500         # PWM threshold
  sounds:
    starting: "~scalefx/assets/engine_start.wav"
    running: "~scalefx/assets/engine_loop.wav"
    stopping: "~scalefx/assets/engine_stop.wav"
    transitions:
      starting_offset_ms: 60000
      stopping_offset_ms: 25000
```

## Audio Mixer API

```cpp
// Initialize mixer (dual-core mode - recommended)
audio_mixer_init_dual_core(&mixer, &sd);
audio_mixer_start_core1(&mixer);

// Use async API for thread-safe commands from Core 0
AudioPlaybackOptions opts = {
    .loop = true,
    .volume = 0.8f,
    .output = AUDIO_OUTPUT_LEFT,
    .start_offset_ms = 0
};
audio_mixer_play_async(&mixer, 0, "/sounds/engine.wav", &opts);

// Stop with fade (async)
audio_mixer_stop_async(&mixer, 0, AUDIO_STOP_FADE);

// Set volume (async)
audio_mixer_set_volume_async(&mixer, 0, 0.5f);      // Channel 0
audio_mixer_set_volume_async(&mixer, -1, 0.8f);     // Master

// Check status (reads volatile state updated by Core 1)
bool playing = mixer.channel_playing[0];
int remaining_ms = mixer.channel_remaining_ms[0];

// Single-core mode (alternative)
audio_mixer_init(&mixer, &sd);
// Must call frequently in loop():
audio_mixer_process(&mixer);
```

### Thread Safety

In dual-core mode:
- **Core 0**: Use `*_async()` functions to queue commands
- **Core 1**: Processes queue and handles audio
- **Status**: Read from `mixer.channel_playing[]` and `mixer.channel_remaining_ms[]`

## Configuration

Edit `config.yaml` on the SD card:

```yaml
device_name: "My Model"
master_volume: 0.8

audio:
  sample_rate: 44100
  buffer_size: 256

# USB Host for GunFX connection (single port - use hub for multiple devices)
usb_host:
  enabled: true
  dp_pin: 2    # D+ on GP2, D- on GP3

engine_fx:
  enabled: true
  startup_sound:
    path: "/sounds/engine_start.wav"
    volume: 1.0

gun_fx:
  enabled: true
  fire_sound:
    path: "/sounds/fire.wav"
    volume: 1.0
```

## USB Host API

```cpp
// Initialize USB host
UsbHost usb_host;
usb_host.begin();
usb_host.startTask();  // Starts on Core 1

// Set callbacks
usb_host.onMount(on_device_mount);
usb_host.onCdcReceive(on_cdc_receive);

// Send data to connected GunFX board
if (usb_host.cdcConnected()) {
    usb_host.cdcPrintln(0, "FIRE");
}

// Read response
int available = usb_host.cdcAvailable(0);
if (available > 0) {
    uint8_t buf[64];
    int read = usb_host.cdcRead(0, buf, sizeof(buf));
}

// Check status
usb_host.printStatus();
```

## Troubleshooting

### No audio output
- Check I2S wiring
- Verify DAC is powered (3.3V)
- Check XSMT pin is HIGH (unmuted)
- Test with `play 0 /test.wav`

### SD card not detected
- Check SPI wiring
- Verify card is FAT32 formatted
- Try slower SPI speed in code

### Distorted audio
- Reduce master volume
- Check WAV file format (must be PCM)
- Ensure files are 44100 Hz / 16-bit

### Config not loading
- Verify file is named `config.yaml` or `hubfx.yaml`
- Check YAML syntax (spaces, not tabs)
- Use `config` command to see loaded values

### USB device not detected
- Verify CPU clock is 120MHz or 240MHz
- Check USB D+/D- wiring (GP2/GP3)
- Add 22Ω series resistors on D+/D-
- Ensure USB device is powered (VBUS = 5V)
- Try `usb` command to see USB host status

### GunFX not responding
- Check USB cable connection
- Verify GunFX firmware is flashed
- Use `usb` command to see if device is mounted
- Try `usend 0 test` and `urecv 0`

## License

MIT License - See main project LICENSE file.
