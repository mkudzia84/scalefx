# Helicopter FX Wiring Guide

Complete wiring diagrams and pin assignments for KA-50 helicopter FX system.

## Table of Contents
- [Overview](#overview)
- [GPIO Pin Assignments](#gpio-pin-assignments)
- [RC Receiver Connections](#rc-receiver-connections)
- [LED Wiring](#led-wiring)
- [Smoke Generator Wiring](#smoke-generator-wiring)
- [Audio HAT Connections](#audio-hat-connections)
- [Power Supply](#power-supply)
- [Complete System Diagram](#complete-system-diagram)

## Overview

The system uses a Raspberry Pi with WM8960 Audio HAT for audio output and GPIO pins for PWM input monitoring and component control.

**Important Notes:**
- All GPIO signals are 3.3V logic. Never connect 5V signals directly to GPIO pins!
- This system uses **pigpio library** for GPIO control with hardware-timed PWM
- WM8960 Audio HAT pins **must be excluded** from pigpio control (see [PIGPIO_SETUP.md](PIGPIO_SETUP.md))
- The install script automatically configures pigpio with proper pin exclusions

## GPIO Pin Assignments

### Default Configuration

| Function | GPIO Pin | Direction | Notes |
|----------|----------|-----------|-------|
| Engine Toggle PWM | 17 | Input | RC receiver channel for throttle |
| Gun Trigger PWM | 27 | Input | RC receiver channel for gun trigger |
| Smoke Heater Toggle PWM | 22 | Input | RC receiver channel for smoke heater |
| Pitch Servo PWM Input | 13 | Input | RC receiver channel for pitch control |
| Yaw Servo PWM Input | 16 | Input | RC receiver channel for yaw control |
| Nozzle Flash LED | 23 | Output | LED control (active high) |
| Smoke Fan Control | 24 | Output | Controls MOSFET for smoke fan |
| Smoke Heater Control | 25 | Output | Controls MOSFET for smoke heater |
| Pitch Servo PWM Output | 7 | Output | PWM to pitch servo |
| Yaw Servo PWM Output | 8 | Output | PWM to yaw servo |

### Reserved Pins (WM8960 Audio HAT)

**⚠️ CRITICAL:** These pins are used by the WM8960 Audio HAT and **MUST be excluded from pigpio control**

| Function | GPIO Pin | Protocol | Notes |
|----------|----------|----------|-------|
| I2C SDA | 2 | I2C | Audio HAT communication (codec control) |
| I2C SCL | 3 | I2C | Audio HAT communication (codec control) |
| I2S BCK | 18 | I2S | Audio bit clock (serial clock) |
| I2S LRCK | 19 | I2S | Audio word select (left/right clock) |
| I2S DIN | 20 | I2S | Audio data input to codec (ADC) |
| I2S DOUT | 21 | I2S | Audio data output from codec (DAC) |

**pigpio exclusion mask:** `0x3C000C` (excludes GPIO 2,3,18,19,20,21)

See [PIGPIO_SETUP.md](PIGPIO_SETUP.md) for configuration details.

### Available GPIO Pins

These pins are available for additional features:
- GPIO 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 26

**Note:** Some pins have special functions (SPI, UART) - check Raspberry Pi pinout before use.

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

**Recommended:** Bi-directional logic level converter (4-channel, 5V to 3.3V)

### Wiring Diagram - RC Receiver PWM Inputs

```
RC Receiver                    Raspberry Pi
-----------                    ------------

Channel 1 (Throttle)
   Signal  ------------------>  GPIO 17 (Engine Toggle)
   +5V     (not connected)
   GND     ------------------>  GND (Pin 6)

Channel 2 (Gun Trigger)
   Signal  ------------------>  GPIO 27 (Gun Trigger)
   +5V     (not connected)
   GND     ------------------>  GND (Pin 6)

Channel 3 (Smoke Heater)
   Signal  ------------------>  GPIO 22 (Heater Toggle)
   +5V     (not connected)
   GND     ------------------>  GND (Pin 6)
```

**Important:** 
- If RC receiver outputs 5V signals, insert level shifter on signal lines
- All grounds must be connected together (common ground)
- Do NOT connect RC receiver +5V to any GPIO pin

## LED Wiring

### Nozzle Flash LED

```
GPIO 23 (Pin 16) ----[ 330Ω ]----[LED]---- GND (Pin 14)
                     Resistor    (Anode)  (Cathode)
```

**Component Selection:**
- **LED:** Any standard LED (red/orange/white recommended)
- **Resistor:** 220-330Ω for 3.3V logic
- **Current:** ~10-15mA per LED
- **Polarity:** Long leg (anode) to resistor, short leg (cathode) to ground

### Multiple LEDs (Parallel)

For brighter flash, use multiple LEDs in parallel:

```
                    ----[ 330Ω ]----[LED1]----+
                    |                         |
GPIO 23 (Pin 16) ---+----[ 330Ω ]----[LED2]----+--- GND
                    |                         |
                    ----[ 330Ω ]----[LED3]----+
```

**Maximum:** 3-4 LEDs (total current <50mA to protect GPIO)

## Smoke Generator Wiring

### IMPORTANT SAFETY NOTES
- Smoke generator requires external power supply (NOT from Pi GPIO)
- Use MOSFET modules to control high-current devices
- Never connect smoke generator directly to GPIO pins
- Ensure proper heat dissipation for heater element
- Use appropriate gauge wire for current requirements

### MOSFET Module Wiring

```
Raspberry Pi                MOSFET Module              Smoke Component
------------                -------------              ---------------

GPIO 24 (Fan) ----------->  Signal Input
                           
GND (Pin 20) ------------>  GND
                           
                            VCC (3.3V)  <--- Optional, for module LED
                           
External 12V+ ----------->  V+ Input
                           
                            V+ Output  ------>  Fan/Heater (+)
                           
External GND ------------>  GND Common
                           
                            GND Common <------>  Fan/Heater (-)
```

### Complete Smoke Generator Wiring

```
Power Supply (12V 2A)           MOSFET Modules                 Smoke Generator
---------------------           --------------                 ---------------

(+) 12V ----+------------------> V+ (Fan Module)
            |
            +------------------> V+ (Heater Module)
            
(-) GND ----+------------------> GND (Fan Module) --------+
            |                                             |
            +------------------> GND (Heater Module) -----+--- Smoke GND Common
            |
            +------------------> Raspberry Pi GND


Raspberry Pi
------------
GPIO 24 (Pin 18) ------------>  Signal (Fan Module)
GPIO 25 (Pin 22) ------------>  Signal (Heater Module)
GND (Pin 20) ----------------->  GND (both modules)


Fan Module Output (+) --------->  Smoke Fan (+)
Fan Module GND ----------------->  Smoke Fan (-)

Heater Module Output (+) ------->  Smoke Heater (+)
Heater Module GND -------------->  Smoke Heater (-)
```

### Recommended MOSFET Modules
- **Model:** IRF520 or similar N-channel MOSFET module
- **Voltage Rating:** 24V or higher
- **Current Rating:** 5A or higher per module
- **Logic Level:** 3.3V compatible (important!)

### Smoke Generator Specifications
- **Fan Voltage:** Typically 5V or 12V (check your model)
- **Fan Current:** 100-500mA
- **Heater Voltage:** 12V (most common)
- **Heater Current:** 1-3A (depends on resistance)
- **Heater Resistance:** 4-12Ω typical

**Calculate Power:**
- P = V² / R
- Example: 12V with 6Ω heater = 24W, 2A current
- Ensure power supply can handle total current (fan + heater)

## Servo Wiring

### Turret Control Servos

The system supports two servo axes for turret control: pitch (elevation) and yaw (rotation). Each servo requires:
1. **PWM input** from RC receiver (monitored by GPIO)
2. **PWM output** to the servo motor

**Input Stage (RC Receiver → Raspberry Pi):**
```
RC Receiver                    Raspberry Pi
-----------                    -----------

Pitch Channel (CH3)
   Signal  ------------------>  GPIO 13 (Pin 33)
   GND     ------------------>  GND (shared)

Yaw Channel (CH4)
   Signal  ------------------>  GPIO 16 (Pin 36)
   GND     ------------------>  GND (shared)
```

**Output Stage (Raspberry Pi → Servos):**
```
Raspberry Pi                   Servo Motor
------------                  -----------

GPIO 7 (Pin 26) ----[PWM]----->  Signal (white/yellow)
3.3V (Pin 1) ---[optional]----->  Power (red) - if servo is 3.3V
                                 [OR use external 5V supply]

GND (Pin 9) --------[GND]----->  Ground (black/brown)
```

**Note:** Most RC servos require 5V power. Options:
- **Option 1:** Use external 5V supply for servo power
- **Option 2:** Use servo voltage reducer (7.5V to 5V)
- **Option 3:** Use low-torque servos rated for 3.3V (not recommended)

### Servo Specifications

| Parameter | Value | Notes |
|-----------|-------|-------|
| Control Type | PWM | 1000-2000µs pulse width |
| Update Rate | 50 Hz | Servo refresh rate |
| Max Speed | 500 µs/sec | Configurable via config.yaml |
| Max Accel | 2000 µs/sec² | Configurable via config.yaml |
| Voltage | 5V or higher | Depends on servo model |
| Current | 100-500mA per servo | At stall |

### Multi-Servo Power Supply

For two servos with external power:
```
5V Power Supply              Servo 1         Servo 2
---------------              -------         -------

(+) 5V ----+---[PWM]------->  Signal      
           |                 Power(+)
           +---[PWM]------->  Signal      
           |                 Power(+)
           |                 (common to both)
           
(-) GND ----+---[GND]------->  GND
           |                 
           +---[GND]------->  GND
```

**Recommended Power Supply:**
- **Voltage:** 5V DC
- **Current:** Minimum 1A (2A recommended for two servos under load)
- **Type:** Regulated DC power supply

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

### Ground Connection
**CRITICAL:** All system grounds must be connected together:
- Raspberry Pi ground
- RC receiver ground  
- Smoke generator power supply ground
- External power supply grounds

**Do NOT:**
- Power the Pi from RC receiver BEC (insufficient current)
- Power smoke generator from Pi GPIO (will damage Pi)
- Connect different voltage power supplies together (only grounds)

## Complete System Diagram

```
                                    RASPBERRY PI 4
                                    ==============
                                    (with WM8960 Audio HAT)

RC Receiver                         GPIO Pins                    Components
-----------                         ---------                    ----------

Throttle Ch --(3.3V PWM)-------->  GPIO 17     GPIO 23 -------> [LED] -> GND
                                                  |
Gun Ch ------(3.3V PWM)-------->   GPIO 27       +--[ 330Ω ]
                                                     (resistor)
Heater Ch ---(3.3V PWM)-------->   GPIO 22
                                   
                                   GPIO 24 ------> MOSFET Module --> Fan (12V)
                                   
                                   GPIO 25 ------> MOSFET Module --> Heater (12V)

Common GND --(shared)---------->   GND Pins
                                   (6,9,14,20,25,30,34,39)


Power Supplies
--------------
5V 3A ---------> USB-C (Pi power)

12V 3A -------+-> MOSFET Modules V+
              |
              +-> Common GND to Pi GND


Audio Output
------------
HAT Speakers --> Left/Right Speakers (4-8Ω, 3W)
HAT 3.5mm ----> Headphones/Amplifier (optional)
```

## Pin Reference (Physical Numbering)

```
Raspberry Pi 40-Pin Header
==========================

     3.3V  (1) (2)  5V
     SDA   (3) (4)  5V
     SCL   (5) (6)  GND
    GPIO4  (7) (8)  GPIO14
      GND  (9) (10) GPIO15
   GPIO17 (11) (12) GPIO18  <-- I2S (Audio HAT)
   GPIO27 (13) (14) GND
   GPIO22 (15) (16) GPIO23  <-- LED
     3.3V (17) (18) GPIO24  <-- Smoke Fan
   GPIO10 (19) (20) GND
    GPIO9 (21) (22) GPIO25  <-- Smoke Heater
   GPIO11 (23) (24) GPIO8
      GND (25) (26) GPIO7
    GPIO0 (27) (28) GPIO1
    GPIO5 (29) (30) GND
    GPIO6 (31) (32) GPIO12
   GPIO13 (33) (34) GND
   GPIO19 (35) (36) GPIO16  <-- I2S (Audio HAT)
   GPIO26 (37) (38) GPIO20  <-- I2S (Audio HAT)
      GND (39) (40) GPIO21  <-- I2S (Audio HAT)
```

## Testing

### Test PWM Input
```bash
# Monitor GPIO 17 for PWM pulses
sudo pigpiod
pigs modes 17 0  # Set as input
pigs r 17        # Read current state
```

### Test LED Output
```bash
# Manually control LED on GPIO 23
gpio mode 23 out
gpio write 23 1  # LED on
sleep 1
gpio write 23 0  # LED off
```

### Test MOSFET Output
```bash
# Manually control fan on GPIO 24
gpio mode 24 out
gpio write 24 1  # Fan on
sleep 2
gpio write 24 0  # Fan off
```

## Troubleshooting

### PWM Not Detected
1. Verify 3.3V logic level (use multimeter or oscilloscope)
2. Check ground connection between RC receiver and Pi
3. Ensure PWM frequency is ~50Hz (20ms period)
4. Test with known working servo/ESC on same channel

### LED Not Working
1. Check LED polarity (anode to resistor, cathode to ground)
2. Verify resistor value (220-330Ω)
3. Test GPIO pin with `gpio` command
4. Measure voltage across LED when on (~2-3V)

### Smoke Generator Not Working
1. Verify MOSFET module logic voltage (should work with 3.3V)
2. Check external power supply voltage (12V)
3. Measure current draw (should be 1-3A for heater)
4. Test MOSFET module with multimeter
5. Check for loose connections on screw terminals

### Ground Issues
- **Symptom:** Erratic behavior, random resets
- **Fix:** Ensure all grounds connected together
- **Check:** Continuity between Pi GND, receiver GND, power supply GND

## Bill of Materials

| Component | Quantity | Notes |
|-----------|----------|-------|
| Raspberry Pi 4 (2GB+) | 1 | Main controller |
| WM8960 Audio HAT | 1 | I2S audio interface |
| MicroSD Card (16GB+) | 1 | Operating system |
| 5V 3A USB-C PSU | 1 | Pi power supply |
| 12V 3A DC PSU | 1 | Smoke generator power |
| MOSFET Module (IRF520) | 2 | Fan + heater control |
| LED (red/orange) | 1-4 | Nozzle flash |
| 330Ω Resistor | 1-4 | LED current limiting |
| Smoke Generator | 1 | Fan + heater unit |
| Speakers 4-8Ω 3W | 2 | Audio output |
| Level Shifter (optional) | 1 | If using 5V RC signals |
| Jumper Wires | ~20 | Connections |
| Breadboard (optional) | 1 | Prototyping |

**Total Cost:** ~$80-120 USD (excluding RC receiver and helicopter model)

## Safety Checklist

- [ ] All GPIO connections are 3.3V logic level
- [ ] RC receiver ground connected to Pi ground
- [ ] LED resistors installed (prevent overcurrent)
- [ ] MOSFET modules rated for smoke generator current
- [ ] External power supply for smoke generator (not from Pi)
- [ ] All power supply grounds connected together
- [ ] Heat dissipation adequate for smoke heater
- [ ] Wire gauge appropriate for current (12AWG for high current)
- [ ] Secure connections (no loose wires)
- [ ] Tested individual components before full system integration

## Additional Resources

- [Raspberry Pi Pinout](https://pinout.xyz/)
- [WM8960 Audio HAT Documentation](https://www.waveshare.com/wiki/WM8960_Audio_HAT)
- [pigpio Library Documentation](http://abyz.me.uk/rpi/pigpio/)
- [RC PWM Signal Explanation](https://oscarliang.com/rc-servo-pwm-signal/)
