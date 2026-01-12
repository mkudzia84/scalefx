# GunFX Wiring Guide

Complete wiring diagrams for the GunFX Pico controller.

## System Architecture

```
RC Receiver ──► HubFX (Raspberry Pi) ◄──USB CDC──► GunFX (Pico)
                     │                                    │
                     │ Audio output                       ├─► Servos (1, 2, 3)
                     ▼                                    ├─► Nozzle Flash LED
                 Speakers                                 ├─► Smoke Fan (PWM)
                                                          ├─► Smoke Heater
                                                          ├─► Blue Status LED
                                                          └─► Yellow Heater LED
```

## Pico Pinout

| GPIO | Function | Notes |
|------|----------|-------|
| 1 | Servo 1 | PWM output |
| 2 | Servo 2 | PWM output |
| 3 | Servo 3 | PWM output |
| 13 | Blue LED | Status indicator |
| 14 | Yellow LED | Heater indicator |
| 16 | Smoke Fan | PWM speed control |
| 17 | Smoke Heater | On/off control |
| 25 | Nozzle Flash | PWM LED output |

## Wiring Diagrams

### Servo Wiring

```
Pico                         Servo
────                         ─────
GPIO 1 ──────────────────►   Signal (white/yellow)
                             Power (red) ◄──── External 5-6V
GND ─────────────────────►   Ground (black/brown)
```

**Notes:**
- Use external power for servos (not from Pico)
- Share common ground between Pico and servo power supply
- Servos typically need 5-6V, 500mA+ each

### Nozzle Flash LED

```
Pico                    LED Circuit
────                    ───────────
GPIO 25 ────[ 220Ω ]────►├ LED ├─── GND
```

For high-power LEDs, use a MOSFET driver:
```
Pico                 MOSFET         LED
────                 ──────         ───
GPIO 25 ───[ 1kΩ ]──┤ Gate
                    │ Source ────── GND
External V+ ────────┼──────────────►(+) LED (+)
                    │ Drain ───────►(-) LED (-)
```

### Smoke Generator (MOSFET Control)

```
Pico               MOSFET Module           Smoke Component
────               ──────────────           ───────────────
GPIO 16 ──────────►  Signal (Fan)
GPIO 17 ──────────►  Signal (Heater)
GND ──────────────►  GND

External 12V+ ────►  V+ ──────────────────► Fan/Heater (+)
External GND ─────►  GND ─────────────────► Fan/Heater (-)
```

**Recommended MOSFET modules:** IRF520, IRLZ44N (logic-level)

**Fan PWM:** GPIO 16 supports PWM for variable fan speed control via the `SMOKE_SETTINGS` command.

### Status LEDs

```
Pico                    LED Circuits
────                    ────────────
GPIO 13 ────[ 220Ω ]────►├ Blue LED ├─── GND    (Status)
GPIO 14 ────[ 220Ω ]────►├ Yellow LED ├── GND   (Heater indicator)
```

**LED Behavior:**
| LED | Condition | State |
|-----|-----------|-------|
| Yellow | Heater OFF | OFF |
| Yellow | Heater ON | Solid ON |
| Blue | Idle | OFF |
| Blue | Firing | Synced with flash |
| Blue | Watchdog timeout | Slow blink (1s on / 2s off) |

## USB Connection

Connect Pico to HubFX board via USB-C cable (custom cable provides power + data).

**USB Enumeration:**
- **VID:** 0x2e8a (Raspberry Pi Foundation)
- **PID:** 0x0180 (gunfx_pico)

The HubFX auto-detects the device by VID/PID - no configuration required.

## Power Requirements

| Component | Voltage | Current | Notes |
|-----------|---------|---------|-------|
| Pico | 5V (USB) | 50mA | Powered from HubFX USB |
| Servos | 5-6V | 500mA each | External supply |
| Smoke Fan | 12V | 500mA | External supply |
| Smoke Heater | 12V | 1-3A | External supply |
| LEDs | 3.3V | 10mA each | From Pico GPIO |

**Important:**
- Use separate power supplies for high-current loads
- Share common ground between all power supplies and Pico
- Add flyback diodes on inductive loads (fan motor)

## Safety Notes

⚠️ **Important Safety Information:**

1. **Never power smoke generator from Pico GPIO** - use external power + MOSFET
2. **Use MOSFET modules** for smoke fan and heater control
3. **Smoke heaters get hot** - mount in ventilated area
4. **Add inline fuse** (5A) on 12V smoke generator supply
5. **Test components individually** before full integration
6. **Ensure adequate wire gauge** for high-current paths (16-18 AWG minimum)
