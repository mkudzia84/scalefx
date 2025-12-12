# pigpio Configuration for WM8960 Audio HAT

## Overview

The helifx application uses the pigpio library **in-process** (not as a daemon) for GPIO control. When using a WM8960 Audio HAT (or similar I2S audio devices), certain GPIO pins are automatically excluded from pigpio control to prevent conflicts.

**Important:** The `pigpiod` daemon must **not** be running when helifx is active, as they would conflict.

## WM8960 Audio HAT Pin Usage

The WM8960 Audio HAT reserves the following GPIO pins:

### I2C Control (EEPROM and codec configuration)
- **GPIO 2** - I2C SDA (Data)
- **GPIO 3** - I2C SCL (Clock)

### I2S Audio Interface
- **GPIO 18** - I2S BCK (Bit Clock)
- **GPIO 19** - I2S LRCK (Left/Right Clock / Word Select)
- **GPIO 20** - I2S ADCDAT (ADC Data / Input)
- **GPIO 21** - I2S DACDAT (DAC Data / Output)

## Automatic Configuration

The `install.sh` script automatically:

1. Installs pigpio package
2. **Stops and disables** the pigpiod daemon (to prevent conflicts)
3. helifx manages pigpio in-process with automatic pin exclusion

The WM8960 pins are **automatically excluded** in the helifx code (GPIO 2,3,18,19,20,21).

## Running helifx

**Method 1: Direct Execution (Recommended for Testing)**

```bash
# Run with sudo (required for GPIO access)
sudo ./build/helifx config.yaml
```

**Method 2: Systemd Service (Recommended for Production)**

```bash
# Start the helifx service
sudo systemctl start helifx

# Enable auto-start on boot
sudo systemctl enable helifx

# Check status
sudo systemctl status helifx
```

## Ensuring No Conflicts

**If you get "Can't lock /var/run/pigpio.pid" error:**

This means the pigpiod daemon is running. Stop it:

```bash
# Stop the daemon
sudo systemctl stop pigpiod

# Disable it from auto-starting
sudo systemctl disable pigpiod

# Verify it's stopped
sudo systemctl status pigpiod
```

## Verification

Test that helifx has GPIO access:

```bash
# Run helifx
sudo ./build/helifx config.yaml

# You should see:
# [GPIO] GPIO subsystem initialized (pigpio version XX)
# [GPIO] WM8960 Audio HAT pins (2,3,18-21) are reserved for I2C/I2S
```

## Available GPIO Pins

After excluding WM8960 pins, these GPIO pins are available for helifx:

### Input Pins (PWM monitoring)
- GPIO 5 - Engine toggle
- GPIO 6 - Gun trigger
- GPIO 12 - Smoke heater toggle
- GPIO 13 - Pitch servo input
- GPIO 16 - Yaw servo input

### Output Pins (Control signals)
- GPIO 7 - Pitch servo output
- GPIO 8 - Yaw servo output
- GPIO 22 - Nozzle flash LED
- GPIO 23 - Smoke fan MOSFET
- GPIO 24 - Smoke heater MOSFET
- GPIO 25 - Available for future use

### Reserved/Excluded (Do Not Use)
- GPIO 2, 3 - I2C (WM8960)
- GPIO 18, 19, 20, 21 - I2S Audio (WM8960)

## Troubleshooting

### "Can't lock /var/run/pigpio.pid" error
**Symptom:** helifx fails to start with this error
**Cause:** The pigpiod daemon is running
**Solution:** 
```bash
sudo systemctl stop pigpiod
sudo systemctl disable pigpiod
```

### pigpio conflicts with audio
**Symptom:** Audio playback is distorted or fails
**Cause:** WM8960 pins not properly excluded
**Solution:** The exclusion is automatic in helifx code. Make sure you're not manually controlling GPIO 2,3,18-21

### Permission denied errors
**Symptom:** helifx cannot initialize GPIO
**Solution:** 
```bash
# Run with sudo
sudo ./build/helifx config.yaml

# Or use the systemd service
sudo systemctl start helifx
```

### Check for daemon conflicts
```bash
# Verify pigpiod is NOT running
sudo systemctl status pigpiod
# Should show "inactive (dead)"

# Check no other process is using pigpio
ps aux | grep pigpio
# Should only show the grep command
```

## References

- [pigpio Library Documentation](http://abyz.me.uk/rpi/pigpio/)
- [WM8960 Datasheet](https://www.cirrus.com/products/wm8960/)
- [Raspberry Pi GPIO Layout](https://pinout.xyz/)
