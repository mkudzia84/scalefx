# pigpio Configuration for WM8960 Audio HAT

## Overview

The helifx application uses the **pigpiod daemon** for GPIO control. The daemon must be configured to exclude WM8960 Audio HAT pins (GPIO 2,3,18-21) to allow the kernel I2C and I2S drivers to use those pins for audio.

**Important:** The installation script automatically handles this configuration.

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

1. Installs pigpio and pigpiod daemon
2. Creates a systemd service override with pin exclusion mask `0x3C000C`
3. Enables and starts the pigpiod daemon

No manual configuration is needed.

## Running helifx

Once pigpiod is running with the correct configuration:

**Direct execution:**
```bash
sudo ./build/helifx config.yaml
```

**Using systemd service (recommended for production):**
```bash
sudo systemctl start helifx
```

**Verify pigpiod is running:**
```bash
sudo systemctl status pigpiod
```

```bash
# Start the helifx service
sudo systemctl start helifx

# Enable auto-start on boot
sudo systemctl enable helifx

# Check status
sudo systemctl status helifx
```

## Troubleshooting

### "Failed to connect to pigpiod daemon"

**Cause:** The pigpiod daemon is not running.

**Solution:**
```bash
# Start the daemon
sudo systemctl start pigpiod

# Enable auto-start on boot
sudo systemctl enable pigpiod

# Verify it's running
sudo systemctl status pigpiod
```

### "Unable to create IPC semaphore" or audio not working

**Cause:** pigpiod is interfering with audio HAT pins.

**Solution:** Verify the daemon is configured with pin exclusions:

```bash
# Check the override configuration
cat /etc/systemd/system/pigpiod.service.d/override.conf

# Should show:
# [Service]
# ExecStart=
# ExecStart=/usr/bin/pigpiod -l -x 0x3C000C

# If not configured correctly, run the installation script again
sudo ./scripts/install.sh
```

## Verification

Test that pigpiod is running correctly:

```bash
# Check daemon status
sudo systemctl status pigpiod

# Verify pin exclusions are active
# These commands should fail (pins excluded):
pigs m 18 w  # Should fail - GPIO 18 is I2S BCK (excluded)
pigs m 19 w  # Should fail - GPIO 19 is I2S LRCK (excluded)

# These should succeed (pins available):
pigs m 5 w   # Should succeed - GPIO 5 is Engine Toggle
pigs m 6 w   # Should succeed - GPIO 6 is Gun Trigger

# Check helifx can connect
sudo ./build/helifx config.yaml
# Should show: "Connected to pigpiod daemon"
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
