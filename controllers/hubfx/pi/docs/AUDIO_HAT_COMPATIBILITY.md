# Audio HAT Compatibility Guide

Complete reference for audio HATs supported by ScaleFX Hub system.

## Table of Contents
- [Supported Audio HATs](#supported-audio-hats)
- [GPIO Pin Reservations](#gpio-pin-reservations)
- [Pin Protection Mechanism](#pin-protection-mechanism)
- [Hardware Specifications](#hardware-specifications)
- [Installation Notes](#installation-notes)
- [Switching Between HATs](#switching-between-hats)

## Supported Audio HATs

ScaleFX Hub is compatible with the following Raspberry Pi audio HATs:

### 1. WM8960 Audio HAT
- **Manufacturer:** Waveshare / Generic
- **Codec:** WM8960 Stereo Audio Codec
- **Features:**
  - Stereo playback and recording
  - Built-in microphone input
  - Headphone output with amplifier
  - Line-out capability
  - I2C configuration interface
  - 24-bit audio, up to 48kHz sample rate

### 2. Raspberry Pi DigiAMP+
- **Manufacturer:** IQaudio (now Raspberry Pi branded)
- **Codec:** Texas Instruments TAS5756M
- **Features:**
  - 35W + 35W stereo amplifier (Class-D)
  - Direct speaker output (no external amplifier needed)
  - High-quality audio playback
  - Programmable shutdown control
  - Automatic muting/unmuting
  - Thermal and short-circuit protection

## GPIO Pin Reservations

Both audio HATs use overlapping GPIO pins for audio communication. The software automatically protects these pins.

### Common Pins (Both HATs)

| GPIO Pin | Function | Protocol | Physical Pin | Notes |
|----------|----------|----------|--------------|-------|
| 2 | I2C SDA | I2C | 3 | Codec configuration and control |
| 3 | I2C SCL | I2C | 5 | Codec configuration and control |
| 18 | I2S BCK | I2S | 12 | Bit clock (serial clock) |
| 19 | I2S LRCK | I2S | 35 | Word select (left/right clock) |
| 20 | I2S DIN | I2S | 38 | Audio data to codec (ADC input) |
| 21 | I2S DOUT | I2S | 40 | Audio data from codec (DAC output) |

### DigiAMP+ Additional Pin

| GPIO Pin | Function | Protocol | Physical Pin | Notes |
|----------|----------|----------|--------------|-------|
| 22 | Shutdown | Digital | 15 | Amplifier shutdown control (active low) |

**Total Protected Pins:** GPIO 2, 3, 18, 19, 20, 21, 22

## Pin Protection Mechanism

The ScaleFX Hub software includes automatic protection for audio HAT pins:

### Software Protection

File: `src/gpio.c`

```c
// Audio HAT Reserved Pins - DO NOT USE
#define AUDIO_I2C_SDA     2   // I2C Data
#define AUDIO_I2C_SCL     3   // I2C Clock
#define AUDIO_I2S_BCK     18  // I2S Bit Clock
#define AUDIO_I2S_LRCK    19  // I2S Left/Right Clock
#define AUDIO_I2S_DIN     20  // I2S Data In
#define AUDIO_I2S_DOUT    21  // I2S Data Out
#define AUDIO_SHUTDOWN    22  // DigiAMP+ shutdown control

static bool is_audio_hat_pin(int pin) {
    return (pin == AUDIO_I2C_SDA || pin == AUDIO_I2C_SCL ||
            pin == AUDIO_I2S_BCK || pin == AUDIO_I2S_LRCK ||
            pin == AUDIO_I2S_DIN || pin == AUDIO_I2S_DOUT ||
            pin == AUDIO_SHUTDOWN);
}
```

### Protection Features

1. **Automatic Detection:** Any attempt to configure protected pins generates an error
2. **Clear Error Messages:** User receives specific error about audio HAT conflict
3. **No Configuration Required:** Protection works automatically for both HATs
4. **Safe by Default:** Cannot accidentally override audio pins

### Example Error Output

```
[GPIO] ERROR: Cannot use GPIO 22 - reserved for audio HAT (WM8960/DigiAMP+)!
```

## Hardware Specifications

### WM8960 Audio HAT

**Audio Specifications:**
- Sample Rate: 8kHz - 48kHz
- Bit Depth: 16-bit, 24-bit, 32-bit
- SNR: >98dB (typical)
- THD+N: <0.01% (typical)
- Headphone Output: 40mW per channel
- Input Impedance: 10kΩ (typical)

**Power Requirements:**
- Supply Voltage: 5V (from Pi header)
- Power Consumption: ~150mA (typical)
- Standby Current: ~50mA

**Physical:**
- Dimensions: 65mm × 56mm
- Mounting: 40-pin GPIO header
- Height: ~15mm above Pi

### Raspberry Pi DigiAMP+

**Audio Specifications:**
- Sample Rate: 44.1kHz, 48kHz, 88.2kHz, 96kHz
- Bit Depth: 16-bit, 24-bit, 32-bit
- Output Power: 35W + 35W @ 4Ω (max)
- SNR: >100dB
- THD+N: <0.003%
- Frequency Response: 20Hz - 20kHz (±0.5dB)

**Power Requirements:**
- Supply Voltage: 12-24V DC (NOT from Pi!)
- External power input required
- Efficient Class-D operation
- Thermal protection enabled

**Physical:**
- Dimensions: 65mm × 56mm
- Mounting: 40-pin GPIO header
- Height: ~15mm above Pi
- Speaker terminals: Screw terminals for up to 16AWG wire

**Important:** DigiAMP+ requires external power supply. Do NOT attempt to power from Raspberry Pi.

## Installation Notes

### WM8960 Audio HAT Setup

1. **Physical Installation:**
   - Power off Raspberry Pi
   - Align HAT with 40-pin GPIO header
   - Press firmly to seat connector
   - Secure with standoffs (recommended)

2. **Software Configuration:**
   ```bash
   # Install drivers (if needed)
   sudo apt-get update
   sudo apt-get install -y i2c-tools
   
   # Enable I2C and I2S in /boot/config.txt
   sudo raspi-config
   # Interface Options → I2C → Enable
   
   # Add to /boot/config.txt:
   dtoverlay=wm8960-soundcard
   
   # Reboot
   sudo reboot
   ```

3. **Test Audio:**
   ```bash
   # List audio devices
   aplay -l
   
   # Test playback
   speaker-test -c 2 -t wav
   ```

### DigiAMP+ Setup

1. **Physical Installation:**
   - Power off Raspberry Pi
   - Connect external power supply (12-24V) to DigiAMP+ screw terminals
   - Align HAT with 40-pin GPIO header
   - Press firmly to seat connector
   - Connect speakers to screw terminals (observe polarity)

2. **Software Configuration:**
   ```bash
   # Add to /boot/config.txt:
   dtoverlay=iqaudio-dacplus
   
   # Reboot
   sudo reboot
   ```

3. **Test Audio:**
   ```bash
   # List audio devices
   aplay -l
   
   # Test playback (START QUIET - amplifier is powerful!)
   speaker-test -c 2 -t wav -l 1
   ```

### ScaleFX Hub Configuration

No special configuration needed! The software automatically:
- Detects and protects audio HAT pins
- Uses ALSA for audio output
- Works with either HAT transparently

## Switching Between HATs

To switch from one audio HAT to another:

1. **Power Down:**
   ```bash
   sudo shutdown -h now
   # Wait for Pi to fully power off
   ```

2. **Physical Swap:**
   - Disconnect power
   - Remove old HAT
   - Install new HAT
   - Reconnect power (and external power for DigiAMP+)

3. **Update Configuration:**
   ```bash
   sudo nano /boot/config.txt
   
   # For WM8960:
   dtoverlay=wm8960-soundcard
   
   # For DigiAMP+:
   dtoverlay=iqaudio-dacplus
   
   # Save and reboot
   sudo reboot
   ```

4. **Verify Operation:**
   ```bash
   # Check audio device
   aplay -l
   
   # Test audio output
   speaker-test -c 2 -t wav -l 1
   
   # Run ScaleFX Hub
   sudo ./sfxhub config.yaml
   ```

## Troubleshooting

### No Audio Output

**WM8960:**
- Check I2C connection: `sudo i2cdetect -y 1` (should show device at 0x1a)
- Verify dtoverlay in `/boot/config.txt`
- Check volume: `alsamixer` (press F6 to select card)
- Ensure headphones/speakers connected

**DigiAMP+:**
- Verify external power supply connected (12-24V)
- Check speaker connections (correct polarity)
- Confirm dtoverlay in `/boot/config.txt`
- Check for thermal shutdown (let cool down)

### GPIO Pin Conflicts

If you see errors like:
```
[GPIO] ERROR: Cannot use GPIO 22 - reserved for audio HAT!
```

**Solution:**
- Check your `config.yaml` pin assignments
- Ensure no GPIO pins 2, 3, 18-22 are used for other functions

### I2C/I2S Errors

**Symptoms:**
- Codec not detected
- No audio initialization
- Crackling or distorted audio

**Solutions:**
1. Verify HAT is firmly seated on GPIO header
2. Check `/boot/config.txt` has correct dtoverlay
3. Ensure I2C is enabled: `sudo raspi-config`
4. Check for bent pins on GPIO header
5. Try reseating the HAT

## Additional Resources

- **WM8960 Datasheet:** [Cirrus Logic WM8960](https://www.cirrus.com/products/wm8960/)
- **DigiAMP+ Documentation:** [Raspberry Pi Official](https://www.raspberrypi.com/products/iqaudio-digiamp/)
- **Raspberry Pi Pinout:** [pinout.xyz](https://pinout.xyz/)

## Summary

Both WM8960 Audio HAT and Raspberry Pi DigiAMP+ are fully supported by ScaleFX Hub:

| Feature | WM8960 | DigiAMP+ |
|---------|--------|----------|
| Automatic pin protection | ✓ | ✓ |
| No code changes required | ✓ | ✓ |
| Stereo output | ✓ | ✓ |
| Built-in amplifier | Headphone only | 35W + 35W |
| External power required | ✗ | ✓ (12-24V) |
| Microphone input | ✓ | ✗ |
| Protected GPIO pins | 2,3,18-21 | 2,3,18-22 |

**Recommendation:** 
- **WM8960:** Best for headphone/line-out, includes microphone input
- **DigiAMP+:** Best for direct speaker connection with powerful amplification
