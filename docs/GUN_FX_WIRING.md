# Gun FX Wiring Guide

## Overview
The gun FX system provides realistic helicopter weapon effects including:
- Multiple rates of fire (selectable via PWM)
- Nozzle flash LED (blinks at firing rate)
- Audio playback (looping gun sounds)
- Smoke generator (fan + heater)

## Recommended Wiring (Raspberry Pi)

### PWM Inputs (from RC receiver)
```
RC Receiver                    Raspberry Pi
┌─────────────┐               ┌─────────────┐
│             │               │             │
│  CH5 Signal ├──────────────►│ GPIO 27     │  Trigger control
│             │               │             │
│  CH6 Signal ├──────────────►│ GPIO 22     │  Smoke heater toggle
│             │               │             │
│  Ground     ├──────────────►│ GND         │
│             │               │             │
└─────────────┘               └─────────────┘
```

**Notes:**
- Connect PWM signal wires only (not +5V from receiver)
- Share common ground between receiver and Pi
- PWM signals: 1000-2000µs pulse width, 50Hz typical
- GPIO 17 is reserved for engine FX (not used by gun system)
- GPIO 18-21 used by WM8960 Audio HAT (I2S interface)

### LED Output (Nozzle Flash)
```
Raspberry Pi                   LED Circuit
┌─────────────┐               ┌─────────────┐
│             │               │             │
│  GPIO 23    ├──────────┬───►│ R (220Ω)    │──►│LED├─┐
│             │          │    │             │           │
│             │          │    └─────────────┘           │
│             │          │                              │
│  GND        ├──────────┴──────────────────────────────┘
│             │
└─────────────┘
```

**Specifications:**
- Resistor: 220Ω (adjust for LED current rating)
- LED: Any standard LED (red/white recommended for muzzle flash)
- Blink rate: Matches firing rate (300-900 RPM)

### Smoke Generator Outputs
```
Raspberry Pi              Relay Module           Smoke Generator
┌─────────────┐          ┌──────────┐           ┌──────────────┐
│             │          │          │           │              │
│  GPIO 24    ├─────────►│ IN1  NO1 ├──────────►│ Fan (+12V)   │
│             │          │          │           │              │
│  GPIO 25    ├─────────►│ IN2  NO2 ├──────────►│ Heater (+12V)│
│             │          │          │           │              │
│  GND        ├─────────►│ GND      │           │              │
│             │          │          │           │              │
│             │          │ VCC  COM ├───┐       │ Ground       │
│  +5V        ├─────────►│          │   │       │              │
│             │          └──────────┘   │       └──────────────┘
└─────────────┘                         │
                                        │
                          +12V Supply ──┘
```

**Specifications:**
- Relay Module: 2-channel, 5V trigger, 12V/10A contacts
- Smoke Generator: 12V DC (fan + heating element)
- Fan: 500-1000mA typical
- Heater: 1-3A typical (depends on smoke unit)

**Important:**
- Use separate power supply for smoke generator (12V)
- Do NOT power smoke generator from Raspberry Pi
- Ensure relay module is rated for smoke generator current

## Complete System Wiring Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                      RC Receiver                            │
│  CH5 ──┐  CH6 ──┐                                           │
└────────┼────────┼───────────────────────────────────────────┘
         │        │
         │        │     ┌──────────────────────────────────┐
         │        │     │      Raspberry Pi 4              │
         │        │     │                                  │
         └────────┼────►│ GPIO 27  (Trigger PWM in)        │
                  └────►│ GPIO 22  (Smoke toggle PWM in)   │
                        │                                  │
                  ┌────►│ GPIO 23  (Nozzle flash out)      │───┐
                  │     │ GPIO 24  (Smoke fan out)         │───┼──┐
                  │     │ GPIO 25  (Smoke heater out)      │───┼──┼──┐
                  │     │                                  │   │  │  │
                  │     │ Audio HAT (I2S)                  │───┼──┼──┼──┐
                  │     └──────────────────────────────────┘   │  │  │  │
                  │                                            │  │  │  │
            ┌─────┴─────┐                                      │  │  │  │
            │    LED    │                                      │  │  │  │
            │ w/ 220Ω R │                                      │  │  │  │
            └───────────┘                                      │  │  │  │
                                                               │  │  │  │
                                    ┌──────────────────────────┘  │  │  │
                                    │  ┌─────────────────────────┘  │  │
                                    │  │  ┌──────────────────────────┘  │
                                    │  │  │                             │
                              ┌─────▼──▼──▼──────┐              ┌──────▼──────┐
                              │  Relay Module    │              │   Speaker   │
                              │  (2-channel)     │              │   or        │
                              │                  │              │  Amplifier  │
                              │  NO1 ──┐         │              └─────────────┘
                              │  NO2 ──┼──┐      │
                              └────────┼──┼──────┘
                                       │  │
                              ┌────────▼──▼──────┐
                              │ Smoke Generator  │
                              │  Fan + Heater    │
                              │    (12V DC)      │
                              └──────────────────┘
                                       │
                                 ┌─────▼─────┐
                                 │  12V PSU  │
                                 │  (3A min) │
                                 └───────────┘
```

## PWM Signal Reference

### Trigger Channel (GPIO 27)
| PWM Width | Action           | Rate  | Status             |
|-----------|------------------|-------|--------------------|
| <1100µs   | Off              | 0 RPM | Not firing         |
| 1200µs    | Slow firing      | 300   | LED slow blink     |
| 1400µs    | Medium firing    | 600   | LED medium blink   |
| 1600µs    | Fast firing      | 900   | LED fast blink     |

**Hysteresis:** ±100µs deadzone prevents rapid switching

### Smoke Heater Toggle (GPIO 22)
| PWM Width | Action      |
|-----------|-------------|
| <1500µs   | Heater OFF  |
| ≥1500µs   | Heater ON   |

**Note:** Smoke fan turns on automatically during firing (independent of heater)

## Parts List

| Component              | Specification          | Qty | Notes                          |
|------------------------|------------------------|-----|--------------------------------|
| Raspberry Pi           | Pi 4 or better         | 1   | Any model with GPIO            |
| Audio HAT              | I2S DAC HAT            | 1   | Raspberry Pi audio HAT         |
| 2-ch Relay Module      | 5V trigger, 12V/10A    | 1   | Opto-isolated recommended      |
| LED                    | 5mm, any color         | 1   | Red/white for realism          |
| Resistor               | 220Ω, 1/4W             | 1   | Current limiting for LED       |
| Smoke Generator        | 12V DC                 | 1   | Helicopter smoke unit          |
| 12V Power Supply       | 3A minimum             | 1   | For smoke generator            |
| Jumper Wires           | Female-female          | 10  | GPIO connections               |

## Safety Notes

⚠️ **Important Safety Information:**

1. **Audio HAT Installation:** Ensure audio HAT is properly seated on GPIO header before powering on
2. **WM8960 Reserved Pins:** GPIO 2,3 (I2C), GPIO 17-21 (I2S + button) are used by audio HAT
3. **Electrical Isolation:** Always use a relay module to isolate Raspberry Pi GPIO from high-current loads
2. **Power Supply:** Never attempt to power smoke generator from Raspberry Pi GPIO
3. **Heater Current:** Smoke generator heaters can draw 1-3A, ensure adequate power supply
4. **Thermal Protection:** Smoke generators get hot - mount in ventilated area away from plastic
5. **Testing:** Test each component individually before full system integration
6. **Fusing:** Add inline fuse (5A) on 12V supply for smoke generator protection

## Software Setup

1. Place sound files in `sounds/` directory:
   - `cannon_slow.wav` (300 RPM sound)
   - `cannon_medium.wav` (600 RPM sound)
   - `cannon_fast.wav` (900 RPM sound)

2. Compile and run demo:
   ```bash
   gcc -o gun_fx_demo gun_fx_demo.c gun_fx.c lights.c smoke_generator.c \
       audio_player.c gpio.c -lpthread -lminiaudio -lm
   sudo ./gun_fx_demo
   ```

3. Test with RC transmitter:
   - Move trigger channel stick through range
   - Observe LED blink rate changes
   - Listen for sound changes
   - Toggle smoke heater channel

## Troubleshooting

**LED not blinking:**
- Check GPIO 23 connection and resistor
- Verify LED polarity (long leg = +)
- Test LED separately with 3.3V

**Smoke not working:**
- Verify 12V power supply connected
- Check relay module power and triggering
- Test relay outputs with multimeter
- Ensure smoke generator is primed

**No sound:**
- Check audio HAT is properly installed and detected
- Verify sound files exist and are valid WAV format
- Test audio mixer separately
- Check ALSA configuration for I2S HAT

**Rate not switching:**
- Verify PWM input on GPIO 27
- Check receiver channel assignment
- Monitor PWM values in console output
- Ensure transmitter trim is centered
