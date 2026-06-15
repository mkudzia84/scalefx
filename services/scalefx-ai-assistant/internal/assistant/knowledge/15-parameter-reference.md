# Parameter reference (per tab / feature)

This is the authoritative list of the settings ScaleFX Studio exposes, grouped by
the tab/effect where you set them. The descriptions are taken from Studio's own
in-app help text, so they match exactly what each control does. Each row gives the
setting, an accurate description (including its options), and its default.

**Rule for the assistant:** describe ONLY the settings, behaviours, options, and
defaults on THIS page (cross-referenced with the operator's live state). Do not
invent parameters, modes, ranges, or defaults, and do not embellish a setting's
behaviour beyond what's written here. If asked about something not listed, say
you're not certain it exists, name the closest real setting, and point the
operator at the tab — never guess. The live state shows the operator's CURRENT
values; this page gives the behaviour and the defaults.

Units: **µs** = servo/RC pulse width (1000–2000 µs is the normal RC range),
**ms** = milliseconds, **%** = 0–100 percent, **mV** = millivolts.

A recurring pattern — **RC channel gating** (used by engine on/off, gun trigger,
gun smoke activation, gear up/down, landing activation): you pick a *named*
channel (defined on the Input & Ports tab), and the effect triggers when that
channel's value rises past a **threshold**; the **hysteresis** is a dead-band
around the threshold that stops stick jitter from re-triggering. Leaving the
channel empty means manual control only.

---

## Input & Ports tab

| Setting | What it does | Default |
|---|---|---|
| Protocol | Input decoding mode for the receiver link, limited to the roles this port can host (`PPM` / `SBUS` / `Jeti EX Input`). | operator picks |
| Channel count | How many channels are decoded from this input (capped at the protocol's max). "Autodetect" sets it from the live signal. | from signal |
| Channel function | Names a channel so effects can reference it by name (defined in the Input & Ports `inputs[]` block). | unset |
| Port role | The hardware function attached to an output port: `ServoActuator`, `LedAnimator`, `DcMotor`, `BiDcMotor` (H-bridge), `Heater`, or the input roles. Output pickers only list ports with the role the effect needs. | unset |

**Servo calibration** (the ⚙ Calibrate… popup — live jog, set limits, edit speed /
accel / jerk): Min/Max endpoint µs, Centre µs, Max speed/accel/jerk, and a
**Reversed** toggle ("open" maps to min µs instead of max). Effects command a servo
by intent; the role maps that onto these calibrated limits.

---

## Engine tab (EngineFX)

| Setting | What it does | Default |
|---|---|---|
| Enable | Master on/off for EngineFX (press Apply to push). | off |
| Type | Engine character — `Turbine`, or `Radial`/`Diesel` (planned). | `Turbine` |
| Speaker output | Which audio channels carry the sound: `Stereo`, `Left`, or `Right`. | `Stereo` |
| On/off channel | The RC channel that starts/stops the engine (RC-gated; see threshold/hysteresis). Empty = manual. | unset |
| Threshold | Engine turns on when the channel rises past this pulse. | `1500µs` |
| Hysteresis | Dead-band around the threshold that prevents stick jitter from re-triggering. | `50µs` |
| Failsafe | Behaviour on RC signal loss: `Hold` last value, `Force OFF`, or `Force ON`. | `Force OFF` |
| Starting / Running / Stopping sound | WAV files on the SD card for ignition, the running loop (required), and shutdown. | empty |
| Starting offset | Delay before the starting sound begins after the engine switches on. | `0ms` |
| Stopping offset | Delay before the stopping sound begins after the engine switches off. | `0ms` |
| Start fade-in | Linear volume ramp from silent to full at the start of the engine sound. | `0ms` |
| Stop fade-out | Linear volume ramp from full to silent at shutdown. | `0ms` |

---

## GunFX tab (per gun, up to 4)

### Trigger
| Setting | What it does | Default |
|---|---|---|
| Name | Label for the gun. | empty |
| Trigger channel | The RC channel that fires the gun (RC-gated). | unset |
| Threshold / Hysteresis | Fire when the channel rises past the threshold; hysteresis is the dead-band. | `1500µs` / `25µs` |

### Rate of fire (ROF) — one or more rate items, selected by an RC channel
| Setting | What it does | Default |
|---|---|---|
| ROF selector channel | RC channel whose stick position arms one ROF item (each item arms when the channel falls within its band). Empty ⇒ always the first item. | unset |
| Band low / high µs | The selector-stick window (in µs) that arms this rate item. `0`/`0` = unbounded (covers any position). Items must not overlap. | `0` / `0` |
| Rate of fire | Firing cadence in rounds per minute when this item is armed. | `600` rpm |
| Sound | Per-shot WAV on the SD card. | empty |
| Speaker | Which channel(s) the shot sound plays on: `Left`, `Right`, or `Stereo`. | `Stereo` |

### Muzzle flash
| Setting | What it does | Default |
|---|---|---|
| LED port | The PWM port (with the LedAnimator role) that flashes per shot. | unassigned |
| Duration | How long the LED stays lit per shot. | `30ms` |
| Brightness | LED brightness during the flash. | `100%` |

### Recoil (an impulse added to the aim servos)
| Setting | What it does | Default |
|---|---|---|
| Recoil | Enables the per-shot recoil kick. | on |
| Jerk | Maximum random recoil kick: each shot, each axis gets a random ± offset up to this added to its output. | `200µs` |
| Hold | (Advanced) How long the recoil offset rides on the aim before the role de-jerks it back. | `80ms` |

### Smoke — heater (warms the cartridge)
| Setting | What it does | Default |
|---|---|---|
| Port | The PWM port (with the Heater role) driving the heating element. | unassigned |
| Element | Rated voltage of the heating element. Firmware scales duty against the port rail, so a 6 V element on an 8 V rail never sees more than its rated average. | `6000mV` |
| Mode | `continuous` = drive at element-scaled duty whenever activated. `cycle` = pulse on for the on-time then off for the off-time, repeating while smoke is armed (power saving / temperature limiting without a thermistor). | `continuous` |
| On / Off | The cycle on-phase and off-phase durations (Mode = cycle). | `5000ms` / `3000ms` |
| Activation channel | Optional RC gate that enables the heater (and fan); empty = always allowed when armed. | unset |
| Activation threshold / hysteresis | The gate's trip point and dead-band. | `1500µs` / `25µs` |

### Smoke — fan (pushes the smoke out)
| Setting | What it does | Default |
|---|---|---|
| Port | The PWM port (with the DcMotor role) driving the fan. | unassigned |
| Element | Rated voltage of the fan motor; firmware scales duty against the port rail. | `6000mV` |
| Mode | `continuous` = fan at 100% of element-rated voltage while firing and armed. `pulse` = sinusoidal envelope per shot — idles at 50% base, ramps to 100% then back to 50% over the pulse duration on each shot. | `pulse` |
| Duration | One sinusoid period (Mode = pulse): rises 50%→100% over the first half and back over the second. 100 ms matches 600 rpm so the envelope completes once per shot. | `100ms` |

### Turret axes (Yaw, Pitch) — each axis
| Setting | What it does | Default |
|---|---|---|
| Enabled | Turns this turret axis on. | off |
| Servo port | The servo that moves this axis. | unassigned |
| Channel | The RC channel that aims this axis (proportional). | unset |
| Neutral | The servo position used when the input channel isn't driving it. | `1500µs` |

---

## Lighting tab (LightFX)

### Master
| Setting | What it does | Default |
|---|---|---|
| Enable | Master on/off for LightFX. | on |
| Master brightness | 0–100% applied as an additional multiplier on every channel — a global dimmer. | `100%` |

### LED channels (shared by all programs)
| Setting | What it does | Default |
|---|---|---|
| Name | Channel name that programs reference. | empty |
| Port | An unclaimed PWM port with the led-animator role. | unassigned |

### Program selector (an RC stick picks the active program)
| Setting | What it does | Default |
|---|---|---|
| Selector channel | The named RC channel; each band below arms when the channel value falls within it. | unset |
| Hysteresis | Dead-band at band edges to prevent chatter. | `50µs` |
| Band low / high µs + Program | A stick window and the program it activates. | operator adds |

### Per-program tracks (one per channel) and their events
| Setting | What it does | Default |
|---|---|---|
| Drive (per track) | Whether this program drives this channel; unchecking mutes the channel for this program. | — |
| Track brightness | Per-program brightness scale for this channel (0 = off). | `100%` |
| Loop | Phase-locked repeating cycle (period = the sum of the events' durations). | off |

An event timeline runs in order. Event **kind** and the fields that apply to it:
| Kind | What it renders |
|---|---|
| `on` | Constant on at the event's brightness (duration 0 = hold indefinitely). |
| `off` | Off for the event's duration. |
| `fade_in` | Linear ramp from 0 to brightness over the duration, then holds on. |
| `fade_out` | Linear ramp from brightness to 0 over the duration, ends off. |
| `flash` | Square-wave on/off at the cycle period; the flash % is the on-share of each cycle. |
| `fading` | Sinusoidal oscillation between min% and max% over the cycle period. |
| `beacon` | A brief bright pulse (flash % of the cycle) up to max%, dim at min% the rest of the cycle. |

Event fields: **duration** (ms; 0 = indefinite), **cycle** (ms period for flash/fading/beacon),
**brightness** (% for on/flash/fade), **min/max** (% for fading/beacon), **flash** (% pulse share for flash/beacon).
Defaults: kind `on`, duration `0`, cycle `0`, brightness `100%`, min `0%`, max `100%`, flash `50%`.

---

## Retractable / Landing lights (per group)

| Setting | What it does | Default |
|---|---|---|
| Name | Label for the group. | `landing<id>` |
| Deploy direction | Which calibrated servo end is the DEPLOYED position; retract goes to the other end. (Set by a toggle, not raw µs — the servo's calibration defines the travel.) | deploy → MAX end |
| Servo port(s) | The servo(s) that deploy/retract the light. | none |
| LED port(s) + Brightness | The searchlight LED(s) and their brightness % when deployed. | none / per LED |
| Fade-in | LED soft-start: ramp 0→brightness over this many ms once the servo is fully deployed (0 = hard on). | `400ms` |
| Activation mode | How the group is triggered: `Manual`, `Input channel` (RC-gated), or `Program` (follows a LightFX program). | `Manual` |
| Deploy channel + threshold / hysteresis | The RC gate when mode = Input channel. | unset / `1500µs` / `50µs` |
| Program + When | The LightFX program that drives it, and whether to deploy when that program is `active` or `inactive` (mode = Program). | empty / `active` |

---

## Gear / undercarriage tab (GearControl)

### Master
| Setting | What it does | Default |
|---|---|---|
| Enable | Master on/off for the whole undercarriage (radio + control). | off |
| Coordination | `Independent` = each strut deploys/retracts on its own, no cross-strut sync. `Full-sync` = all struts move in lockstep: doors open together, then struts run together, then doors close together. | `Independent` |
| Up/down channel | The RC channel that deploys/retracts the gear (RC-gated). | unset |
| Threshold / Hysteresis | Deploy when the channel rises past the threshold; hysteresis is the dead-band. | `1500µs` / `50µs` |
| On signal loss (deploy) | If an input LINK drops (e.g. the Jeti UART dies, not just the gear channel), the gear emergency-deploys. | off |
| Deploy / Retract sound + Speaker | Optional transit WAVs and the channel(s) they play on. | empty / `Stereo` |

### Per strut (leg)
| Setting | What it does | Default |
|---|---|---|
| Name | Label (e.g. `Main Left`, `Nose`). | first three: `Main Left` / `Main Right` / `Front/Back` |
| Motor port | The H-bridge port (BiDcMotor role) that drives the leg. | unassigned |
| Deploy direction | `Forward` = deploy runs the motor forward (retract reverses). `Reverse` = deploy runs in reverse. | `Forward` |
| Travel timeout | Full-travel watchdog: the endstop seek aborts to ERROR after this. 0 = seek until stall. | `30000ms` |

**Motor / stall guard** (set in the ⚙ Calibrate motor… popup — live drive, A→B
sweep that measures travel + stall current, stall-guard tuning):
| Setting | What it does | Default |
|---|---|---|
| Mode | `live` = trip when current spikes to the ratio × the measured running baseline. `fixed` = trip at an absolute current threshold. | `live` |
| Ratio | (live mode) Stall threshold as a multiple of baseline current — `250` means 2.5×. | `250` (2.5×) |
| Threshold | (fixed mode) Absolute stall current. | `1000mA` |
| Ceiling | Hard over-current cutoff regardless of mode; `0` = none. | `0` |

### Doors (per strut, 0–2 servos)
| Setting | What it does | Default |
|---|---|---|
| Door port(s) | The gear-bay door servo(s). | none |
| Door mode | `Together` = both doors open together. `Staggered` = door 1 opens, door 2 follows after a fixed delay. `One, then other` = door 1 opens fully (motion-done monitored), then door 2 starts. | `Together` |
| Stagger | (Staggered mode) Delay before door 2 starts opening. | `500ms` |
| After deploy (close policy) | `Both close` = both doors close once the gear is down. `One closes` = door 1 closes, door 2 stays open around the leg. `None close` = both stay open. | `Both close` |

---

## Battery (on boards with a sensor)

| Setting | What it does | Default |
|---|---|---|
| Cutoff voltage | The low-voltage warning/cutoff level. | depends on the pack |
| Chemistry / cell count | The pack type used to compute thresholds. | depends on the pack |

---

## Audio / Alerts

Sound files are chosen per effect (engine, gun ROF items, gear transit) as SD-card
paths, each with a speaker-routing choice (`Left` / `Right` / `Stereo`, default
`Stereo`). Some audio/alert settings live in the board's config file rather than a
Studio tab — if the operator asks about one that isn't shown in a tab, say so
rather than guessing.
