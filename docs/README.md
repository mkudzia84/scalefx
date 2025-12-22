# ScaleFX Hub System

Integrated sound and effects system for scale RC models, featuring synchronized engine sounds, gun effects, nozzle flash LED, and smoke generation.

## Features

### Engine FX
- **Realistic engine sounds** with start-up, running loop, and shutdown sequences
- **PWM throttle monitoring** for automatic engine state detection
- **Configurable transition offsets** for seamless audio blending
- **State management** with proper sound overlapping during state changes

### Gun FX
- **Multiple rates of fire** (configurable via YAML)
- **PWM trigger control** with hysteresis to prevent oscillation
- **Synchronized nozzle flash LED** blinking at firing rate
- **Looping gun sounds** for each rate of fire
- **Smoke generator control** (fan-off delay handled by Pico firmware)
- **Independent smoke heater toggle** via PWM input
- **Turret servo control** with smooth motion limits (pitch and yaw axes)

### Audio System
- Multi-channel audio mixer (8 channels)
- Simultaneous playback of engine and gun sounds
- Smooth transitions and crossfading

### Hardware Support
- **Raspberry Pi** (tested on Pi 4)
- **WM8960 Audio HAT** for high-quality audio output
- **GPIO-based PWM monitoring** for RC receiver inputs
- **Gun controller via USB CDC** (Raspberry Pi Pico with custom VID/PID)
- **LED and smoke generator control** managed on Pico; Pi coordinates effects

## Hardware Requirements

- Raspberry Pi (3/4/Zero 2 W recommended)
- WM8960 Audio HAT (or compatible I2S audio interface)
- RC receiver with PWM outputs
- LED for nozzle flash
- Smoke generator (optional)
  - Fan (5V/12V depending on model)
  - Heater element
- MOSFET modules for smoke control (if using 12V components)
- Power supply (5V for Pi, appropriate voltage for smoke generator)

## Quick Start

**After installation:**

1. **Ensure pigpiod daemon is running:**
   ```bash
   sudo systemctl status pigpiod
   sudo systemctl start pigpiod  # Start if not running
   ```

2. **Run sfxhub with sudo:**
   ```bash
   sudo ./build/sfxhub config.yaml
   ```
   
   Or use the systemd service (recommended):
   ```bash
   sudo systemctl start sfxhub
   ```

**Important:** The installation script automatically configures pigpiod with WM8960 Audio HAT pin exclusions. Audio should work without additional configuration.

**USB Gun Controller:** Plug in the Raspberry Pi Pico-based gun controller. It enumerates as a USB CDC device with VID `0x2e8a` (Raspberry Pi) and PID `0x0180` (gunfx). sfxhub auto-detects and opens the device; no serial port configuration is needed in YAML.

See [Troubleshooting](#troubleshooting) for more details.

## Wiring

See [WIRING.md](WIRING.md) for complete wiring diagrams and pin assignments.

**Default GPIO Pin Assignment:**
- **Engine Toggle PWM Input:** GPIO 17
- **Gun Trigger PWM Input:** GPIO 27
- **Smoke Heater Toggle PWM:** GPIO 22
- **Nozzle Flash LED:** GPIO 23
- **Smoke Fan Control:** GPIO 24
- **Smoke Heater Control:** GPIO 25
- **Pitch Servo PWM Input:** GPIO 13
- **Pitch Servo PWM Output:** GPIO 7
- **Yaw Servo PWM Input:** GPIO 16
- **Yaw Servo PWM Output:** GPIO 8

Note: Gun outputs (nozzle flash LED, smoke fan, smoke heater, turret servos) are driven by the Pico firmware. The Pi reads PWM inputs and coordinates sounds and commands over USB; direct GPIO control of these outputs on the Pi is no longer required.

**Reserved Pins (WM8960 Audio HAT):**
- GPIO 2, 3: I2C (SCL, SDA)
- GPIO 17: HAT button (can be reused if button not needed)
- GPIO 18-21: I2S (BCK, LRCK, DIN, DOUT)

## Installation

### Prerequisites

**Compiler Requirements:**
- GCC 14 or later (required for C23 standard support)
- Check your GCC version: `gcc --version`

Install dependencies:
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libyaml-dev \
    libasound2-dev \
    libsndfile1-dev \
    pigpio \
    python3-pigpio
```

**Note:** This project uses the C23 standard (`-std=c23`) and requires a modern compiler with C23 support. If your system GCC version is older than 14, you may need to install a newer version or build from a newer toolchain.

**pigpio Configuration:**

sfxhub uses the **pigpiod daemon** for GPIO control. The installation script automatically:

1. Installs pigpio and pigpiod
2. Configures pigpiod with WM8960 Audio HAT pin exclusions
3. Enables and starts the pigpiod service

The exclusion mask `0x3C000C` prevents pigpiod from controlling GPIO 2,3,18-21 (reserved for I2C/I2S audio).

### Build from Source

**Important:** Install dependencies first (see Prerequisites above) before building.

```bash
cd scalefx
make
```

Build artifacts will be in the `build/` directory.

**Common build errors:**
- `yaml.h: No such file or directory` → Install `libyaml-dev`: `sudo apt-get install libyaml-dev`
- `pigpio.h: No such file or directory` → Install `pigpio`: `sudo apt-get install pigpio`
- GPIO access errors → Run with `sudo` or add user to `gpio` group

### Automated Installation

Use the installation script for complete system setup:

```bash
# Make executable
chmod +x scripts/install.sh

# Run installation for default user (pi)
sudo ./scripts/install.sh

# Or specify a different user
sudo ./scripts/install.sh username
```

The script will:
- Install all dependencies
- Copy binaries to `/home/<user>/scalefx`
- Install systemd service with correct user
- Optionally enable auto-start on boot
- Create directory structure for assets and logs

### Manual Installation

**Note:** The automated installation script is recommended. For manual installation, replace `pi` with your username if different.

```bash
# Create directory structure
sudo mkdir -p /home/pi/scalefx/assets
sudo mkdir -p /home/pi/scalefx/logs

# Copy files (from build directory)
sudo cp build/sfxhub /home/pi/scalefx/
sudo cp config.yaml /home/pi/scalefx/

# Copy your sound files
sudo cp your_sounds/* /home/pi/scalefx/assets/

# Set ownership
sudo chown -R pi:pi /home/pi/scalefx

# Install systemd service (update User, Group, and paths in service file if needed)
sudo cp scripts/sfxhub.service /etc/systemd/system/
sudo systemctl daemon-reload

# Enable and start service
sudo systemctl enable sfxhub
sudo systemctl start sfxhub
```

### Uninstallation

To remove the ScaleFX Hub system:

```bash
# Uninstall for default user (pi)
sudo ./scripts/uninstall.sh

# Or specify a different user
sudo ./scripts/uninstall.sh username
```

The script will:
- Stop and disable the service
- Remove service file
- Optionally remove installation directory and all files

## Configuration

Edit `/home/pi/scalefx/config.yaml` to customize:

**Important:** Sections are optional. If a section is omitted from the config file, that subsystem is disabled. There is no explicit `enabled: true/false` flag - presence or absence of the section determines if the feature is active.

### Engine FX Configuration

```yaml
# Engine FX - omit entire section to disable engine sounds
engine_fx:
  # Engine type: turbine, radial, diesel (future)
  type: turbine
  
  # Engine Toggle PWM Input
  engine_toggle:
    input_channel: 1           # Input channel 1-12 (mapped to GPIO pins on PCB)
    threshold_us: 1500         # PWM threshold in microseconds (engine on/off)
  
  # Sound Files and Transitions
  sounds:
    starting: "~scalefx/assets/engine_start.wav"    # Engine start-up sound (optional)
    running: "~scalefx/assets/engine_loop.wav"      # Engine running loop sound (optional)
    stopping: "~scalefx/assets/engine_stop.wav"     # Engine shut-down sound (optional)
    
    # Transition Offsets (for seamless audio blending)
    transitions:
      starting_offset_ms: 60000    # Offset when restarting from stopping state
      stopping_offset_ms: 25000    # Offset when stopping from starting state
```

### Gun FX Configuration

```yaml
# Gun FX - omit entire section to disable gun effects
gun_fx:
  # Trigger PWM Input
  trigger:
    input_channel: 2           # Input channel 1-12 (mapped to GPIO pins on PCB)
  
  # Smoke Generator (optional - omit to disable smoke)
  smoke:
    heater_toggle_channel: 3   # Input channel 1-12 for heater toggle
    heater_pwm_threshold_us: 1500  # PWM threshold for heater toggle
    fan_off_delay_ms: 2000     # Delay before turning smoke fan off after firing stops
  
  # Rates of Fire (optional - omit for no gun sounds)
  rates_of_fire:
    - name: "200"
      rpm: 200                 # Rounds per minute
      pwm_threshold_us: 1300   # PWM threshold in microseconds
      sound_file: "~scalefx/assets/gun_200rpm.wav"
    
    - name: "550"
      rpm: 550                 # Rounds per minute
      pwm_threshold_us: 1600   # PWM threshold in microseconds
      sound_file: "~scalefx/assets/gun_550rpm.wav"
  
  # Turret Control Servos (optional - omit to disable turret control)
  turret_control:
    pitch:
      servo_id: 1              # Pico servo ID (1, 2, or 3)
      input_channel: 4         # Input channel 1-12 (mapped to GPIO pins on PCB)
      input_min_us: 1000       # Minimum input PWM pulse width (µs)
      input_max_us: 2000       # Maximum input PWM pulse width (µs)
      output_min_us: 1000      # Minimum output PWM pulse width (µs)
      output_max_us: 2000      # Maximum output PWM pulse width (µs)
      max_speed_us_per_sec: 4000      # Maximum speed (µs/second)
      max_accel_us_per_sec2: 8000     # Maximum acceleration (µs/second²)
      max_decel_us_per_sec2: 8000     # Maximum deceleration (µs/second²)
    
    yaw:
      servo_id: 2              # Pico servo ID (1, 2, or 3)
      input_channel: 5         # Input channel 1-12 (mapped to GPIO pins on PCB)
      input_min_us: 1000
      input_max_us: 2000
      output_min_us: 1000
      output_max_us: 2000
      max_speed_us_per_sec: 4000
      max_accel_us_per_sec2: 8000
      max_decel_us_per_sec2: 8000
```

### PWM Calibration

PWM thresholds are in microseconds (typical RC PWM range: 1000-2000µs):
- **Engine threshold:** PWM value above which engine is considered "on"
- **Gun rate thresholds:** PWM values for each firing rate (ordered lowest to highest)
- **Smoke heater threshold:** PWM value to enable smoke heater

Use a servo tester or RC receiver to measure your actual PWM values and adjust accordingly.

### Optional Sections

All configuration sections are optional. **Presence or absence of a section determines if the feature is enabled:**

| Section | Effect when omitted |
|---------|-------------------|
| `engine_fx` | Engine sounds disabled |
| `gun_fx` | All gun effects disabled |
| `gun_fx.smoke` | Smoke generator disabled |
| `gun_fx.turret_control` | Turret servos disabled |
| `gun_fx.rates_of_fire` | Gun sounds disabled (trigger still detected) |

**Example: Engine sounds only (no gun effects)**
```yaml
engine_fx:
  type: turbine
  engine_toggle:
    input_channel: 1
    threshold_us: 1500
  sounds:
    starting: "~scalefx/assets/engine_start.wav"
    running: "~scalefx/assets/engine_loop.wav"
    stopping: "~scalefx/assets/engine_stop.wav"
# gun_fx section omitted - no gun effects
```

### USB Auto-Detection

The gun controller is auto-detected by VID/PID and opened over USB CDC. No serial port configuration is required in YAML.

### Sound Files

Place your sound files in `/home/pi/scalefx/assets/` directory:
- Engine sounds should be WAV format, 44.1kHz recommended
- Gun sounds should loop seamlessly
- Paths support `~` expansion for home directory

## Usage

### Systemd Service (Recommended)

```bash
# Start service
sudo systemctl start sfxhub

# Stop service
sudo systemctl stop sfxhub

# Restart service
sudo systemctl restart sfxhub

# View status
sudo systemctl status sfxhub

# View live logs
sudo journalctl -u sfxhub -f

# Enable auto-start on boot
sudo systemctl enable sfxhub

# Disable auto-start
sudo systemctl disable sfxhub
```

### Manual Execution

```bash
cd /home/pi/scalefx
./sfxhub config.yaml
```

Or from the build directory during development:
```bash
cd scalefx
./build/sfxhub config.yaml
```

Press `Ctrl+C` to stop.

### Monitoring

Check system status every 10 seconds (logged to journal). Example output:
```
[GUN STATUS @ 10.2s] Firing: YES | Rate: 2 | RPM: 550 | Trigger PWM: 1650 µs | Heater: ON
[GUN SERVOS] Pitch: 1500 µs | Yaw: 1600 µs | Pitch Servo: ACTIVE | Yaw Servo: ACTIVE
```

The status dump includes:
- **Gun Status:** Firing state, selected rate, RPM, trigger PWM width, heater status
- **Servo Status:** Pitch/yaw input PWM readings and servo active/disabled state

View logs:
```bash
# Live logs
sudo journalctl -u sfxhub -f

# Last 100 lines
sudo journalctl -u sfxhub -n 100

# Today's logs
sudo journalctl -u sfxhub --since today
```

## Troubleshooting

### No Audio Output

1. Check WM8960 Audio HAT installation:
   ```bash
   aplay -l  # List audio devices
   ```

2. Test speaker output:
   ```bash
   speaker-test -c 2 -t wav
   ```

3. Verify ALSA configuration:
   ```bash
   amixer  # Check volume levels
   ```

### GPIO Access Denied / "Can't lock /var/run/pigpio.pid"### GPIO Access Denied / "Failed to connect to pigpiod daemon"

**This error means the pigpiod daemon is not running.**

**Solution - Start the pigpiod daemon:**
```bash
# Start pigpiod daemon
sudo systemctl start pigpiod

# Enable it to start on boot
sudo systemctl enable pigpiod

# Verify it's running
sudo systemctl status pigpiod
```

The installation script should have automatically configured pigpiod with WM8960 pin exclusions. If you need to verify:

```bash
cat /etc/systemd/system/pigpiod.service.d/override.conf
# Should show: ExecStart=/usr/bin/pigpiod -l -x 0x3C000C
```

**Then run sfxhub with sudo:**
```bash
sudo ./sfxhub config.yaml
```

Or use the systemd service (recommended):
```bash
sudo systemctl start sfxhub
```

### PWM Not Detected

1. Check RC receiver wiring
2. Verify PWM signal with oscilloscope or logic analyzer
3. Check GPIO pin assignments in config.yaml
4. Monitor live PWM values in status output

### LED Not Working

1. Check GPIO pin assignment
2. Verify LED wiring (anode to GPIO via resistor, cathode to ground)
3. Test GPIO manually:
   ```bash
   gpio mode 23 out
   gpio write 23 1  # LED on
   gpio write 23 0  # LED off
   ```

### Smoke Generator Issues

1. Ensure MOSFET modules are correctly wired
2. Check power supply voltage and current capacity
3. Verify heater resistance (should match power supply)
4. Monitor fan and heater pin states in logs

### Service Won't Start

Check logs for errors:
```bash
sudo journalctl -u sfxhub -n 50
```

Common issues:
- Missing sound files (check paths in config.yaml)
- Invalid GPIO pin numbers
- Configuration syntax errors (validate YAML)
- Insufficient permissions

## Development

### Building

```bash
make             # Build sfxhub
make clean       # Clean build artifacts
make help        # Show all available targets
```

### Project Structure

```
scalefx/
├── src/                   # Source files (.c)
│   ├── main.c             # Main application entry point
│   ├── config_loader.c    # YAML configuration parser
│   ├── engine_fx.c        # Engine sound effects
│   ├── gun_fx.c           # Gun effects controller
│   ├── audio_player.c     # Audio mixer and playback
│   ├── lights.c           # LED control
│   ├── smoke_generator.c  # Smoke generator control
│   └── gpio.c             # GPIO abstraction layer
├── include/               # Header files (.h)
│   ├── engine_fx.h
│   ├── gun_fx.h
│   ├── audio_player.h
│   ├── lights.h
│   ├── smoke_generator.h
│   ├── gpio.h
│   ├── config_loader.h
│   ├── miniaudio.h
│   └── pwm_monitor.h
├── build/                 # Build output (generated)
│   ├── sfxhub             # Main binary
│   └── *.o                # Object files
├── scripts/               # Installation and service scripts
│   ├── sfxhub.service     # Systemd service unit
│   ├── install.sh         # Installation script
│   └── uninstall.sh       # Uninstallation script
├── docs/                  # Documentation
│   ├── README.md          # This file
│   ├── WIRING.md          # Complete wiring guide
│   └── GUN_FX_WIRING.md   # Gun FX specific wiring
├── config.yaml            # Configuration file
├── Makefile               # Build system
└── .gitignore             # Git ignore rules
```

### Testing Individual Components

Demo programs available:
- `engine_fx_demo` - Test engine sounds
- `gun_fx_demo` - Test gun effects

## Uninstallation

```bash
chmod +x scripts/uninstall.sh
sudo ./scripts/uninstall.sh
```

This will:
- Stop and disable the service
- Remove systemd service file
- Optionally remove installation directory

## Credits

- Audio mixing based on libsndfile
- GPIO control using pigpio library
- YAML parsing with libyaml

## Documentation

Additional documentation available:
- [C23 Migration Guide](C23_MIGRATION.md) - C23 standard migration details and compiler requirements
- [Wiring Guide](WIRING.md) - Complete wiring diagrams and hardware setup
- [Logging System](LOGGING.md) - Logging architecture and usage guide

## License

[Your License Here]

## Support

For issues, questions, or contributions, please open an issue on the project repository.

## Technical Specifications

- **Programming Language:** C23 standard (ISO/IEC 9899:2023)
- **Threading:** C23 standard threads (`<threads.h>`)
- **Compiler:** GCC 14 or later required
- **Audio:** 44.1kHz, 16-bit stereo WAV files
- **PWM Input:** 50Hz (20ms period), 1000-2000µs pulse width
- **GPIO:** 3.3V logic levels
- **LED Current:** 20mA max per GPIO pin
- **Smoke Generator:** External power supply required (not from Pi GPIO)

## Safety Notes

- **Never exceed 3.3V on any GPIO pin** - Raspberry Pi is not 5V tolerant
- **Use current-limiting resistors** for LEDs (typically 220-330Ω)
- **Use MOSFET modules** for controlling smoke generator (high current loads)
- **Ensure proper heat dissipation** for smoke generator heater element
- **Test smoke generator outdoors** or in well-ventilated area
- **Never leave smoke generator unattended** when powered

## Performance

- **Latency:** <50ms from PWM trigger to audio/LED response
- **CPU Usage:** ~5-10% on Raspberry Pi 4
- **Memory:** ~50MB RAM
- **Audio Quality:** Lossless playback, no compression
