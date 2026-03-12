# HubFX Hardware Wiring Guide

## Components

- **Raspberry Pi Pico** (RP2040 microcontroller)
- **Waveshare WM8960 Audio HAT** (or compatible I2S audio codec)
- **SD Card Module** (SPI interface)

---

## Visual Wiring Diagram

### WM8960 Audio HAT 40-Pin Header Layout

```
    WM8960 HAT (Top View)
    ═════════════════════════════════════════
    
    3.3V  ⚡ [●] 1     2 [●] ⚡ 5V (optional)
    SDA   🔧 [●] 3     4 [●]
    SCL   🔧 [●] 5     6 [●] ⚡ GND
             [●] 7     8 [●]
             [●] 9    10 [●]
             [●] 11   12 [●] 🎵 I2S CLK (GPIO18)
             [●] 13   14 [●]
             [●] 15   16 [●]
             [●] 17   18 [●]
             [●] 19   20 [●]
             [●] 21   22 [●]
             [●] 23   24 [●]
             [●] 25   26 [●]
             [●] 27   28 [●]
             [●] 29   30 [●]
             [●] 31   32 [●]
             [●] 33   34 [●]
    I2S LRCLK 🎵 [●] 35   36 [●]
             [●] 37   38 [●] 🎵 I2S ADC (GPIO20) ⚠️ PLAYBACK
             [●] 39   40 [●] I2S DAC (GPIO21) ✗ NOT USED
    
    ═════════════════════════════════════════

Legend:
⚡ Power          🔧 I2C Control       🎵 I2S Audio
⚠️ Connect here   ✗ Do NOT connect
```

### Pico to WM8960 Connections

| Pico Pin | Pico GPIO | Signal | → | WM8960 Pin | WM8960 GPIO | Function |
|----------|-----------|--------|---|------------|-------------|----------|
| Pin 36 | 3V3 OUT | Power | → | **Pin 1** | 3.3V | ⚡ Power Supply |
| Pin 39 | VSYS | Power | → | **Pin 2** | 5V | ⚡ Amplifier (optional) |
| Pin 6 | GP4 | I2C | → | **Pin 3** | GPIO2 | 🔧 I2C SDA |
| Pin 7 | GP5 | I2C | → | **Pin 5** | GPIO3 | 🔧 I2C SCL |
| Pin 38 | GND | Ground | → | **Pin 6** | GND | ⚡ Ground |
| Pin 10 | GP7 | I2S | → | **Pin 12** | GPIO18 | 🎵 Bit Clock |
| Pin 11 | GP8 | I2S | → | **Pin 35** | GPIO19 | 🎵 LR Clock |
| Pin 9 | GP6 | I2S | → | **Pin 38** | GPIO20 | 🎵 ADC Data ⚠️ |

**Total: 8 wires** (3 power + 2 I2C + 3 I2S)

---

## Raspberry Pi Pico → WM8960 Audio HAT

The WM8960 Audio HAT requires both **I2S** (for audio streaming) and **I2C** (for codec configuration).

### I2S Audio Connections

| Pico Pin | GPIO | Function | WM8960 Pin | Notes |
|----------|------|----------|------------|-------|
| Pin 9 | GP6 | I2S DATA (DOUT) | **ADC (GPIO20/Pin 38)** | ⚠️ Audio data from Pico to WM8960 (playback) |
| Pin 10 | GP7 | I2S BCLK | CLK (GPIO18/Pin 12) | Bit clock (Pico is I2S master) |
| Pin 11 | GP8 | I2S LRCLK | LRCLK (GPIO19/Pin 35) | Left/Right clock (frame sync) |

⚠️ **IMPORTANT:** For playback, connect to **ADC (Pin 38)**, NOT DAC (Pin 40).
- **ADC (Pin 38/GPIO20)** = Input TO WM8960 (for playback) ✓
- **DAC (Pin 40/GPIO21)** = Output FROM WM8960 (for recording) ✗

### I2C Control Connections

| Pico Pin | GPIO | Function | WM8960 Pin | Notes |
|----------|------|----------|------------|-------|
| Pin 6 | GP4 | I2C0 SDA | SDA (GPIO2) | Codec register configuration |
| Pin 7 | GP5 | I2C0 SCL | SCL (GPIO3) | I2C clock |

### Power Connections

| Pico Pin | WM8960 Pin | Notes |
|----------|------------|-------|
| 3V3 (Pin 36) | 3.3V | Logic power for WM8960 |
| GND | GND | Common ground |
| VSYS (Pin 39) | 5V | *(Optional)* Power for speaker amplifiers (1W per channel) |

**Note:** The WM8960 HAT can run on 3.3V for headphone output only. For speaker output (Class D amplifier), 5V is recommended.

---

## Raspberry Pi Pico → SD Card Module

Standard SPI connection on SPI0 bus.

| Pico Pin | GPIO | Function | SD Card Pin | Notes |
|----------|------|----------|-------------|-------|
| Pin 21 | GP16 | SPI0 MISO | MISO / DO | Data from SD card |
| Pin 22 | GP17 | SPI0 CS | CS | Chip select |
| Pin 24 | GP18 | SPI0 SCK | SCK / CLK | SPI clock (up to 25 MHz) |
| Pin 25 | GP19 | SPI0 MOSI | MOSI / DI | Data to SD card |
| 3V3 | - | Power | VCC | 3.3V power |
| GND | - | Ground | GND | Common ground |

**SD Card Format:** FAT32 recommended for best compatibility.

---

## Pin Summary Table

### WM8960 / TAS5825M Configuration

| GPIO | Pico Pin | Function | Connected To |
|------|----------|----------|--------------|
| GP4 | 6 | I2C0 SDA | WM8960 SDA |
| GP5 | 7 | I2C0 SCL | WM8960 SCL |
| GP6 | 9 | I2S DATA | WM8960 DAC |
| GP7 | 10 | I2S BCLK | WM8960 CLK |
| GP8 | 11 | I2S LRCLK | WM8960 LRCLK |
| GP16 | 21 | SPI0 MISO | SD Card MISO |
| GP17 | 22 | SPI0 CS | SD Card CS |
| GP18 | 24 | SPI0 SCK | SD Card SCK |
| GP19 | 25 | SPI0 MOSI | SD Card MOSI |
| GP25 | - | Built-in LED | Status indicator |

### Pimoroni Pico Audio Pack Configuration

| GPIO | Pico Pin | Function | Connected To |
|------|----------|----------|--------------|
| GP9 | 12 | I2S DATA (DIN) | PCM5100A DIN |
| GP10 | 14 | I2S BCLK | PCM5100A BCK |
| GP11 | 15 | I2S LRCLK | PCM5100A LRCK |
| GP16 | 21 | SPI0 MISO | SD Card MISO |
| GP17 | 22 | SPI0 CS | SD Card CS |
| GP18 | 24 | SPI0 SCK | SD Card SCK |
| GP19 | 25 | SPI0 MOSI | SD Card MOSI |
| GP22 | 29 | Mute (XSMT) | PCM5100A XSMT (HIGH=unmute) |
| GP25 | - | Built-in LED | Status indicator |

**Note:** The Pico Audio Pack plugs directly onto the Pico header — no individual wires needed.
No I2C pins are used (PCM5100A is hardware-controlled).

---

## Pimoroni Pico Audio Pack Notes

### Hardware

The Pico Audio Pack uses a **TI PCM5100A** DAC with a **PAM8908JER** headphone amplifier:
- **Line Out (3.5mm):** Unamplified stereo, up to 2.1Vᵣₘₛ
- **Headphone Out (3.5mm):** Amplified stereo (low/high gain switch on board)
- **Mute Pin (GP22):** Controls PCM5100A XSMT — HIGH=unmute (pulled HIGH by default on board)

### I2S Pin Constraint

The RP2040 PIO state machine requires BCLK and LRCLK to be on **consecutive GPIO pins** (LRCLK = BCLK + 1). The Pico Audio Pack uses GP10 (BCLK) and GP11 (LRCLK) which satisfies this constraint.

### Configuration

No I2C/SPI configuration needed. The PCM5100A auto-detects bit depth and sample rate from I2S clock signals:
- 16-bit at 44.1kHz (default ScaleFX configuration)
- Up to 32-bit at 384kHz supported

### Codec Selection

In `hubfx_pico.ino`, uncomment:
```cpp
#define USE_PICOAUDIO_CODEC         // Pimoroni Pico Audio Pack (PCM5100A)
```

This automatically selects the correct I2S pins (GP9/GP10/GP11) and configures the mute pin (GP22).

---

## WM8960 Audio HAT Notes

### Raspberry Pi HAT Pinout Mapping

The WM8960 Audio HAT is designed for Raspberry Pi (40-pin header) with **different GPIO numbering**. When connecting to Pico, use the functional signals, not the Pi GPIO numbers:

| WM8960 Label | Raspberry Pi | Actual Function | Connect to Pico |
|--------------|--------------|-----------------|-----------------|
| CLK (GPIO18) | Pi BCM18 | I2S BCLK | GP7 (Pico I2S BCLK) |
| LRCLK (GPIO19) | Pi BCM19 | I2S LRCLK | GP8 (Pico I2S LRCLK) |
| DAC (GPIO21) | Pi BCM21 | I2S Data IN | GP6 (Pico I2S DATA) |
| SDA (GPIO2) | Pi BCM2 | I2C Data | GP4 (Pico I2C SDA) |
| SCL (GPIO3) | Pi BCM3 | I2C Clock | GP5 (Pico I2C SCL) |

**⚠️ WARNING:** Do NOT connect the WM8960 HAT directly to the Pico via 40-pin header. The GPIO numbers are incompatible. Use individual jumper wires to connect the correct functional pins.

### Audio Outputs

- **3.5mm Headphone Jack:** Stereo output, up to 40mW per channel (16Ω @ 3.3V)
- **Speaker Terminals (LP/LN, RP/RN):** Stereo Class D amplifier, 1W per channel (8Ω recommended)
- **Microphones:** Dual MEMS microphones on board (ADC input not used in this project)

### Configuration

The WM8960 codec is configured via I2C at address **0x1A**. The `wm8960_codec` module handles all register initialization:
- I2S Slave mode (Pico provides BCLK/LRCLK)
- PLL generates internal clocks from BCLK (no MCLK required)
- 44.1 kHz sample rate
- 16-bit audio
- Stereo DAC enabled
- Speaker and headphone outputs enabled

**Note:** The Pico I2S library only generates BCLK and LRCLK. The WM8960's internal PLL generates the required SYSCLK (system clock) from the BCLK signal, so no MCLK pin connection is needed.

---

## Software Configuration

See `hubfx_pico.ino` for pin definitions:

```cpp
// I2S Audio Output (to WM8960)
#define DEFAULT_PIN_I2S_DATA    6   // GP6
#define DEFAULT_PIN_I2S_BCLK    7   // GP7
#define DEFAULT_PIN_I2S_LRCLK   8   // GP8

// I2C for WM8960 Control
#define DEFAULT_PIN_I2C_SDA     4   // GP4
#define DEFAULT_PIN_I2C_SCL     5   // GP5

// SD Card (SPI0)
#define DEFAULT_PIN_SD_CS       17  // GP17
#define DEFAULT_PIN_SD_SCK      18  // GP18
#define DEFAULT_PIN_SD_MOSI     19  // GP19
#define DEFAULT_PIN_SD_MISO     16  // GP16
```

---

## Testing

1. **Power On:** Pico LED should blink (status indicator)
2. **I2C Check:** Serial output should show "WM8960 codec initialized"
3. **Audio Test:** Use serial command `play 0 /test.wav` to test playback
4. **Volume Control:** Use `volume 0.5` to adjust master volume
5. **SD Card:** Use `ls` to list SD card contents

### Troubleshooting

| Issue | Possible Cause | Solution |
|-------|---------------|----------|
| "WM8960 codec initialization failed" | I2C wiring incorrect | Check GP4/GP5 connections to SDA/SCL |
| No audio output | I2S wiring incorrect | Verify GP6/GP7/GP8 connections |
| SD card not detected | SPI wiring or format | Check SPI0 pins, format SD as FAT32 |
| Distorted audio | Clock timing issue | Ensure I2S BCLK/LRCLK are correct |
| Low volume | Codec volume too low | Increase volume via serial or codec settings |

---

## References

- [Waveshare WM8960 Audio HAT Wiki](https://www.waveshare.com/wiki/WM8960_Audio_HAT)
- [WM8960 Datasheet](https://files.waveshare.com/upload/1/18/WM8960_v4.2.pdf)
- [Raspberry Pi Pico Pinout](https://datasheets.raspberrypi.com/pico/Pico-R3-A4-Pinout.pdf)
