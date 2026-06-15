# Parameter reference (per tab / feature)

This is the authoritative list of the settings ScaleFX Studio actually exposes,
grouped by the tab/effect where you set them. Each row is: the setting, what it
does, its options/range, and its **default** (the value a freshly-added item or an
unset field takes).

**Rule for the assistant:** only describe settings, options, modes, ranges, and
defaults that appear on THIS page (cross-referenced with the operator's live
state). Do not invent parameters, extra modes, value ranges, or defaults. If the
operator asks about something not here, say you're not certain it exists, name the
closest real setting, and point them at the tab — rather than guessing. The live
state shows the operator's CURRENT values; this page shows the defaults so you can
tell them what an unset setting falls back to.

Units: **µs** = servo/RC pulse width (1000–2000 µs is the normal RC range),
**ms** = milliseconds, **%** = 0–100 percent, **mV/mA** = millivolts/milliamps.

---

## Input & Ports tab

### Input port (the RC receiver link)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Protocol | how the receiver connects | `PPM` / `SBUS` / `Jeti EX Input` (limited by what the port supports) | — (operator picks) |
| Channel count | how many RC channels to read | up to the protocol's max; can auto-expand from the live signal | — |

### Channel mapping (one per RC channel)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Function | what the channel drives | a named function (`engine_toggle`, `gun_trigger`, `gear_updown`, `gun_rof`, `light_program`, `light_brightness`, `gun_smoke`, `gun_yaw`, `gun_pitch`, `master_volume`) or a custom name | unset |

Effects reference channels by this **name**, not by number.

### Port roles & servo calibration
A port is given a **role**: `ServoActuator`, `LedAnimator`, `DcMotor`, `BiDcMotor`
(H-bridge), `Heater`, or the input roles `RcPwmInput`/`SbusInput`/`JetiExInput`/
`JetiExTelemetry`. A servo's calibration/motion profile (set in the calibration
dialog) has: Min/Max endpoint (µs), Centre (µs), Max speed/acceleration/jerk, and
Reversed. Effects command a servo by intent (deploy/retract or 0–100%); the role
maps that onto these calibrated limits. Defaults come from the servo dialog, not a
fixed number.

### Expanders
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Alias | friendly name for a connected expander | text (e.g. `gear1`) | — |
| GUID | the board's hardware id | read-only | — |

---

## Engine tab (EngineFX)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Enabled | master on/off for engine sound | on / off | **off** |
| Type | engine character | `Turbine` / `Radial (planned)` / `Diesel (planned)` | `Turbine` |
| Speaker output | which channels carry the sound | `Stereo` / `Left` / `Right` | `Stereo` |
| Channel (toggle input) | RC channel that starts/stops | a named channel | unset |
| Threshold | pulse above which the engine is ON | within the RC range | `1500µs` |
| Hysteresis | dead-band around the threshold | small µs value | `50µs` |
| Failsafe | behaviour on signal loss | `Hold` / `Force OFF` / `Force ON` | `Force OFF` |
| Starting / Running / Stopping sound | WAV files on SD (Running is required) | file paths | empty |
| Starting offset | delay before the start sound | ≥ 0 ms | `0ms` |
| Stopping offset | delay before the stop sound | ≥ 0 ms | `0ms` |
| Start fade-in | fade the running sound in | ≥ 0 ms | `0ms` |
| Stop fade-out | fade the running sound out | ≥ 0 ms | `0ms` |

---

## GunFX tab
Per gun:

### Trigger
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Name | label for the gun | text | empty |
| Trigger channel | RC channel that fires it | a named channel | unset |
| Trigger threshold | when the channel counts as firing | within the RC range | `1500µs` |
| Trigger hysteresis | dead-band | small µs value | `25µs` |

### Rate of fire (ROF)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| ROF selector channel | RC channel that picks a band (empty ⇒ always the first ROF) | a named channel | unset |
| ROF item: Name | label for this rate | text | `rof1` |
| ROF item: Band lo / hi | stick range that selects this rate (0 = unbounded); bands must not overlap | µs | `0` / `0` (unbounded) |
| ROF item: RPM | shots per minute | a number | `600` |
| ROF item: Sound path | per-shot WAV on SD | file path | empty |
| ROF item: Speaker output | which channel(s) the shot plays on | `Left` / `Right` / `Stereo` | `Stereo` |

### Muzzle flash
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Port | LED/PWM output for the flash | a free LED port | unassigned |
| Duration | how long the flash stays lit per shot | ms | `30ms` |
| Brightness | flash intensity | 0–100% | `100%` |

### Recoil
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Recoil enabled | adds a kick per shot | on / off | **on** |
| Strength / jerk | size of the kick on the aim servo | µs (0 = none) | `200µs` |
| Hold | how long the kick is held before settling | ms | `80ms` |

### Smoke — heater (warms the cartridge)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Port | PWM output to the heating element | a free port | unassigned |
| Voltage | element supply rail | mV | `6000mV` |
| Mode | always-on or pulsed | `continuous` / `cycle` | `continuous` |
| Cycle on / off | on and off durations when Mode = Cycle | ms | `5000ms` / `3000ms` |
| Activation channel | optional RC gate that enables the heater | a named channel | unset |
| Activation threshold / hysteresis | the gate's trip point + dead-band | µs | `1500µs` / `25µs` |

### Smoke — fan (pushes the smoke out)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Port | PWM output to the fan | a free port | unassigned |
| Voltage | fan supply rail | mV | `6000mV` |
| Mode | always-on or a per-shot ramp | `continuous` / `pulse` | `pulse` |
| Pulse duration | length of one fan pulse when Mode = Pulse | ms | `100ms` |

### Turret axes (Yaw, Pitch) — each axis
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Enabled | turns this axis on | on / off | **off** |
| Servo port | the servo for this axis | a free servo port | unassigned |
| Input channel | RC channel that aims it | a named channel | unset |
| Neutral | the centre/rest position | within the RC range | `1500µs` |

---

## Lighting tab (LightFX)
### Master
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Enabled | master on/off for lighting | on / off | **on** |
| Master brightness | scales every channel in every program | 0–100% | `100%` |

### LED channels (shared by all programs)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Name | label (e.g. `Nav Red`, `Strobe`) | text | empty |
| Port | the LED/PWM output it drives | a free LED port | unassigned |
| Default brightness | base level when a program doesn't override it | 0–100% | `0%` (off) |

### Program tracks & events
A program has one track per channel; each track is a timeline of light **events**.
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Track brightness | per-track level (overrides the channel default) | 0–100% | `100%` |
| Track loop | repeat the track's events | on / off | off |
| Event: Kind | the light behaviour | `on` / `off` / `flash` / `fade_in` / `fade_out` / `fading` / `beacon` | `on` |
| Event: Duration | how long an on/off/fade event lasts | ms | `0ms` |
| Event: Cycle | period of a repeating pattern (flash/fading/beacon) | ms | `0ms` |
| Event: Brightness | level for an `on`/peak | 0–100% | `100%` |
| Event: Min / Max | low/high of a `fading`/`beacon` oscillation | 0–100% | `0%` / `100%` |
| Event: Flash | bright-pulse share of a `beacon`/`flash` cycle | 0–100% | `50%` |

### Program selector (RC stick picks the program)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Selector enabled | turns RC program selection on | on / off | **off** |
| Input channel | the RC channel that selects | a named channel | unset |
| Hysteresis | dead-band at band edges | µs | `50µs` |
| Range: From / To + Program | a stick band and the program it activates | µs + a program name | — (operator adds) |

---

## Retractable / Landing lights
Per group (a searchlight). The servo Open/Close are clamped to the servo's
calibrated travel.
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Name | label for the group | text | `landing<id>` |
| Servo port(s) | the servo(s) that deploy/retract it | free servo ports | none |
| Open | deployed servo position | within the calibrated travel | `2500µs` (clamped) |
| Close | stowed servo position | within the calibrated travel | `500µs` (clamped) |
| LED port(s) + Brightness | searchlight LED(s) and level | free LED ports + 0–100% | none / set per LED |
| Fade-in | LED soft-start after deploy | ms (0 = instant) | `400ms` |
| Activation mode | how the group is triggered | `Manual` / `Input channel` / `Program` | `Manual` |
| Channel + threshold / hysteresis | the RC gate (mode = Input channel) | a named channel + µs | unset / `1500µs` / `50µs` |
| Program + When | the LightFX program that drives it (mode = Program) | a program name + `active` / `inactive` | empty / `active` |

---

## Gear / undercarriage tab (GearControl)
### Master
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Enabled | master on/off for the gear | on / off | **off** |
| Coordination | how the legs move together | `Independent` / `Full-sync` (legacy configs may also carry `door_sync` / `sequenced`) | `Independent` |
| Channel | RC channel for deploy/retract | a named channel | unset |
| Threshold | when "down" is commanded | within the RC range | `1500µs` |
| Hysteresis | dead-band | small µs value | `50µs` |
| Deploy on connection loss | emergency-deploy if the RC link drops | on / off | **off** |
| Deploy / Retract sound + Speaker output | optional transit WAVs and channel(s) | file paths + `Left`/`Right`/`Stereo` | empty / `Stereo` |

### Per strut (leg)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Name | label (e.g. `Main Left`, `Nose`) | text | `Main Left` / `Main Right` / `Front/Back` for the first three |
| Motor port | the H-bridge output that drives the leg | a free H-bridge port | unassigned |
| Deploy duty | motor power+direction while lowering | signed value (full = `20000`) | `20000` |
| Retract duty | motor power+direction while raising | signed value (full reverse = `-20000`) | `-20000` |
| Timeout | max run time before the motor force-stops (jam protection) | ms | `30000ms` |

### Stall guard (per strut, motor protection)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Mode | auto-detect vs. operator-calibrated threshold | `live` / `fixed` | `live` |
| Ratio | stall threshold as a multiple of baseline current (`250` = 2.5×) | a number (×100) | `250` (2.5×) |
| Sample / Window | the detector's averaging windows | ms | `200ms` / `80ms` |
| Threshold | absolute current limit | mA | `1000mA` |
| Ceiling | optional hard current limit (0 = none) | mA | `0` (disabled) |

### Doors (per strut, 0–2 servos)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Door port(s) | the gear-bay door servo(s) | free servo ports | none |
| Open / Close | door servo positions | within the calibrated travel | from the servo calibration |
| Door mode | how multiple doors move | `sync` / `delay` / `sequence` | `sync` |
| Door delay | the stagger when mode = delay | ms | `500ms` |
| Close policy | which doors close after the strut moves | `both` / `first` / `none` | `both` |

---

## Battery (on boards with a sensor)
| Setting | What it does | Options / range | Default |
|---|---|---|---|
| Cutoff voltage | the low-voltage warning/cutoff level | a voltage | depends on the pack |
| Chemistry / cell count | the pack type used to compute thresholds | the supported pack options | depends on the pack |

---

## Audio / Alerts
Sound files are chosen per effect (engine, gun ROF items, gear transit) as SD-card
paths, each with a speaker-routing choice (`Left` / `Right` / `Stereo`, default
`Stereo`). Some audio/alert settings live in the board's config file rather than a
Studio tab — if the operator asks about one that isn't shown in a tab, say so
rather than guessing.
