# Helicopter FX Wiring Guide

Complete wiring diagrams and pin assignments for ScaleFX helicopter FX system.

## Table of Contents
- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Raspberry Pi GPIO Assignments](#raspberry-pi-gpio-assignments)
- [RC Receiver Connections](#rc-receiver-connections)
- [Audio HAT Connections](#audio-hat-connections)
- [GunFX Pico Wiring](#gunfx-pico-wiring)
- [Power Supply](#power-supply)
- [Complete System Diagram](#complete-system-diagram)

## Overview

The system uses a distributed architecture:
- **Raspberry Pi** with audio HAT for sound playback and PWM input monitoring
- **Raspberry Pi Pico** (GunFX controller) for gun hardware outputs

**Important Notes:**
- All Raspberry Pi GPIO signals are 3.3V logic. Never connect 5V signals directly to GPIO pins!
- This system uses **pigpio library** for GPIO control
- Audio HAT pins are automatically protected by the software
- Gun outputs (servos, LEDs, smoke) are controlled by the Pico, not Pi GPIO

## System Architecture

```
                    ┌─────────────────────────────────────────┐
                    │           RASPBERRY PI                  │
                    │         (Audio + PWM Inputs)            │
                    │                                         │
RC Receiver ───────►│  GPIO 17: Engine Toggle                 │
                    │  GPIO 27: Gun Trigger                   │
                    │  GPIO 22: Smoke Heater Toggle           │
                    │  GPIO 13: Pitch Servo Input             │
                    │  GPIO 16: Yaw Servo Input               │
                    │                                         │
                    │  Audio HAT ──────────────► Speakers     │
                    │                                         │
                    │  USB ─────────────────────────┐         │
                    └───────────────────────────────┼─────────┘
                                                    │
                                                    ▼
                    ┌───────────────────────────────────────────┐
                    │         RASPBERRY PI PICO                 │
                    │           (GunFX Controller)              │
                    │                                           │
                    │  GPIO 1  ─────────────────► Servo 1       │
                    │  GPIO 2  ─────────────────► Servo 2       │
                    │  GPIO 3  ─────────────────► Servo 3       │
                    │  GPIO 13 ─────────────────► Blue LED      │
                    │  GPIO 14 ─────────────────► Yellow LED    │
                    │  GPIO 16 ─────────────────► Smoke Fan     │
                    │  GPIO 17 ─────────────────► Smoke Heater  │
                    │  GPIO 25 ─────────────────► Nozzle Flash  │
                    └───────────────────────────────────────────┘
```

## Raspberry Pi GPIO Assignments

### PWM Input Pins

| Function | GPIO Pin | Direction | Notes |
|----------|----------|-----------|-------|
| Engine Toggle PWM | 17 | Input | RC receiver channel for throttle |
| Gun Trigger PWM | 27 | Input | RC receiver channel for gun trigger |
| Smoke Heater Toggle PWM | 22 | Input | RC receiver channel for smoke heater |
| Pitch Servo PWM Input | 13 | Input | RC receiver channel for pitch control |
| Yaw Servo PWM Input | 16 | Input | RC receiver channel for yaw control |

### Reserved Pins (Audio HATs)

**⚠️ CRITICAL:** These pins are used by audio HATs and are **automatically protected** by the software.

| Function | GPIO Pin | Protocol | Notes |
|----------|----------|----------|-------|
| I2C SDA | 2 | I2C | Audio HAT control |
| I2C SCL | 3 | I2C | Audio HAT control |
| I2S BCK | 18 | I2S | Audio bit clock |
| I2S LRCK | 19 | I2S | Audio word select |
| I2S DIN | 20 | I2S | Audio data in |
| I2S DOUT | 21 | I2S | Audio data out |

**Protected pins:** GPIO 2, 3, 18, 19, 20, 21

## RC Receiver Connections

### PWM Signal Requirements

- **Signal Type:** Standard RC PWM (50Hz)
- **Pulse Width:** 1000-2000µs (1ms to 2ms)
- **Voltage:** 3.3V logic level
- **Common:** Ground must be shared between Pi and RC receiver

### Level Shifting (if needed)

If your RC receiver outputs 5V PWM signals, you MUST use a level shifter:

```
RC Receiver (5V)  -->  Level Shifter  -->  Raspberry Pi (3.3V)
     |                      |                      |
   Signal              5V -> 3.3V              GPIO Pin
     |                      |                      |
    GND ----------------  Common Ground  ---------- GND
```

### Wiring Diagram

```
RC Receiver                    Raspberry Pi
-----------                    ------------

Channel 1 (Throttle)
   Signal  ------------------>  GPIO 17 (Engine Toggle)
   GND     ------------------>  GND

Channel 2 (Gun Trigger)
   Signal  ------------------>  GPIO 27 (Gun Trigger)
   GND     ------------------>  GND

Channel 3 (Smoke Heater)
   Signal  ------------------>  GPIO 22 (Heater Toggle)
   GND     ------------------>  GND

Channel 4 (Pitch)
   Signal  ------------------>  GPIO 13 (Pitch Input)
   GND     ------------------>  GND

Channel 5 (Yaw)
   Signal  ------------------>  GPIO 16 (Yaw Input)
   GND     ------------------>  GND
```

**Important:** 
- Do NOT connect RC receiver +5V to any GPIO pin
- All grounds must be connected together (common ground)

## Audio HAT Connections

### WM8960 Audio HAT

The WM8960 Audio HAT mounts directly on the Raspberry Pi GPIO header.

**Connections:**
- Automatically uses I2C (GPIO 2, 3) and I2S (GPIO 18-21)
- Speaker output: 3W per channel (left/right)
- Headphone output: 3.5mm jack
- Microphone input: Not used by this system

**Speaker Wiring:**
```
Speaker Terminals on HAT        Speakers
------------------------        --------
Left (+) ------------------>    Left Speaker (+)
Left (-) ------------------>    Left Speaker (-)
Right (+) ----------------->    Right Speaker (+)
Right (-) ----------------->    Right Speaker (-)
```

**Recommended Speakers:**
- Impedance: 4-8Ω
- Power: 3-5W
- Type: Full-range or small 2-way

## Power Supply

### Raspberry Pi Power
- **Voltage:** 5V DC
- **Current:** Minimum 2.5A (3A recommended for Pi 4)
- **Connector:** USB-C (Pi 4) or Micro-USB (Pi 3)

### Smoke Generator Power
- **Voltage:** 12V DC (typical)
- **Current:** 3-5A minimum (depends on heater + fan)
- **Connector:** Barrel jack or screw terminals
- **Type:** Regulated DC power supply

## GunFX Pico Wiring

The GunFX Pico controller handles all gun hardware outputs. See [GUN_FX_WIRING.md](GUN_FX_WIRING.md) for detailed Pico wiring diagrams.

**Pico Pin Summary:**

| GPIO | Function |
|------|----------|
| 1 | Servo 1 |
| 2 | Servo 2 |
| 3 | Servo 3 |
| 13 | Blue LED (status) |
| 14 | Yellow LED (heater indicator) |
| 16 | Smoke Fan Motor |
| 17 | Smoke Heater |
| 25 | Nozzle Flash (PWM) |

## Audio HAT Connections

### WM8960 Audio HAT

The WM8960 Audio HAT mounts directly on the Raspberry Pi GPIO header.

**Connections:**
- Automatically uses I2C (GPIO 2, 3) and I2S (GPIO 18-21)
- Speaker output: 3W per channel (left/right)
- Headphone output: 3.5mm jack

**Speaker Wiring:**
```
Speaker Terminals on HAT        Speakers
------------------------        --------
Left (+) ------------------>    Left Speaker (+)
Left (-) ------------------>    Left Speaker (-)
Right (+) ----------------->    Right Speaker (+)
Right (-) ----------------->    Right Speaker (-)
```

**Recommended Speakers:**
- Impedance: 4-8Ω
- Power: 3-5W
- Type: Full-range or small 2-way

## Power Supply

### Raspberry Pi Power
- **Voltage:** 5V DC
- **Current:** Minimum 2.5A (3A recommended for Pi 4)
- **Connector:** USB-C (Pi 4) or Micro-USB (Pi 3)

### Pico Power
- **Voltage:** 5V via USB from Raspberry Pi
- **Current:** ~50mA (Pico draws minimal power)

### Smoke Generator Power
- **Voltage:** 12V DC (typical)
- **Current:** 3-5A minimum (depends on heater + fan)
- **Connector:** Barrel jack or screw terminals

### Servo Power
- **Voltage:** 5-6V DC
- **Current:** 500mA+ per servo

### Ground Connection
**CRITICAL:** All system grounds must be connected together:
- Raspberry Pi ground
- Pico ground (via USB)
- RC receiver ground  
- Smoke generator power supply ground
- Servo power supply ground

## Complete System Diagram

```
                    ┌─────────────────────────────────────────┐
                    │           RASPBERRY PI 4                │
                    │         (with WM8960 Audio HAT)         │
                    │                                         │
RC Receiver ───────►│  GPIO 17: Engine Toggle                 │
(PWM Inputs)        │  GPIO 27: Gun Trigger                   │
                    │  GPIO 22: Smoke Heater Toggle           │
                    │  GPIO 13: Pitch Input                   │
                    │  GPIO 16: Yaw Input                     │
                    │                                         │
                    │  Audio HAT ──────────────► Speakers     │
                    │                                         │
                    │  USB ─────────────────────────┐         │
                    └───────────────────────────────┼─────────┘
                                                    │
                                                    ▼ USB CDC
                    ┌───────────────────────────────────────────┐
                    │         RASPBERRY PI PICO                 │
                    │                                           │
                    │  GPIO 1-3 ────────────────► Servos        │
                    │  GPIO 13,14 ──────────────► Status LEDs   │
                    │  GPIO 16,17 ──[MOSFET]───► Smoke Gen      │
                    │  GPIO 25 ─────────────────► Nozzle Flash  │
                    └───────────────────────────────────────────┘

Power Supplies:
  5V 3A ────► Pi (USB-C)
  5V 2A ────► Servos (external)
  12V 3A ───► Smoke Generator (via MOSFET modules on Pico)
```

## Pin Reference (Raspberry Pi Physical Numbering)

```
Raspberry Pi 40-Pin Header
==========================

     3.3V  (1) (2)  5V
     SDA   (3) (4)  5V           ◄── Audio HAT I2C
     SCL   (5) (6)  GND          ◄── Audio HAT I2C
    GPIO4  (7) (8)  GPIO14
      GND  (9) (10) GPIO15
   GPIO17 (11) (12) GPIO18       ◄── Engine Toggle / I2S
   GPIO27 (13) (14) GND          ◄── Gun Trigger
   GPIO22 (15) (16) GPIO23       ◄── Smoke Toggle
     3.3V (17) (18) GPIO24
   GPIO10 (19) (20) GND
    GPIO9 (21) (22) GPIO25
   GPIO11 (23) (24) GPIO8
      GND (25) (26) GPIO7
    GPIO0 (27) (28) GPIO1
    GPIO5 (29) (30) GND
    GPIO6 (31) (32) GPIO12
   GPIO13 (33) (34) GND          ◄── Pitch Input
   GPIO19 (35) (36) GPIO16       ◄── I2S / Yaw Input
   GPIO26 (37) (38) GPIO20       ◄── I2S
      GND (39) (40) GPIO21       ◄── I2S
```

## Testing

### Test PWM Input
```bash
# Start pigpiod daemon
sudo systemctl start pigpiod

# Monitor GPIO for PWM
pigs modes 17 0  # Set as input
pigs r 17        # Read current state
```

## Troubleshooting

### PWM Not Detected
1. Verify 3.3V logic level (use multimeter or oscilloscope)
2. Check ground connection between RC receiver and Pi
3. Ensure PWM frequency is ~50Hz (20ms period)
4. Test with known working servo/ESC on same channel

### Pico Not Detected
1. Check USB cable (must support data, not just power)
2. Verify Pico firmware is flashed correctly
3. Check dmesg for USB enumeration: `dmesg | grep -i usb`
4. Look for /dev/ttyACM0 device

### Ground Issues
- **Symptom:** Erratic behavior, random resets
- **Fix:** Ensure all grounds connected together
- **Check:** Continuity between Pi GND, Pico GND, receiver GND, power supply GND

## Bill of Materials

| Component | Quantity | Notes |
|-----------|----------|-------|
| Raspberry Pi 4 (2GB+) | 1 | Main controller |
| Raspberry Pi Pico | 1 | GunFX controller |
| WM8960 Audio HAT | 1 | I2S audio interface |
| MicroSD Card (16GB+) | 1 | Operating system |
| 5V 3A USB-C PSU | 1 | Pi power supply |
| 5V 2A DC PSU | 1 | Servo power supply |
| 12V 3A DC PSU | 1 | Smoke generator power |
| MOSFET Module (IRF520) | 2 | Fan + heater control |
| RC Servos | 2-3 | Turret control |
| LED (various) | 3 | Flash + status |
| 220Ω Resistor | 3 | LED current limiting |
| Smoke Generator | 1 | Fan + heater unit |
| Speakers 4-8Ω 3W | 2 | Audio output |
| Level Shifter (optional) | 1 | If using 5V RC signals |
| USB Cable (data) | 1 | Pi to Pico |
| Jumper Wires | ~30 | Connections |

## Safety Checklist

- [ ] All GPIO connections are 3.3V logic level
- [ ] RC receiver ground connected to Pi ground
- [ ] LED resistors installed (prevent overcurrent)
- [ ] MOSFET modules rated for smoke generator current
- [ ] External power supplies for high-current loads
- [ ] All power supply grounds connected together
- [ ] Heat dissipation adequate for smoke heater
- [ ] Secure connections (no loose wires)
- [ ] Tested individual components before full integration

## Additional Resources

- [Raspberry Pi Pinout](https://pinout.xyz/)
- [WM8960 Audio HAT Documentation](https://www.waveshare.com/wiki/WM8960_Audio_HAT)
- [pigpio Library Documentation](http://abyz.me.uk/rpi/pigpio/)
- [Arduino-Pico Documentation](https://arduino-pico.readthedocs.io/)

