# pigpio Configuration for WM8960 Audio HAT

## Overview

The helifx application uses the pigpio library for GPIO control. When using a WM8960 Audio HAT (or similar I2S audio devices), certain GPIO pins must be excluded from pigpio control to prevent conflicts.

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

The `install.sh` script automatically configures pigpio to exclude these pins. It:

1. Installs pigpio package
2. Creates a systemd service override with pin exclusion
3. Enables and starts the pigpiod daemon

The exclusion mask `0x3C000C` represents:
- Bit 2 (GPIO 2)
- Bit 3 (GPIO 3)
- Bit 18 (GPIO 18)
- Bit 19 (GPIO 19)
- Bit 20 (GPIO 20)
- Bit 21 (GPIO 21)

## Manual Configuration

If you need to configure pigpio manually:

### Method 1: Systemd Service Override (Recommended)

```bash
# Create override directory
sudo mkdir -p /etc/systemd/system/pigpiod.service.d

# Create override configuration
sudo tee /etc/systemd/system/pigpiod.service.d/override.conf << EOF
[Service]
ExecStart=
ExecStart=/usr/bin/pigpiod -l -x 0x3C000C
EOF

# Reload and restart
sudo systemctl daemon-reload
sudo systemctl enable pigpiod
sudo systemctl restart pigpiod
```

### Method 2: Command Line

```bash
# Stop pigpiod if running
sudo systemctl stop pigpiod

# Start with exclusions
sudo pigpiod -x 0x3C000C
```

### Method 3: Configuration File

Create `/etc/pigpio.conf`:
```
# Exclude WM8960 Audio HAT pins
-x 0x3C000C
```

Then enable the service:
```bash
sudo systemctl enable pigpiod
sudo systemctl start pigpiod
```

## Verification

Check that pigpio is running with correct exclusions:

```bash
# Check service status
sudo systemctl status pigpiod

# Verify pigpio version and options
pigs hwver

# Test that excluded pins are protected
# This should fail with "GPIO not 0-31" error for excluded pins
pigs m 2 w  # Should fail - GPIO 2 excluded
pigs m 5 w  # Should succeed - GPIO 5 available
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

### pigpio conflicts with audio
**Symptom:** Audio playback is distorted or fails
**Solution:** Ensure pigpio exclusions are set correctly and pigpiod is restarted

### "GPIO not 0-31" errors
**Symptom:** pigpio returns errors when trying to use certain pins
**Solution:** This is expected for excluded pins. Check your pin assignments in `config.yaml`

### Permission denied errors
**Symptom:** helifx cannot access GPIO
**Solution:** 
```bash
# Check if pigpiod is running
sudo systemctl status pigpiod

# Add user to gpio group (if needed)
sudo usermod -a -G gpio $USER

# Reboot to apply group changes
sudo reboot
```

### Check current pigpio configuration
```bash
# View systemd override
cat /etc/systemd/system/pigpiod.service.d/override.conf

# Check running process
ps aux | grep pigpiod
```

## References

- [pigpio Library Documentation](http://abyz.me.uk/rpi/pigpio/)
- [WM8960 Datasheet](https://www.cirrus.com/products/wm8960/)
- [Raspberry Pi GPIO Layout](https://pinout.xyz/)
