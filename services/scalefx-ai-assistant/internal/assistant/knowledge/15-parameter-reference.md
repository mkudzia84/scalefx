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
| Protocol | Input decoding mode for this port, limited to the roles it can host (`PPM` / `SBUS` / `Jeti EX Input` / `ESC Telemetry`). | operator picks |
| ESC (Telemetry sub-tab, per esc-telemetry port) | Which ESC's native telemetry stream the port listens to: `Kontronik (Kolibri/Kosmik/JIVE Pro)`, `Scorpion Tribunus (Unsc Telem)`, `Hobbywing Platinum V4 / FlyFun`, or `Hobbywing Platinum V5`. The ESC's sensors (RPM, voltage, current, used mAh, throttle, temperatures, BEC, faults) appear on the Telemetry tab and are served to the Jeti radio automatically; ESC fault bits are also pushed to the radio as warning/error messages. Configured on the Input & Ports → Telemetry sub-tab. | `Kontronik` |
| Motor poles (Telemetry sub-tab, per esc-telemetry port) | The motor's magnet/pole count (even, 2–100). Kontronik ESCs transmit ELECTRICAL rpm = shaft rpm × poles/2, so the published RPM divides by the pole pairs. An outrunner's pole count is its magnet count (e.g. 10). | `2` (no correction) |
| Gear ratio (Telemetry sub-tab, per esc-telemetry port) | Gearbox ratio motor:output (0.01–99). Published RPM = transmitted ÷ (poles/2 × gear ratio) — the output-shaft (head/prop) speed reaches the radio and the Telemetry tab. | `1` (direct drive) |
| Channel count | How many channels are decoded from this input (capped at the protocol's max). "Autodetect" sets it from the live signal. | from signal |
| Channel function | Names a channel so effects can reference it by name (defined in the Input & Ports `inputs[]` block). | unset |
| Port role | The hardware function attached to an output port: `ServoActuator`, `LedAnimator`, `DcMotor`, `BiDcMotor` (H-bridge), `Heater`, or the input roles. Output pickers only list ports with the role the effect needs. | unset |

### Servo calibration window (the ⚙ Calibrate… popup)

Opens for any servo port (gun turret, gear doors, landing, IO tab). It jogs the
servo live and saves a motion profile to `/hubfx.yaml` (persisted immediately on
Save). Since 2.46.0 the profile is a pure MOTION ENVELOPE — min/max are hard
caps and speed/accel/jerk shape every move. Named positions (door open/close,
strut deploy/retract, landing open/close) are separate ABSOLUTE µs values set
per effect with the ◧ Positions window.

| Control | What it does | Default |
|---|---|---|
| Live jog (slider / ± buttons / arrow keys) | Moves the servo across the calibration envelope; the big readout shows the current µs (arrows ±1 µs, Shift ×10; PageUp/Down ±50 µs). Jogging never changes the saved limits — sweep freely to explore; only **Set as min/max** (or typing in the fields) changes them. | — |
| Set as min / center / max | Captures the current jog position as that endpoint (center doubles as the neutral / failsafe position). | — |
| Min / Center / Max µs | The two travel limits and the neutral, as fine numeric tweaks. | from the dialog |
| Speed µs/s | Max slew rate; `0` = unlimited (snap straight to target). | from the dialog |
| Accel µs/s² | Ramp accel/decel (symmetric); `0` = full speed instantly. | from the dialog |
| Jerk µs/s³ | S-curve smoothing; `0` = a plain trapezoidal profile (most servos don't need jerk). | from the dialog |
| Save / Cancel | Save pushes the profile to `/hubfx.yaml`; Cancel restores the original. | — |

The Travel summary shows max − min µs and the time to cross it at the chosen speed.

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
| Starting offset | For the case where the engine is switched back ON while it is still shutting down (on → off → on). The starting sound then resumes this many ms INTO the track instead of replaying the full ignition from the beginning, so the re-start sounds continuous. A normal cold start (from fully stopped) plays from the start regardless. It is NOT a delay before the sound. | `0ms` |
| Stopping offset | For the case where the engine is switched OFF while it is still starting up (off → on → off). The stopping sound then begins this many ms INTO the track instead of from the start. It is NOT a delay before the sound. | `0ms` |
| Start fade-in | Linear volume ramp from silent to full at the start of the engine sound — applied on a COLD start only (from fully stopped). A warm re-start during shutdown comes in at full level with no fade. | `0ms` |
| Stop fade-out | Linear volume ramp from full to silent over the tail of the stopping sound. | `0ms` |

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

Adding a program from **+ Template…** auto-creates any channels the
template references that don't exist yet, mapping each onto the next
unclaimed led-animator port and naming it after the template's channel
(e.g. "Red beacon"). Existing channels are never changed; if there are
more template channels than free ports, the extras are created with no
port and the Channels card shows a yellow warning until one is picked.

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
| Positions (◧ Positions… window) | The group's OPEN and CLOSED positions as ABSOLUTE µs (2.46.0 — no direction flags anywhere: which number is bigger IS the direction). Live-jog the servo inside its calibrated range, "Set as Open" / "Set as Closed", Save — persisted to /landing.yaml immediately. Unset positions default to the servo's calibrated max (open) / min (closed). | open → cal. max · closed → cal. min |
| Servo port(s) | The servo(s) that deploy/retract the light. | none |
| LED port(s) + Brightness | The searchlight LED(s) and their brightness % when deployed. | none / per LED |
| Fade-in | LED soft-start: ramp 0→brightness over this many ms once the servo is fully deployed (0 = hard on). | `400ms` |
| Activation mode | How the group is triggered: `Manual` (wire command only), `Input channel` (RC-gated), or `Program` (tied to a LightFX program). | `Manual` |
| Deploy channel + threshold / hysteresis | The RC gate when mode = Input channel. | unset / `1500µs` / `50µs` |
| Program (+ When) | Mode = Program binds the group ON while the chosen LightFX program is the active one (it deploys when that program runs). Firmware applies this as a static on/off binding when the program is selected — the `When active/inactive` choice is honored for the `active` case; there is no live "deploy while the program is inactive" gate. | empty / `active` |

---

## Gear / undercarriage tab (GearControl)

### Master
| Setting | What it does | Default |
|---|---|---|
| Enable | Master on/off for the whole undercarriage (radio + control). | off |
| Coordination | `Independent` = each strut deploys/retracts on its own, no cross-strut sync. `Full-sync` = all struts move in lockstep: doors open together, then struts run together, then doors close together. (Studio's toggle offers these two; the firmware also supports `door_sync` and `sequenced`, which only appear if an older config carries them.) | `Independent` |
| Up/down channel | The RC channel that deploys/retracts the gear (RC-gated). | unset |
| Threshold / Hysteresis | Deploy when the channel rises past the threshold; hysteresis is the dead-band. | `1500µs` / `50µs` |
| On signal loss (deploy) | If an input LINK drops (e.g. the Jeti UART dies, not just the gear channel), the gear emergency-deploys. | off |
| Deploy / Retract sound + Speaker | Optional transit WAVs and the channel(s) they play on. | empty / `Stereo` |

### Strut drive (whole undercarriage — one selector)
| Setting | What it does | Default |
|---|---|---|
| Strut drive | How the strut stage moves. `H-bridge motor` = custom DC motor per strut (BiDcMotor role, endstop detected by current). `Servo per strut` = each strut is an integrated/3rd-party retract controller on its OWN servo (PWM) channel. `Servo shared` = ONE servo channel drives the whole undercarriage. In both servo modes the controller is a black box: the sequencer commands the pulse and waits a fixed **Travel time** before the door stage engages — there is no feedback. | `H-bridge motor` |
| Shared channel (Servo shared only) | The single servo port wired to the undercarriage controller. | unassigned |
| Shared travel time (Servo shared only) | Fixed stroke duration for the whole set — time a full stroke and add margin. | `5000ms` (min 500) |

### Per strut (leg)
| Setting | What it does | Default |
|---|---|---|
| Name | Label (e.g. `Main Left`, `Nose`). | first three: `Main Left` / `Main Right` / `Front/Back` |
| Motor port (H-bridge mode) | The H-bridge port (BiDcMotor role) that drives the leg. | unassigned |
| Strut channel (Servo-per-strut mode) | This strut's own servo (PWM) channel to its integrated retract controller. | unassigned |
| Travel time (Servo-per-strut mode) | Fixed stroke duration — the doors engage only after it elapses. | `5000ms` (min 500) |
| Deploy direction (H-bridge mode only) | `Forward` = deploy runs the motor forward (retract reverses). Servo-driven struts use the ◧ Positions window instead: DEPLOY and RETRACT are absolute µs you capture by jogging — no direction flag. | `Forward` |
| Positions (◧ Positions… window, servo strut modes) | The strut's DEPLOY and RETRACT pulse positions as ABSOLUTE µs. Live-jog, capture each, Save — persisted immediately. Shared mode has ONE pair for the whole undercarriage. Unset = calibrated max (deploy) / min (retract). | deploy → cal. max · retract → cal. min |
| Travel timeout (H-bridge mode) | Full-travel watchdog: the endstop seek aborts to ERROR after this. 0 = seek until stall. | `30000ms` |

### Motor calibration window (the ⚙ Calibrate motor… popup)

Opens for a strut's H-bridge motor (BiDcMotor). A gear motor has no position
feedback, so this drives it end-to-end and watches the current to measure travel
and tune the stall guard.

| Control | What it does | Default |
|---|---|---|
| Live status (~2 Hz) | Shows duty, voltage, **current (mA)** (the stall-current anchor), position, stall state, and the active voltage cap (V-cap) while you drive. | — |
| Motor V | The motor DRIVE voltage — every move (deploy, retract, calibration sweep, jog) delivers exactly this average at the motor, on any pack, battery-sag compensated (the firmware drives full-scale PWM capped against the live per-motor supply reading). Set it to what the mechanism was tuned for — e.g. nominal-6 V retracts often run at 9 V deliberately. Minimum 1 V (there is no "raw duty" mode any more). Saved to the strut as `motor_voltage_mv`. | `6 V` |
| Timeout (s) | Per-leg seek timeout; the seek aborts to TIMEOUT after this. | `30s` |
| Calibrate (A→B sweep) | Drives to end A then end B, measuring each leg's travel-time and peak current; the full-stroke time auto-suggests the strut's travel timeout (≈ stroke × 1.5). | — |
| To End A / To End B | Drive to one endstop (+duty / −duty) until stall or timeout. | — |
| Stop / Manual jog | Stop hard-brakes (always available); Manual jog drives raw while held (no stall guard). | — |
| Save to strut | Applies the drive voltage + travel timeout + stall guard to the strut (persisted on the next Apply). | — |

**Stall guard** (tune here; "Apply guard" pushes it live):
| Control | What it does | Default |
|---|---|---|
| Mode | `LiveRatio` = averages running current per stroke and trips on a ratio spike (no per-motor threshold, battery-voltage independent — recommended). `Fixed` = trips when \|current\| exceeds a known mA threshold. | `LiveRatio` |
| Ratio × | (LiveRatio) Trip at this multiple of the trailing-minimum running current — `2.5` (stored as `250`). | `2.5×` |
| Sample ms | (LiveRatio) The baseline-sampling window after the start inrush. | `200ms` |
| Ceiling mA | (LiveRatio) An absolute over-current backstop (≈ 80% of the stall peak); `0` = off. | `0` |
| Threshold mA | (Fixed) The absolute trip current. | `1000mA` |
| Confirm ms | (both modes) How long the current must stay over the trip point before a stall is declared (debounce). | `80ms` |

### Doors (per strut, 0–2 servos)
| Setting | What it does | Default |
|---|---|---|
| Door port(s) | The gear-bay door servo(s). | none |
| Door mode | `Together` = both doors open together. `Staggered` = door 1 opens, door 2 follows after a fixed delay. `One, then other` = door 1 opens fully (motion-done monitored), then door 2 starts. | `Together` |
| Stagger | (Staggered mode) Delay before door 2 starts opening. | `500ms` |
| After deploy (close policy) | `Both close` = both doors close once the gear is down. `One closes` = door 1 closes, door 2 stays open around the leg. `None close` = both stay open. | `Both close` |

### Manual / maintenance (per strut + fleet, in the Gear tab)
For setup/checkout, each strut card has a **Manual / maintenance** section that
drives the **doors** and the **strut** independently of the coordinated
deploy/retract sequence (there are also fleet "all doors / all struts" buttons):
| Control | What it does |
|---|---|
| Open doors / Close doors | Move just this strut's doors (honours the door-mode). |
| Strut down / Strut up | Move just this strut's motor to its endstop. |

These respect **safety interlocks enforced in firmware** (the buttons also grey
out to match): you cannot **close the doors** unless the strut is **up**
(retracted), and you cannot **move the strut** (either way) unless the doors are
**open** — the leg passes through the door gap. A manual command is refused while
a coordinated cycle is running. The interlocks are firmware-enforced, so they
also apply to the Console (`gear-doors` / `gear-strut`) — the GUI gate just
mirrors them.

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

The amplifier's analog gain is NOT a setting: since firmware 2.42.0 the codec
measures its own supply rail (PVDD) at every boot and picks the gain to match —
the old `codec_supply` config key was removed and is ignored if present in an
old file. There is nothing to configure; changing the battery pack is enough.

### Audio Power (read-only card on the Firmware tab, firmware ≥ 2.42.0)

| Field | Meaning |
| --- | --- |
| Codec | Amplifier model driving the speakers; the die id identifies the silicon variant. |
| PVDD rail | The amplifier supply voltage, measured live by the chip's own ADC. |
| Analog gain | The attenuation the codec auto-picked so full-scale output fits the measured rail. |
| Output level | Peak of the mixed audio output since the last refresh (updates ~every 2 s). |
| Est. power | Estimated speaker power at that peak, with a `4 Ω` / `8 Ω` toggle for the speaker impedance (default 4 Ω — the ScaleFX BOM; the choice is remembered). An estimate computed from levels and gains, not a measurement — speaker impedance cannot be autodetected on this hardware. |
| Faults | Only shown when the amplifier has latched a fault (clock / supply / over-current). |
