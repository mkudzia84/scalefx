# ScaleFX media library

Audio assets + YAML preset templates that pair with the HubFX firmware.
Tree:

```
media/
  sounds/                              # WAV assets — upload to /sounds/ on the board
    sys/                               # system alert chimes (/sounds/sys/*.wav)
    KA50/                              # KA-50 Kamov turbine
    2A42/                              # 2A42 30mm autocannon
  presets/                             # YAML config templates — drop into LittleFS
    hubfx/                             # /hubfx.yaml (thin master: features + audio)
    alerts/                            # /alerts.yaml severity maps
    engines/                           # /enginefx.yaml engine templates
    landing/                           # /landing.yaml landing-light defs
    lightfx/                           # /lightfx.yaml master settings (brightness + program list)
    lightfx/programs/                  # /lightfx/programs/<name>.yaml individual LED programs
```

The on-device layout mirrors this:

```
/hubfx.yaml                features enable matrix + audio.codec_supply
├── /alerts.yaml           severity → AlertSound + volumes
├── /enginefx.yaml         engine wiring + sounds
├── /landing.yaml          landing-light defs (servo + LED group)
└── /lightfx.yaml          master brightness + program path list
      └── /lightfx/programs/<n>.yaml   one file per LED program,
                                       referenced by FULL PATH from
                                       /lightfx.yaml.
```

`/hubfx.yaml` is the BOARD master: it describes the physical hardware
(codec PVDD rail, port → role attachment) and the master enable matrix.
Every effect's details live in its own canonical sub-file at a fixed
path — no path overrides, no implicit references; you simply drop the
matching file into the right slot.

The four top-level blocks of `/hubfx.yaml`:

- `audio:` — codec rail voltage (TAS5825P PVDD).
- `features:` — master kill-switch matrix.  Overrides each sub-file's local `enabled:` flag.
- `ports:` — per-port role attachment.  Maps each hub-local port (`{kind, idx}`) to a `RoleKind` variant (`led_animator`, `dc_motor`, `servo_actuator`, `rc_pwm_input`, `sbus_input`, `jeti_ex_input`, …) and a human-readable `label:`.  Omit the block ⇒ falls back to "LedAnimator on every PWM port" so a bare board still drives LEDs.
- `inputs:` — named RC channels.  Each entry binds a logical `name` (e.g. `throttle`, `engine_toggle`, `gear_switch`) to a `(port, id)` source and declares its `type:` (`boolean` / `enum_n` / `proportional_u8` / `proportional_s8` / `raw`) + the µs → typed-value mapping (`threshold_us`, `min_us` / `max_us`, `positions`, `failsafe:` `hold` / `force_low` / `force_high`).  Effects reference inputs by `name` only — threshold, hysteresis, range, and failsafe behaviour all live in this block, so swapping transmitter programming touches one file.

## Preset catalog

### Full board configs — `presets/hubfx/`

| File | Purpose |
| --- | --- |
| [helicopter_ka50.yaml](presets/hubfx/helicopter_ka50.yaml) | KA-50 reference build — full FAA lighting + KA-50 turbine. |
| [minimal.yaml](presets/hubfx/minimal.yaml) | Schema defaults (equivalent to "no file present"). |

### LightFx master — `presets/lightfx/`

| File | Upload as | Purpose |
| --- | --- | --- |
| [helicopter.yaml](presets/lightfx/helicopter.yaml) | `/lightfx.yaml` | Master brightness (60% — dimmed for low-light flying) + explicit path list of the four helicopter programs. |

### LightFx programs — `presets/lightfx/programs/`

All four programs follow the same 8-channel layout:

| ch | Light |
| --- | --- |
| 0 | Red rotating beacon (anti-collision) |
| 1 | White strobe (anti-collision) |
| 2 | Red position/nav light (port / left) |
| 3 | Green position/nav light (starboard / right) |
| 4 | White position/nav light (tail / aft) |
| 5 | Landing light (searchlight) |
| 6 | Cabin / cockpit light |
| 7 | Spare |

| Program | When to use |
| --- | --- |
| [helicopter_off](presets/lightfx/programs/helicopter_off.yaml) | **Default.** Rotors not turning, no FAA anti-collision required. |
| [helicopter_nav](presets/lightfx/programs/helicopter_nav.yaml) | Position lights only — taxi / static display. |
| [helicopter_flight](presets/lightfx/programs/helicopter_flight.yaml) | Full FAA flight config — beacon + strobe + nav.  Required whenever rotors are turning (FAR 91.209(b)). |
| [helicopter_landing](presets/lightfx/programs/helicopter_landing.yaml) | Flight + landing light deployed (servo + LEDs). |

The four programs are mutually exclusive — `light:program <name>` switches between them.  `helicopter_off` is listed first in `/lightfx.yaml` so a board with this catalog boots dark on the ramp.

### Alerts — `presets/alerts/`

| File | Purpose |
| --- | --- |
| [default.yaml](presets/alerts/default.yaml) | Mirrors `AlertsConfig` defaults — drop in to override per-severity volumes. |

### Landing lights — `presets/landing/`

| File | Upload as | Purpose |
| --- | --- | --- |
| [helicopter_default.yaml](presets/landing/helicopter_default.yaml) | `/landing.yaml` | Nose searchlight (servo on `IN_2`, LED on `pwm[5]`).  Loaded by `LandingConfigStore` → `LandingLightServicePolicy::configure()`. |

### Gear control — `presets/gearcontrol/`

| File | Upload as | Purpose |
| --- | --- | --- |
| [helicopter.yaml](presets/gearcontrol/helicopter.yaml) | `/gearcontrol.yaml` (on the HUB) | 3 retract gears (nose + 2 main), each a BiDcMotor + 2 status LEDs on a **GearControl expander** addressed by GUID.  Loaded by `GearControlConfigStore` → `GearControlServicePolicy::configure()`.  Replace the `guid:` placeholders with your expander's deviceName suffix. |

### Engines — `presets/engines/`

| File | Upload as | Reference airframe |
| --- | --- | --- |
| [ka50_turbine.yaml](presets/engines/ka50_turbine.yaml) | `/enginefx.yaml` | KA-50 Klimov TV3-117VMA — ~60 s startup spool, ~25 s shutdown wind-down.  Pairs with [media/sounds/KA50/](sounds/KA50/).  Loaded by `EngineFxConfigStore` → `EngineFxServicePolicy::configure()`. |

## Sound assets

### `sounds/sys/` — alert chimes

Firmware (see `alert_sound.h`) expects files at:

```
/sounds/sys/init.wav            AlertSound::Init
/sounds/sys/warning.wav         AlertSound::Warning
/sounds/sys/error.wav           AlertSound::Error
/sounds/sys/critical.wav        AlertSound::Critical
/sounds/sys/lightfx_detected.wav
/sounds/sys/lightfx_fw_error.wav
/sounds/sys/gunfx_fw_error.wav
/sounds/sys/gear_moving.wav
/sounds/sys/battery_low.wav
```

> Some legacy files in `media/sounds/sys/` (e.g. `gearctrl_detected.wav`, `lightfx_initialized.wav`) don't yet have an `AlertSound` enum entry — they're sample assets, not referenced by the firmware.  Add an enum entry + WAV pair to expose a new chime; missing paths log a one-line WARN and degrade to silent no-op.  See the [`AlertSound` enum](../controllers/hubfx/esp32s3/src/effects/alerts/alert_sound.h) for the canonical name table.

### `sounds/KA50/` — KA-50 Kamov twin-turbine

| File | Role |
| --- | --- |
| `engine_start.wav` | Spool-up sample (~60 s). |
| `engine_loop.wav` | Steady-running loop. |
| `engine_stop.wav` | Spool-down sample (~25 s). |

### `sounds/2A42/` — 2A42 30mm autocannon (KA-50, BMP-2, Mi-28)

| File | Role |
| --- | --- |
| `gun_200rpm.wav` | Low rate of fire. |
| `gun_550rpm.wav` | High rate of fire. |

## How to deploy presets

Upload via `scalefx-cli` (or Studio's file manager):

```
scalefx-cli config-save /hubfx.yaml < media/presets/hubfx/helicopter_ka50.yaml
scalefx-cli config-save /alerts.yaml < media/presets/alerts/default.yaml
# repeat for each /lightfx/programs/*.yaml
```

Then `config-reload` or reboot to apply.  Missing files always fall back to firmware schema defaults — a board with no presets uploaded still boots functional.

## YAML formatting

All files use the indented block-sequence style (Rule 27 in `CLAUDE.md`): sequence items 2 spaces under the parent key, continuations 4 spaces.  The firmware parser accepts compact-form too, but generators must emit the indented form to stay round-trip-clean through Studio.
