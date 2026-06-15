# Parameter reference (per tab / feature)

This is the authoritative list of the settings ScaleFX Studio actually exposes,
grouped by the tab/effect where you set them. Each entry is: the setting, what it
does, and its options or range.

**Rule for the assistant:** only describe settings, options, modes, and ranges
that appear on THIS page (cross-referenced with the operator's live state). Do not
invent parameters, extra modes, ranges, or defaults. If a setting the operator
asks about isn't here, say you're not certain it exists rather than guessing.

Units used below: **µs** = servo/RC pulse width (1000–2000 µs is the normal RC
range), **ms** = milliseconds, **%** = 0–100 percent.

---

## Input & Ports tab

### Input port (the RC receiver link)
| Setting | What it does | Options / range |
|---|---|---|
| Protocol | How the receiver connects | `PPM` / `SBUS` / `Jeti EX Input` (limited by what the port supports) |
| Channel count | How many RC channels to read | a number up to the protocol's max; can auto-expand from the live signal |

### Channel mapping (one per RC channel)
| Setting | What it does | Options / range |
|---|---|---|
| Function | What the channel drives | a named function (e.g. `engine_toggle`, `gun_trigger`, `gear_updown`, `gun_rof`, `light_program`, `light_brightness`, `gun_smoke`, `gun_yaw`, `gun_pitch`, `master_volume`) or a custom operator-chosen name |

Effects reference channels by this **name**, not by number.

### Port roles (what a hub output does)
A port is given a **role**. The role kinds are: `ServoActuator`, `LedAnimator`,
`DcMotor`, `BiDcMotor` (H-bridge), `Heater`, and the input roles `RcPwmInput`,
`SbusInput`, `JetiExInput`, `JetiExTelemetry`. Output pickers only offer ports
whose role matches what the effect needs.

### Servo calibration / motion profile (per servo port)
Edited via the servo calibration dialog (Rule 28/44). Fields:
| Setting | What it does |
|---|---|
| Min / Max endpoint (µs) | the two travel limits the servo will reach |
| Centre (µs) | the neutral/centre pulse |
| Max speed / acceleration / jerk | how fast and how smoothly it moves |
| Reversed | flips the direction of travel |

Effects command a servo by intent (deploy/retract or a 0–100% position); the role
maps that onto these calibrated limits, so re-calibrating is honoured immediately.

### Expanders
| Setting | What it does | Options / range |
|---|---|---|
| Alias | a friendly name for a connected expander board | text (e.g. `gear1`) |
| GUID | the board's hardware id | shown read-only; effects can address a board by alias |

---

## Engine tab (EngineFX)
| Setting | What it does | Options / range |
|---|---|---|
| Enabled | master on/off for engine sound | on / off |
| Type | engine character label | `Turbine` / `Radial` / `Diesel` |
| Speaker output | which channels carry the sound | `Stereo` / `Left` / `Right` |
| Channel (toggle input) | RC channel that starts/stops the engine | a named channel |
| Threshold (µs) | pulse above which the engine is ON | within the RC range |
| Hysteresis (µs) | dead-band around the threshold to stop chatter | small µs value |
| Failsafe | behaviour on signal loss | `Hold` / `Force OFF` / `Force ON` |
| Starting / Running / Stopping sound | WAV files on the SD card (Running is required) | file paths |
| Starting / Stopping offset (ms) | delay before each transition sound | ≥ 0 ms |
| Start fade-in / Stop fade-out (ms) | fade the running sound in/out | ≥ 0 ms |

---

## GunFX tab
Per gun:

### Trigger
| Setting | What it does | Options / range |
|---|---|---|
| Name | label for the gun | text |
| Trigger channel | RC channel that fires it | a named channel |
| Trigger threshold / hysteresis (µs) | when the channel counts as "firing", plus dead-band | within the RC range |

### Rate of fire (ROF)
| Setting | What it does | Options / range |
|---|---|---|
| ROF selector channel | RC channel that picks a rate band (empty ⇒ always the first ROF) | a named channel |
| ROF item: Name | label for this rate | text |
| ROF item: Band lo / hi (µs) | stick range that selects this rate (0 = unbounded); bands must NOT overlap | µs |
| ROF item: RPM | cadence in shots per minute | a number |
| ROF item: Sound path | per-shot WAV on the SD card | file path |
| ROF item: Speaker output | which channel(s) the shot plays on | `Left` / `Right` / `Stereo` |

### Muzzle flash
| Setting | What it does | Options / range |
|---|---|---|
| Port | the LED/PWM output for the flash | a free LED port |
| Duration (ms) | how long the flash stays lit per shot | ms |
| Brightness (%) | flash intensity | 0–100% |

### Recoil
| Setting | What it does | Options / range |
|---|---|---|
| Recoil enabled | adds a recoil kick on each shot | on / off |
| Strength / jerk (µs) | size of the kick on the aim servo | µs (0 = none) |
| Hold (ms) | how long the kick is held before settling back | ms |

### Smoke (heater + fan)
Heater (warms the smoke cartridge):
| Setting | What it does | Options / range |
|---|---|---|
| Port | PWM output to the heating element | a free port |
| Mode | `Continuous` (always on when armed) or `Cycle` (pulse on/off) | `continuous` / `cycle` |
| Cycle on / off (ms) | on and off durations when Mode = Cycle | ms |
| Activation channel + threshold / hysteresis | optional RC gate that enables the heater | a named channel + µs |

Fan (pushes the smoke out):
| Setting | What it does | Options / range |
|---|---|---|
| Port | PWM output to the fan | a free port |
| Mode | `Continuous` or `Pulse` (a per-shot ramp up and down) | `continuous` / `pulse` |
| Pulse duration (ms) | length of one fan pulse when Mode = Pulse | ms |

### Turret axes (Yaw, Pitch) — each axis
| Setting | What it does | Options / range |
|---|---|---|
| Enabled | turns this axis on | on / off |
| Servo port | the servo for this axis | a free servo port |
| Input channel | RC channel that aims it | a named channel |
| Neutral (µs) | the centre/rest position | within the RC range |
| Motion profile | the servo calibration (see Input & Ports) | — |

---

## Lighting tab (LightFX)
### Master
| Setting | What it does | Options / range |
|---|---|---|
| Enabled | master on/off for lighting | on / off |
| Master brightness (%) | scales every channel in every program | 0–100% |

### LED channels (shared by all programs)
| Setting | What it does | Options / range |
|---|---|---|
| Name | label for the channel (e.g. `Nav Red`, `Strobe`) | text |
| Port | the LED/PWM output it drives | a free LED port |
| Default brightness (%) | base level when a program doesn't override it | 0–100% |

### Programs and their tracks
A program has one track per channel; each track is a timeline of light **events**.
| Event setting | What it does | Options / range |
|---|---|---|
| Kind | the light behaviour | `on` / `off` / `flash` / `fade_in` / `fade_out` / `fading` / `beacon` |
| Duration (ms) | how long an on/off/fade event lasts | ms |
| Cycle (ms) | the period of a repeating pattern (flash/fading/beacon) | ms (> 0) |
| Brightness (%) | level for an `on`/peak | 0–100% |
| Min / Max (%) | the low/high of a `fading`/`beacon` oscillation | 0–100% |
| Flash (%) | the bright-pulse share of a `beacon`/`flash` cycle | 0–100% |

### Program selector (RC stick picks the program)
| Setting | What it does | Options / range |
|---|---|---|
| Selector enabled | turns RC program selection on | on / off |
| Input channel | the RC channel that selects | a named channel |
| Hysteresis (µs) | dead-band at band edges | µs |
| Range: From / To (µs) + Program | a stick band and the program it activates | µs + a program name |

---

## Retractable / Landing lights
Per group (a searchlight):
| Setting | What it does | Options / range |
|---|---|---|
| Name | label for the group | text |
| Servo port(s) | the servo(s) that deploy/retract it (one or more) | free servo ports |
| Open / Close (µs) | the deployed and stowed servo positions | within the servo's calibrated travel |
| LED port(s) + Brightness (%) | the searchlight LED(s) and their level | free LED ports + 0–100% |
| Fade-in (ms) | soft-start of the LEDs after deploy | ms (0 = instant) |
| Activation mode | how the group is triggered | `Manual` / `Input channel` / `Program` |
| Channel + threshold / hysteresis | the RC gate (when mode = Input channel) | a named channel + µs |
| Program + When | the LightFX program that drives it, active or inactive (when mode = Program) | a program name + `active` / `inactive` |

---

## Gear / undercarriage tab (GearControl)
### Master
| Setting | What it does | Options / range |
|---|---|---|
| Enabled | master on/off for the gear | on / off |
| Coordination | how the legs move together | `Independent` (each leg separately) / `Full-sync` (all in lock-step) |
| Channel | RC channel for deploy/retract | a named channel |
| Threshold / Hysteresis (µs) | when "up" vs "down" is commanded, plus dead-band | within the RC range |
| Deploy on connection loss | emergency-deploy if the RC link drops | on / off |
| Deploy / Retract sound + Speaker output | optional transit WAVs and their channel(s) | file paths + `Left`/`Right`/`Stereo` |

### Per strut (leg)
| Setting | What it does | Options / range |
|---|---|---|
| Name | label for the leg (e.g. `Main Left`, `Nose`) | text |
| Motor port | the H-bridge output that drives the leg | a free H-bridge port |
| Deploy / Retract duty (%) | motor power and direction for each way | a signed percentage |
| Timeout (ms) | max run time before the motor is force-stopped (jam protection) | ms |

### Stall guard (per strut, motor protection)
| Setting | What it does | Options / range |
|---|---|---|
| Mode | `Live` (firmware auto-detects a stall) or `Fixed` (operator-calibrated threshold) | `live` / `fixed` |
| Ratio | stall threshold as a multiple of the baseline current | a number |
| Sample / Window (ms) | the detector's averaging windows | ms |
| Threshold / Ceiling (mA) | absolute current limits | mA |

### Doors (per strut, 0–2 servos)
| Setting | What it does | Options / range |
|---|---|---|
| Door port(s) | the gear-bay door servo(s) | free servo ports |
| Open / Close (µs) | the door servo positions | within the servo's calibrated travel |
| Door mode | how multiple doors move | `sync` (together) / `delay` (staggered) / `sequence` (one then the other) |
| Door delay (ms) | the stagger when mode = delay | ms |
| Close policy | which doors close after the strut moves | `both` / `first` / `none` |

---

## Battery (on boards that have a sensor)
| Setting | What it does | Options / range |
|---|---|---|
| Cutoff voltage | the low-voltage warning/cutoff level | a voltage |
| Chemistry / cell count | the pack type used to compute thresholds | the supported pack options |

---

## Audio / Alerts
Sound files are chosen per effect (engine, gun ROF items, gear transit) as SD-card
paths, each with a speaker-routing choice (`Left` / `Right` / `Stereo`). Some audio
and alert settings live in the board's config file rather than a Studio tab — if
the operator asks about one that isn't shown in a tab, say so rather than guessing.
