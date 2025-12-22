# GunFX Controller - Raspberry Pi Pico

Slave microcontroller for gun FX hardware control. Receives commands from the main Raspberry Pi over USB serial and drives:
- Nozzle flash LED (PWM)
- Smoke heater and fan
- Turret servos (pitch, yaw, retract)

## Hardware Setup

### Raspberry Pi Pico Pins

Outputs are grouped on the **right side** with servos adjacent:

**Gun Servo Outputs (PWM):**
- GPIO 19 (Physical Pin 28): Gun Servo 1
- GPIO 20 (Physical Pin 30): Gun Servo 2
- GPIO 21 (Physical Pin 32): Gun Servo 3

**Smoke and Flash Outputs:**
- GPIO 22 (Physical Pin 34): Nozzle flash LED (PWM)
- GPIO 16 (Physical Pin 21): Smoke heater (MOSFET/relay)
- GPIO 17 (Physical Pin 22): Smoke fan (MOSFET/relay)

**Board Layout Reference:**
```
Left Side (1-20)     |     Right Side (21-40)
─────────────────────────────────────────
...                  |     GP16 · GP17 (21-22) ← Heater / Fan
...                  |     GND  ·  GP19 (28)    ← Servo 1
...                  |     GND  ·  GP20 (30)    ← Servo 2
...                  |     GND  ·  GP21 (32)    ← Servo 3
...                  |     GND  ·  GP22 (34)    ← Flash LED
...                  |     ...  ·  ...
```

**Power:**
- Connect Pico via USB to main Raspberry Pi
- Use external power supply for servos and high-current loads (smoke heater/fan)
- Common ground between Pico, power supply, and all peripherals

**MOSFET Drivers:**
- For smoke heater and fan, use logic-level MOSFETs (e.g., IRLZ44N)
- Add flyback diodes if loads are inductive
- Gate resistors: 220Ω–1kΩ between Pico GPIO and MOSFET gate

### Binary Protocol

**Baud Rate:** 115200  
**Format:** Binary packets, COBS-encoded, terminated by 0x00 delimiter

Packet format (before COBS encoding):
```
[type:u8][len:u8][payload:len bytes][crc8:u8]
```
CRC-8 polynomial 0x07 computed over type + len + payload.

### Commands (Pi → Pico)

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x01 | TRIGGER_ON | rpm:u16le | Start firing at specified RPM |
| 0x02 | TRIGGER_OFF | fan_delay_ms:u16le | Stop firing; delay before fan turns off |
| 0x10 | SRV_SET | servo_id:u8, pulse_us:u16le | Set servo position |
| 0x11 | SRV_SETTINGS | servo_id:u8, min:u16le, max:u16le, max_speed:u16le, accel:u16le, decel:u16le | Configure servo limits and motion profile |
| 0x12 | SRV_RECOIL_JERK | servo_id:u8, jerk_us:u16le, variance_us:u16le | Configure recoil jerk per shot |
| 0x20 | SMOKE_HEAT | on:u8 (0=off, 1=on) | Control smoke heater |
| 0xF0 | INIT | (none) | Daemon initialization - reset to safe state |
| 0xF1 | SHUTDOWN | (none) | Daemon shutdown - enter safe state |
| 0xF2 | KEEPALIVE | (none) | Periodic keepalive from daemon |

### Telemetry (Pico → Pi)

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0xF3 | INIT_READY | module_name:string | Response to INIT with module name |
| 0xF4 | STATUS | flags:u8, fan_off_remaining_ms:u16le, servo_us[3]:u16le each, rpm:u16le | Periodic status update |

**Status flags (bit field):**
- bit0: firing
- bit1: flash_active
- bit2: flash_fading
- bit3: heater_on
- bit4: fan_on
- bit5: fan_spindown

**Servo motion:** `SRV_SET` commands move smoothly using trapezoidal velocity: accel until max_speed, decel to stop at target, and automatic braking/turnaround when reversing direction. Defaults: max_speed=4000 µs/s, accel=8000 µs/s², decel=8000 µs/s²; override per servo with `SRV_SETTINGS`.

### Recoil Jerk Effect

The `SRV_RECOIL_JERK` command configures a simulated recoil kick effect on turret servos. When configured:

- On each shot (muzzle flash), a random jerk offset is applied to the servo position
- Jerk direction is randomly ± (positive or negative)
- Jerk magnitude = base `jerk_us` + random(0 to `variance_us`)
- The jerk is cleared after the flash fade completes
- Jerk offsets are additive to the normal motion profile

Example: With `jerk_us=50` and `variance_us=25`, each shot applies ±50µs to ±75µs of random offset.

### Firing Behavior

- `TRIGGER_ON` starts the muzzle flash blinking at the specified RPM
- Flash pulse duration: 30 ms per shot
- Smoke fan turns on immediately when firing starts
- When `TRIGGER_OFF` is received, the muzzle flash stops immediately, but the smoke fan continues running for the specified delay (e.g., 3000 ms) before turning off

## Build & Upload

### Using Arduino IDE

1. Install the [Arduino-Pico board package](https://github.com/earlephilhower/arduino-pico)
   - File → Preferences → Additional Board Manager URLs: `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   - Tools → Board → Boards Manager → Search "pico" → Install "Raspberry Pi Pico/RP2040"

2. Open `controllers/gunfx/pico/gunfx_pico.ino`

3. Configure:
   - Tools → Board → Raspberry Pi Pico/RP2040 → Raspberry Pi Pico
   - Tools → Port → (select COM port)
   - Tools → USB Stack → "Pico SDK"

4. Upload

### Using PlatformIO (Optional)

Create `platformio.ini` in the `pico` folder:

```ini
[env:pico]
platform = raspberrypi
board = pico
framework = arduino
board_build.core = earlephilhower
lib_deps = 
    Servo
monitor_speed = 115200
```

Then:
```bash
cd controllers/gunfx/pico
pio run -t upload
pio device monitor
```

## Testing

### Serial Monitor Test

Open Serial Monitor at 115200 baud and send commands:

```
SMOKE_HEAT_ON
TRIGGER_ON;200
SRV:1:1500
SRV:2:1500
SRV:3:1500
TRIGGER_OFF;3000
SMOKE_HEAT_OFF
```

Expected output:
- LED flashes at specified rate
- Servos move to commanded positions with smooth accel/decel and clamped limits
- Heater/fan respond to commands
- Status messages confirm each command

### Python Test Script (from main Raspberry Pi)

```python
import serial
import time

# Open serial connection to Pico
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
time.sleep(2)  # Wait for Pico to initialize

# Turn heater on
ser.write(b'SMOKE_HEAT_ON\n')
time.sleep(0.5)

# Start firing at 400 RPM
ser.write(b'TRIGGER_ON;400\n')
time.sleep(5)

# Set gun servo positions
ser.write(b'SRV:1:1600\n')
ser.write(b'SRV:2:1400\n')
ser.write(b'SRV:3:1500\n')
time.sleep(2)

# Stop firing with 3 second fan delay
ser.write(b'TRIGGER_OFF;3000\n')
time.sleep(5)

# Turn heater off
ser.write(b'SMOKE_HEAT_OFF\n')

# Read responses
while ser.in_waiting:
    print(ser.readline().decode().strip())

ser.close()
```

## Pin Customization

To change pin assignments, edit the constants at the top of `gunfx_pico.ino`. The format is:

```cpp
const uint8_t PIN_NAME = GPIO; // GP[GPIO] (Physical Pin [PIN])
```

Current configuration (right side grouping):
```cpp
// Gun Servo Outputs (PWM)
const uint8_t PIN_GUN_SRV_1       = 19; // GP19 (Physical Pin 28)
const uint8_t PIN_GUN_SRV_2       = 20; // GP20 (Physical Pin 30)
const uint8_t PIN_GUN_SRV_3       = 21; // GP21 (Physical Pin 32)

// Smoke and Flash Outputs
const uint8_t PIN_NOZZLE_FLASH    = 22; // GP22 (Physical Pin 34)
const uint8_t PIN_SMOKE_HEATER    = 16; // GP16 (Physical Pin 21)
const uint8_t PIN_SMOKE_FAN       = 17; // GP17 (Physical Pin 22)
```

All Pico GPIOs (0-25) support PWM and can be used for servo control or LED PWM.

## Troubleshooting

**Pico not detected:**
- Hold BOOTSEL button while connecting USB
- Pico should appear as a mass storage device
- Drag and drop UF2 file from Arduino IDE build output

**Servos jittering:**
- Check power supply voltage (servos typically need 5-6V)
- Ensure common ground between Pico and servo power
- Add decoupling capacitors near servos

**High-current loads not working:**
- Verify MOSFET wiring and gate resistors
- Check power supply capacity
- Use multimeter to verify GPIO output voltage (3.3V)

**Serial communication issues:**
- Confirm baud rate (115200)
- Check USB cable (must support data, not just power)
- On Linux, add user to `dialout` group: `sudo usermod -a -G dialout $USER`
