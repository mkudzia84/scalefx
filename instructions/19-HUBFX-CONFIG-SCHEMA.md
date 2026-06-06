# HubFX Config Schema + ConfigService — Plan

Spec for the HubFX configuration filesystem, the matching `HubFxConfig`
struct, and the `HubFxConfigServicePolicy` that loads it and wires every
effect service. Drafted 2026-05-20, revised against the policy stack:

- `LightFxEffectService`, `LandingLightService`, `GearControlService`,
  `EngineFxService`, `GunFxService`, `AlertService` (all `configure()`-driven).
- `InputDispatcherService` (binding table populated by effects, not config).
- `TopologyService`, `ExpanderService`, `StorageService`, `AudioService`
  (no static config — hardware/runtime-only).

Builds on `sfx_config`'s `ConfigSchema` concept + `ConfigStore<TSchema, TPool>` + `YamlParser`.

## Goals

1. **Per-controller system file + per-effect catalogs** (Rule 26 extended) —
   `/hubfx.yaml` is the system file. Catalog items (LED programs, alert
   severities, future sound packs) live in per-effect subdirectories and
   are referenced by name from the system file.
2. **All config on LittleFS** (small, atomic, fast to round-trip). Bulk
   media (WAVs) stays on SD where it already is.
3. **Explicit reference — no catalog auto-load.** Only the programs / packs
   `hubfx.yaml` names get parsed and configured; the rest stay on disk
   as a library.
4. **Hardware invariants stay in the sketch** — PCA9685 freq, INA226
   shunt, port descriptors, GPIO pin numbers.
5. **One reload path** rewires every effect from one typed `HubFxConfig`
   struct.
6. **Failure-tolerant** — bad file / missing reference ⇒ that section
   stays empty, WARN logged, board boots functional.
7. **Wire surface stays unchanged** — `CONFIG_RELOAD` / `CONFIG_SAVE` /
   `CONFIG_LOAD` packets from `sfx_config`; per-catalog file
   upload/list/delete via the existing storage wire surface.

## Filesystem layout

```
LittleFS (config — small, fast, round-trippable):
  /hubfx.yaml                    # system file: port mapping, channel alloc, refs
  /alerts.yaml                   # severity → sound enum, volumes
  /lightfx/programs/             # LED program library — one file per program
    ├── AllOn.yaml
    ├── BeaconStrobe.yaml
    └── GearTaxiDance.yaml
  /gunfx/presets/                # gun behaviour presets (timing, recoil, sound)
    ├── M2Browning.yaml
    ├── MG42.yaml
    └── Hispano20mm.yaml
  /enginefx/profiles/            # engine sound packs (future)
  /gearcontrol/profiles/         # gear motion presets (future)

SD (bulk media — WAV audio):
  /sounds/sys/                   # alert sounds (referenced by AlertSound enum)
    ├── init.wav
    ├── warning.wav
    ├── error.wav
    ├── critical.wav
    └── lightfx_detected.wav
  /sounds/engine/<pack>/         # per-pack: start.wav, idle.wav, stop.wav
  /sounds/guns/<preset>/         # per-preset: fire.wav, ...
```

Every per-effect subdirectory follows the same library-with-manifest
pattern: items live as files, the controller's system file
(`hubfx.yaml`) names the ones it uses, the rest stay on disk as a
library for `light-program preview` / future hot-swap workflows.

**Naming convention.** A catalog item's *filename minus extension is its
canonical name*. The YAML inside doesn't carry a redundant `name:` field
— the filesystem is the source of truth. References in `hubfx.yaml` use
the bare name (no path, no extension):

```yaml
lightfx:
  programs: [AllOn, BeaconStrobe]   # ⇒ /lightfx/programs/{AllOn,BeaconStrobe}.yaml
```

This rules out the entire "filename says X, content says Y" footgun.

## Port reference syntax

```yaml
port: { kind: pwm,     idx: 4 }                # hub-local PCA9685 channel 4
port: { kind: servo,   idx: 0 }                # hub-local servo header IN_2
port: { kind: input,   idx: 0 }                # hub-local IN_1 multi-modal input
port: { kind: hbridge, idx: 0, board: gear1 }  # on the expander aliased "gear1"
port: { kind: hbridge, idx: 0, guid:  GC01 }   # raw GUID fallback ("...-GC01")
```

Board addressing on a PortRef:

- **none** (`board`/`guid` both absent) → hub-local.
- **`board: <alias>`** (preferred) → resolved to the expander's GUID via the
  `expanders:` block in `/hubfx.yaml` (declared once; see below). An unknown
  alias logs a WARN and falls back to hub-local.
- **`guid: <4-hex>`** → raw hardware suffix, bypasses the alias table.

`board:` is resolved by `portRefFromNode` ([port_ref_yaml.h](../controllers/hubfx/esp32s3/src/config/port_ref_yaml.h))
against a module-level alias table seeded by `/hubfx.yaml`'s populate() — which
loads first, so every effect sub-file parsed afterward sees the table.

> **The hub's own GUID collapses to hub-local.** Studio stamps the hub's 4-hex
> GUID (e.g. `guid: 6D60`) on ports it writes into effect sub-files (its device
> model surfaces every port with its real GUID), but the topology/role-claim
> tables key hub ports as **local** (`guid == ""`). `portRefFromNode` therefore
> normalizes any PortRef addressed to the board's own GUID down to hub-local —
> otherwise the ref resolves to a *remote* port that no locally-attached role
> owns, and effect output silently goes nowhere. Expander GUIDs are never the
> hub's own, so only a raw `guid:<hub>` is affected.

## LightFx channel pool — `channels:` block in `/lightfx.yaml`

`/lightfx.yaml` declares an **instance-owned LED channel pool** (`schema_version: 2`):
each entry binds a channel `name` → physical `port` (+ optional
`default_brightness_pct`). Every program file references LED channels **by name**
(`tracks[].channel`), and the program loader resolves them against this pool FIRST
(`resolveLightFxChannelPooled`), falling back to `/hubfx.yaml` LedAnimator port
**labels** only on a miss. Define the channel once in Studio's *Channels* card;
the picker offers it to every program.

```yaml
channels:
  - name: Position lights
    port: { guid: 6D60, kind: pwm, idx: 0 }   # guid == hub ⇒ collapsed to local
    default_brightness_pct: 0
```

A pool entry that exists but has no port wired (`portKind == 0`) is a deliberate
miss — the track skips rather than steering at port 0. Without this block parsed,
v2 programs load **zero channels** (every track "channel not found"), so the
program list is empty and any RC `program_selector` finds no usable ranges and
goes dormant.

## Landing-light group — `lights[]` block in `/landing.yaml`

A landing light couples one or more servos (deploy/stow the searchlight) with one
or more LED bulbs. **Deploy order**: every servo drives to its **deployed end**, and
once ALL report `SERVO_TARGET_REACHED` the bulbs come on — optionally ramped via
`fade_in_ms` (a `[FadeIn, On]` LedAnimator queue; 0 = hard on). **Retract**: bulbs
off immediately, then servos drive to their **stowed end**.

> **`open_us` / `close_us` are DIRECTION ONLY** (since 2026-06-06). They no longer
> set the literal target µs — `LandingLight::deploy()`/`retract()` drive each servo
> via `SERVO_SET_INPUT_US` (0x55): a full-throw RC pulse (2000 = open end, 1000 =
> close end) that the **role maps onto its own LIVE calibrated `[min,max]`** (Rule
> 42/44). So the servo always travels the full calibrated throw even if these still
> hold a mid-range value, **and a later re-calibration is honoured automatically**
> (the role re-maps on the next deploy/retract — no re-save needed). `open_us >=
> close_us` ⇒ deploy → calibrated **MAX**-µs end (else the **MIN**-µs end). Physical
> wiring direction is the servo's own **REV** flag (orthogonal). The travel limits
> live in the servo's `/hubfx.yaml` `ports[].profile`, never here. Studio's "Deploy
> direction" toggle just flips these two so the operator never types µs.

```yaml
lights:
  - id: 0
    name: searchlight
    owner: landing-light            # who may setState(): lightfx|gunfx|gearcontrol|landing-light
    servos:
      - port: { kind: servo, idx: 0 }
    open_us:  1900
    close_us: 1100
    leds:
      - { port: { kind: pwm, idx: 5 }, brightness_pct: 80 }
    fade_in_ms: 400                  # LED soft-start after the servo deploys
    activation:                      # OPTIONAL RC-channel auto-deploy
      input: landing_deploy          # named channel from /hubfx.yaml inputs[]
      threshold_us: 1500             # ≥ ⇒ deploy, < ⇒ retract (fail-low on RC loss)
      hysteresis_us: 50
```

**Three activation paths** (a group uses whichever the operator wires):
- **Manual** — Studio Deploy/Retract or the wire `LL_SET_STATE`.
- **RC channel** — the `activation:` block above. Resolved + wired by the sketch's
  `LandingActivationDriver` (file-scope, re-installed on every CONFIG_RELOAD,
  mirrors the LightFx program selector + the EngineFx toggle). The firmware keys
  purely off a non-empty `activation.input`; Studio clears it for non-channel modes.
- **Program-attach** — the *program's* `landing_bindings: [{id, state}]` (NOT a
  `/landing.yaml` field). Studio's Landing panel "program" mode pushes `{id, on}`
  into the chosen program (same data the program editor's landing-binding rows
  edit), so the group deploys while that program is active and auto-retracts when
  the operator switches away — the existing program→landing path.

## Expander boards — `expanders:` block in `/hubfx.yaml`

The single place a remote board is declared. Each entry names a board
(`alias` → hardware `guid`) and lists its port → role attachments; the nested
ports are flattened into the master port table, each stamped with the board's
GUID.

```yaml
expanders:
  - alias: gear1                    # friendly handle effect files reference
    guid:  AB12                     # hardware 4-hex deviceName suffix (read via `init` / `expanders`)
    type:  gearcontrol              # optional sanity label — mismatch is WARN'd, not fatal
    ports:
      - { kind: hbridge, idx: 0, role: bi_dc_motor, label: "Nose retract" }
      - { kind: hbridge, idx: 1, role: bi_dc_motor, label: "Left main" }
```

**Attach timing (deferred).** Hub-local roles attach at boot. Expander roles
attach when the board reaches `Handshake::Ready` — fired via
`ExpanderService::onReady(...)`, since the outbound queue drops commands sent
before Ready. Because the mapping is **GUID-addressed, not slot-addressed**,
this also re-applies cleanly when a board reconnects on a different USB port.
A declared board that is offline logs `deferred (board offline)` and attaches
the moment it appears.

**CLI.** `topo-ports` lists every board's ports across the system and now
annotates each with its attached role (`pwm[0] → led-animator`); `topo-roles`
lists attachments per board; `role-attach` / `role-detach` bind at runtime.
Aliases live only in `/hubfx.yaml` — they are not on the wire; a GUI joins the
live `topo-ports` roster (by GUID) against the config file (alias → GUID).

## `/hubfx.yaml` — system file

```yaml
# Schema version — bump only when adding required fields.
schema_version: 1

# Global LightFX state.  `programs` is a list of program-filename
# references (relative to /lightfx/programs/).  Only these get loaded;
# other files in the directory stay on disk as a library.
lightfx:
  master_brightness_pct: 100
  programs:
    - AllOn
    - BeaconStrobe
    - GearTaxiDance

# Alerts config lives in a sibling file (defaults to /alerts.yaml).
# Uncomment to override.
# alerts_file: /alerts.yaml

# Landing lights — airframe-specific port wiring; stays inline.
landing_lights:
  - id: 0
    name: nose
    servo: { kind: servo, idx: 0 }
    leds:
      - { kind: pwm, idx: 4 }
      - { kind: pwm, idx: 5 }
    open_us: 1900
    close_us: 1100
    brightness_pct: 100
    owner: lightfx
  - id: 1
    name: left_main
    servo: { kind: servo, idx: 1 }
    leds: [ { kind: pwm, idx: 6 } ]
    open_us: 1900
    close_us: 1100
    brightness_pct: 90
    owner: gearcontrol

# Gears — airframe wiring; stays inline.
gears:
  - id: 0
    name: left_main
    motor: { kind: hbridge, idx: 0, guid: GC01 }
    leds: []
    deploy_duty:   20000
    retract_duty: -20000
    timeout_ms:   4000
  # ...

# Engine sound + RC throttle.
enginefx:
  enabled: true
  channel_a: 4
  channel_b: 5
  output_mask: 0x03
  rc_input:
    source:  { kind: input, idx: 0 }
    channel: 2
  threshold_us: 1500
  starting_path: /sounds/engine/start.wav
  running_path:  /sounds/engine/idle.wav
  stopping_path: /sounds/engine/stop.wav

# Guns — each entry pairs airframe wiring (ports + audio channel) with
# a behaviour preset (timing, recoil profile, sound).  `preset` is a
# filename reference under /gunfx/presets/ — see GunFx preset section.
gunfx:
  - id: 0
    name: main
    preset: M2Browning                           # → /gunfx/presets/M2Browning.yaml
    muzzle_flash: { kind: pwm,   idx: 7 }
    recoil_servo: { kind: servo, idx: 8 }
    smoke_heater: { kind: pwm,   idx: 6, guid: GC01 }
    trigger:
      source:  { kind: input, idx: 0 }
      channel: 4
    audio_channel: 2
    # Per-gun overrides — any preset field can be overridden inline.
    # Useful for "same gun, slightly faster fire rate" without forking
    # the preset.  Omit the `overrides:` key to use the preset verbatim.
    overrides:
      default_interval_ms: 80                    # 750 RPM instead of preset default

# Input port settings (mode selection per declared input).  Deferred to
# a follow-up — currently roles attach automatically based on what each
# effect's `trigger` ref expects.  See § "Open questions" below.
# inputs:
#   - port: { kind: input, idx: 0 }
#     mode: sbus                                  # ∈ {pulse, sbus, jeti_ex, uart_raw}
```

## `/lightfx/programs/<Name>.yaml` — per-program file

One file per program. Filename = canonical name. No `name:` field
inside — the file's location is its identity.

```yaml
# /lightfx/programs/BeaconStrobe.yaml
schema_version: 1
channels:
  - port: { kind: pwm, idx: 0 }
    brightness_pct: 100
    events:
      - kind: beacon
        cycle_ms: 1500
        min_pct: 0
        max_pct: 100
        flash_pct: 8
landing_bindings:
  - { id: 0, state: on }
```

## `/alerts.yaml` + `AlertSound` enum

Two-layer addressing:
- `AlertSound` is a **compile-time enum** in `sfx_audio` / alert policy,
  append-only. Each value names a WAV under `/sounds/sys/<snake_name>.wav`.
- `/alerts.yaml` maps severities to enum values + volume.

```yaml
# /alerts.yaml
schema_version: 1
enabled: true
channel: 0
severities:
  info:     { sound: init,             volume_pct: 70 }
  warning:  { sound: warning,          volume_pct: 80 }
  error:    { sound: error,            volume_pct: 90 }
  critical: { sound: critical,         volume_pct: 100 }
```

```cpp
// sfx_audio/audio/alert_sound.h — shared across controllers
namespace sfx_audio {

enum class AlertSound : uint8_t {
    None              = 0,    // sentinel — "no sound for this severity"
    Init              = 1,    // /sounds/sys/init.wav
    Warning           = 2,    // /sounds/sys/warning.wav
    Error             = 3,    // /sounds/sys/error.wav
    Critical          = 4,    // /sounds/sys/critical.wav
    LightFxDetected   = 5,
    LightFxFwError    = 6,
    GunFxFwError      = 7,
    GearMoving        = 8,
    // ... append-only (Rule 11)
};

/// snake_name ⇒ enum (used by YAML parser).
AlertSound alertSoundFromName(const char* name);
/// enum ⇒ snake_name (used by emit + diag).
const char* alertSoundName(AlertSound s);
/// enum ⇒ "/sounds/sys/<name>.wav".
const char* alertSoundPath(AlertSound s);

}  // namespace sfx_audio
```

**Boot validation.** AlertService walks the in-use enum values
(non-`None` entries in `_cfg.severities`) and checks the corresponding
file exists on SD. Missing files log a one-line WARN. Playing a sound
with a missing file returns false + WARN; doesn't crash.

**`AlertSeverityCfg` shape** (replaces the path-based form):

```cpp
struct AlertSeverityCfg {
    AlertSound sound      = AlertSound::None;
    uint8_t    volumePct  = 100;
};
```

The boot-up "system initialized" beep becomes `alerts.play(AlertSound::Init)`
— hardcoded in sketch, decoupled from severity mapping. Rename the
existing `hubfx_initialized.wav` → `init.wav` on the SD card.

## `/gunfx/presets/<Name>.yaml` — gun behaviour presets

A preset captures the *kind of weapon* — timing, recoil profile, sound
binding — independent of which airframe it's bolted to. The
airframe-specific part (port refs, audio channel) stays in `hubfx.yaml`.

> **✓ Landed (GunFX Phase 2, 2026-05-23).** The `GunDef` split shipped —
> `GunWiring` / `GunPreset` / `GunSpec` in
> [effects/gunfx/gunfx_config.h](../controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_config.h)
> + `gun_unit.h`, resolved/applied in
> [config/apply_hubfx_config.h](../controllers/hubfx/esp32s3/src/config/apply_hubfx_config.h).
> Wiring (port refs, audio channel) stays in `hubfx.yaml`; behaviour lives in the
> preset. The YAML below reflects the shipped shape.

```yaml
# /gunfx/presets/M2Browning.yaml
schema_version: 1
flash_duration_ms: 30
flash_brightness:  100

recoil_center_us:  1500
recoil_jerk_us:    200
recoil_hold_ms:    80

smoke_target_cx10: 1500                          # 150.0 °C while armed

trigger_threshold_us: 1500
default_interval_ms:  100                        # 600 RPM auto-fire

# Sound binding — either a single-file path or a sound-pack reference
# (future: /sounds/guns/<pack>/ enumeration).
fire_sound: /sounds/guns/m2_browning_fire.wav
output_mask: 0x03                                # AudioChannel::ALL
```

Inline `overrides:` in `hubfx.yaml` (see Guns example above) lets the
airframe tweak any preset field without forking the preset file — handy
when two guns share the same weapon profile but you want one to fire
faster than the other.

## Studio library — built-in templates + user catalog

Studio ships with a **built-in library** of programs, gun presets, and
(eventually) engine sound packs, and supports a **user library** on the
host PC where the operator can save their own customs and import
community-shared content. The on-device filesystem stays minimal — only
items actually referenced by `hubfx.yaml` get pushed to LittleFS.

```
Studio (host PC):
  <Studio install>/library/                       # built-in (read-only)
    lightfx/programs/
      ├── Beacon.yaml
      ├── Strobe.yaml
      ├── TaxiTriple.yaml
      ├── FormationRunway.yaml
      └── PoliceFlash.yaml
    gunfx/presets/
      ├── M2Browning.yaml
      ├── MG42.yaml
      ├── Hispano20mm.yaml
      ├── M61Vulcan.yaml
      └── BrowningHi-Power.yaml
    enginefx/profiles/
      ├── WarbirdRadial.yaml
      ├── JetTurbine.yaml
      └── ElectricMotor.yaml

  ~/AppData/Roaming/ScaleFX/library/              # user-editable
    lightfx/programs/
      └── MyCustomBeacon.yaml
    gunfx/presets/
      └── MyTwinPraxis.yaml
```

**Repo-side seed library** lives at [media/](../media/) — a checked-in
mirror of the built-in templates that the firmware can flash directly
to LittleFS (handy before Studio ships its built-in catalog).  Current
contents:

| Subdir | Files |
|---|---|
| `media/presets/hubfx/` | `helicopter_ka50.yaml`, `minimal.yaml` |
| `media/presets/lightfx/programs/` | `helicopter_off.yaml`, `helicopter_nav.yaml`, `helicopter_flight.yaml`, `helicopter_landing.yaml` (FAA Part 91 anti-collision + nav + landing) |
| `media/presets/alerts/` | `default.yaml` |
| `media/presets/landing/` | `helicopter_default.yaml` (Phase-3 shape — no live loader yet) |
| `media/presets/engines/` | `ka50_turbine.yaml` (Phase-3 shape — no live loader yet) |
| `media/sounds/sys/` | system alert chimes |
| `media/sounds/KA50/` | Klimov TV3-117VMA turbine (start / loop / stop) |
| `media/sounds/2A42/` | 2A42 30mm autocannon (200 / 550 rpm) |

See [media/README.md](../media/README.md) for the full catalog +
deployment notes.

### Library workflow

| Action | What it does |
|---|---|
| **Browse library** | Studio shows the built-in + user catalogs in a left-panel tree, grouped by effect (LightFx programs, GunFx presets, …). |
| **Drag preset into project** | Copies the file into the project's local model. Marks it for upload. Project still owns the file; library copy is the template. |
| **Save as preset** | Saves a project-local item to the user library so future projects can drag it in. |
| **Import bundle** | Drag a `.zip` of presets/programs onto Studio → unpacks into user library. Distribution format for community shares. |
| **Export bundle** | Selects N library items → zips them with a `manifest.yaml`. Same shape; round-trippable. |
| **Update built-in library** | Studio's installer / update step refreshes the built-in catalog. User library is untouched. |

### What's *in* the library vs the project

- **Library items** are *templates* — generic, airframe-agnostic.
  `Beacon.yaml` defines a generic 2 Hz beacon on `{pwm, 0}`; the
  project edits the port ref to point at the actual rail before
  uploading.
- **Project items** are *configured copies* — port refs filled in for
  the specific airframe, included in the upload set.
- **Studio doesn't enforce a hard separation** — drag from project →
  user library to promote a configured item back into the catalog (the
  port refs become defaults that the next user can re-aim).

### Built-in catalog growth

The built-in library evolves like any first-party asset — same MAJOR/
MINOR rules per item (Rule 9/10): adding a new preset is MINOR,
changing the schema of an existing preset is MAJOR. Studio surfaces
preset version compatibility in the import dialog.

### Per-effect catalog reuse

The library structure mirrors the per-effect filesystem namespace
exactly — Studio reads `lightfx/programs/*.yaml` from the library,
shows them in the LightFx tab; same for `gunfx/presets/`. When new
catalogs land (engine sound packs, gear motion presets), they slot in
by adding a directory — no Studio code change required beyond a tab.

## C++ schema struct

```cpp
// controllers/hubfx/esp32s3/src/config/hubfx_config.h
namespace hubfx::config {

constexpr uint8_t kMaxProgramRefs = 8;
constexpr size_t  kProgramNameMax = 32;

struct ProgramRef {
    char name[kProgramNameMax] = {};   // filename minus .yaml
};

struct HubFxConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    struct LightFxBlock {
        uint8_t      masterBrightnessPct = 100;
        ProgramRef   programs[kMaxProgramRefs] = {};
        uint8_t      numPrograms = 0;
        // Resolved at load time from /lightfx/programs/<name>.yaml.
        // Not serialized — populated by `loadProgramFiles()`.
        hubfx::effects::lightfx::Program loaded[kMaxProgramRefs] = {};
        uint8_t      numLoaded = 0;
    } lightfx;

    char alertsFile[64] = "/alerts.yaml";   // override-able

    struct LandingBlock {
        hubfx::effects::landing::LandingLightDef defs[hubfx::effects::landing::kMaxLandingLights] = {};
        uint8_t numDefs = 0;
    } landing;

    struct GearBlock {
        hubfx::effects::gearctrl::GearDef defs[hubfx::effects::gearctrl::kMaxGears] = {};
        uint8_t numDefs = 0;
    } gear;

    hubfx::effects::enginefx::EngineFxConfig engine;

    struct GunBlock {
        hubfx::effects::gunfx::GunDef defs[hubfx::effects::gunfx::kMaxGuns] = {};
        uint8_t numDefs = 0;
    } guns;

    hubfx::effects::alerts::AlertServiceConfig alerts;
};

class HubFxConfigSchema {
public:
    using DataType = HubFxConfig;
    static const char* defaultPath() { return "/hubfx.yaml"; }
    static bool populate(DataType& out, const sfx_config::YamlParser& yaml);
    static bool validate(const DataType& cfg, sfx_config::ValidationLog& errs);
    static bool emit    (const DataType& cfg, sfx_config::YamlEmitter& w);
    static void defaults(DataType& out);

    /// Resolve every ProgramRef into the matching Program struct by
    /// parsing /lightfx/programs/<name>.yaml.  Missing files: WARN +
    /// skip.  Called after `populate()` from the config service.
    static bool loadProgramFiles(DataType& out, FileSystem& fs);
    /// Same idea for /alerts.yaml.
    static bool loadAlertsFile (DataType& out, FileSystem& fs);
};

}  // namespace hubfx::config
```

## `HubFxConfigServicePolicy::applyConfig()` choreography

(unchanged from the previous draft, plus the file-resolution step)

```cpp
void applyConfig(const HubFxConfig& cfgIn) {
    HubFxConfig cfg = cfgIn;

    // 0. Resolve external file references.
    HubFxConfigSchema::loadProgramFiles(cfg, _fs);
    HubFxConfigSchema::loadAlertsFile  (cfg, _fs);

    // 1. Quiesce LightFx (LED_STOP per channel → FULL_OFF).
    _board->policy<LightFxEffectService>().controller().reset();

    // 2. Detach every previously-attached role on hub-local ports.
    _board->policy<HubFxTopologyService>().detachAllLocal();

    // 3. Hand each service its slice.
    _board->policy<LandingLightService>().configure(cfg.landing.defs, cfg.landing.numDefs);
    _board->policy<LightFxEffectService>().configure(cfg.lightfx.loaded, cfg.lightfx.numLoaded);
    _board->policy<LightFxEffectService>().controller().setMasterBrightness(cfg.lightfx.masterBrightnessPct);
    _board->policy<GearControlService>().configure(cfg.gear.defs, cfg.gear.numDefs);
    _board->policy<EngineFxService>().configure(cfg.engine);
    _board->policy<GunFxService>().configure(cfg.guns.defs, cfg.guns.numDefs);
    _board->policy<AlertService>().configure(cfg.alerts);

    // 4. Derive + attach roles for every port-ref.
    rebuildPortAttachments(cfg);
    // 5. Rebuild InputDispatcher bindings.
    rebuildInputBindings(cfg);
    // 6. Cache + commit.
    _cfg = cfg;
}
```

## GUI workflow + reload semantics — design opinion

The multi-file split is a strong fit for GUI editing, but adds three
new failure modes the system has to handle cleanly. Here's how I'd
structure it.

### What the split buys (vs single-file)

- **One artifact, one file**. Studio's program editor saves *one* small
  YAML, not a rewrite of the whole system config. Per-file mtime makes
  conflict detection trivial.
- **Library mental model.** `/lightfx/programs/` is a folder. Drag,
  rename, copy, delete — same model the user already has on their PC.
  No "config blob with 14 nested arrays" UI.
- **Per-file diff** for version control / two-Studio collaboration.
  One commit per program change, not whole-file rewrites.
- **Templates ship cheap.** Built-in `BeaconStrobe.yaml`,
  `LandingPattern.yaml` are just file fixtures Studio drops into
  `/lightfx/programs/` on first connect.
- **Faster iteration.** Edit one program → upload one file → reload
  one section. Don't churn the whole config.

### Three new failure modes — and how to handle them

#### 1. Dangling references

`hubfx.yaml` lists `BeaconStrobe` but `/lightfx/programs/BeaconStrobe.yaml`
doesn't exist (user deleted it; never uploaded; rename happened CLI-side).

- **Boot/reload:** WARN, skip that ref. Other programs still load.
- **Validate:** surface as `non-blocking warning` in `CONFIG_STATUS`
  → Studio shows `⚠ program 'BeaconStrobe' missing` in the program list.
- **Studio guard rail:** before pushing `hubfx.yaml`, verify each ref
  resolves against the local model. Catch dangling refs at edit time.

#### 2. Multi-file atomicity

Studio's "Save Project" might push several files + an updated
`hubfx.yaml`. If interrupted mid-upload, the partial state must be
safe.

- **Upload order:** referenced files first (programs, alerts), then
  `hubfx.yaml` last. If interrupted before `hubfx.yaml`, the board's
  active config is unchanged.
- **Atomic file write:** use the existing `upload → temp file →
  rename` pattern in `sfx_storage`. Half-written files never become
  visible.
- **No implicit reload on upload.** Files land on disk; the active
  in-memory config is untouched until an explicit `CONFIG_RELOAD`.
- **Reload as the commit point.** Studio's "Save & Apply" = "upload
  all changed files, then send `CONFIG_RELOAD`". If reload validation
  fails, the previous in-memory config keeps running.

#### 3. Hot reload mid-flight

`CONFIG_RELOAD` while the engine is running or a gear is mid-cycle
is risky. Three modes:

- **Safe (default):** reload is allowed only when no effect is
  active. Returns `RELOAD_BUSY` otherwise. Studio prompts the user
  "stop engine + retract gears, then retry."
- **Force:** caller acknowledges the risk; reload quiesces effects
  (engine stop, gears coast, LightFx reset) then applies.
- **Stage:** files written, reload deferred until next idle moment
  (manual idle signal from Studio, or auto-detected). Lower-priority
  feature; defer.

I'd ship just "safe" + "force" for v1. Stage is a nice-to-have.

### CLI / wire surface — per-catalog commands

Already-available primitives:
- `ls /lightfx/programs/`
- `download /lightfx/programs/BeaconStrobe.yaml local.yaml`
- `upload local.yaml /lightfx/programs/BeaconStrobe.yaml`
- `rm /lightfx/programs/BeaconStrobe.yaml`

New convenience verbs (thin wrappers over the storage commands +
config-policy hooks):

| Verb | What it does | Risk gate |
|---|---|---|
| `light-program list`             | enumerate `/lightfx/programs/*.yaml` | safe |
| `light-program upload <file>`    | upload to `/lightfx/programs/<name>.yaml` (name = filename) | safe — no reload |
| `light-program delete <name>`    | remove `/lightfx/programs/<name>.yaml` | safe — no reload (dangling refs WARN'd) |
| `light-program preview <name>`   | parse the file, push to LightFx as a transient program, activate it | live but doesn't touch flash |
| `config reload`                  | re-read all files, re-apply | safe — refuses if effects active |
| `config reload --force`          | as above, quiesce effects first | dangerous, explicit |
| `config status`                  | last load result, validation errors, dangling refs | safe |

`preview` is the killer feature for live editing: edit a program in
Studio → push it via `light-program preview` → see the LEDs react
immediately → save to flash when happy. No reload churn during the
edit loop.

### Studio cache model

Studio mirrors the board's filesystem locally:

- **On connect**: download `/hubfx.yaml`, `/alerts.yaml`, and every
  `/lightfx/programs/*.yaml` into Studio's in-memory model.
  Auto-hydrate per [Rule 26](../CLAUDE.md). Show "synced" status.
- **Edit in Studio**: mutates local model only; no upload until save.
- **Save (per-file)**: pushes one changed file; debounced
  (Rule 24). LED previews via `light-program preview` for live effect.
- **Save & Apply (project-wide)**: pushes every dirty file in
  dependency order (programs first, then `hubfx.yaml`), then
  `CONFIG_RELOAD`. Single transaction with a clear "applying…" UI.
- **External-change detection**: on reconnect or after a CLI session,
  re-download and diff. Flag locally-modified files that drifted.

### Schema versioning per file

Each file gets its own `schema_version`. Lets us evolve, e.g., LED
program format independently of system config. Append-only per
Rule 11. Reload rejects future-versioned files individually rather
than nuking the whole config.

## Implementation order

1. **`AlertSound` enum + helpers in `sfx_audio`**, rename WAVs on SD,
   refactor `AlertServiceConfig.AlertSeverityCfg` to take `AlertSound`
   instead of `path`. (Smallest first PR — proves the catalog pattern.)
2. **`/alerts.yaml` schema + loader + `applyAlerts()`** in a new
   `HubFxConfigServicePolicy` skeleton (no reload wire yet).
3. **`/hubfx.yaml` for inline sections** (landing/gears/engine/guns)
   + `applyConfig()` that wires them. Replaces the inline configure()
   calls in `setup()`.
4. **LightFx program-catalog**: `/lightfx/programs/<Name>.yaml` parser
   + ref resolution + `loadProgramFiles()` step.
5. **`rebuildPortAttachments()`** — derives roles from port refs,
   replaces the hand-coded `attachLedAnimator x8` loop.
6. **`rebuildInputBindings()`** — wires Engine + Gun triggers via the
   input dispatcher.
7. **Wire surface plug-in** — `CONFIG_RELOAD` + `light-program preview`
   wire commands.
8. **Studio HubFx tab** — per-section panels + program editor with
   `preview` button. Verifier + debounce per Rule 23/24.

## Validation rules

| Rule | Severity | Action |
|---|---|---|
| `schema_version` newer than firmware understands | ERROR | reject load; fall back to defaults |
| Port ref `kind` not in `{servo,pwm,hbridge,input}` | ERROR | reject |
| Hub-local port idx out of declared range | WARN  | strip ref |
| Program ref points to missing file | WARN  | skip program |
| Sound file referenced by AlertSound has no WAV on SD | WARN  | mark sound unavailable; `play()` returns false |
| Two effects claim the same port | WARN  | first-wins |
| `enginefx.channel_a == channel_b` | ERROR | reject |
| `landing_lights[*].id` duplicated | ERROR | reject section, keep others |
| `gears[*].id` duplicated | ERROR | reject section |

## Deferred work + open questions

### Deferred — own follow-up

1. **Input settings + wiring abstraction.** Per-declared-input role
   selection (`pulse|sbus|jeti_ex|uart_raw`) needs to be configurable
   independent of the consumers (Engine/Gun triggers). Planned shape:
   an `inputs:` section in `hubfx.yaml` listing each declared input
   port with its mode + UART params:
   ```yaml
   inputs:
     - port: { kind: input, idx: 0 }
       mode: sbus                               # ∈ {pulse, sbus, jeti_ex, uart_raw}
       # uart-raw extras (when mode == uart_raw):
       # baud: 100000
       # invert: true
       # half_duplex: false
   ```
   Drives the right `configureXxx()` call on `EspInputPort` during
   `rebuildPortAttachments`. Effects (Engine throttle, Gun trigger)
   then subscribe to a *sub-channel* on that already-configured port
   via the `InputDispatcher`. Tracked as a follow-up; the open API
   work covers both the YAML surface and the C++ side of
   wiring/role-attach.

2. **GunFx feature refactor — split `GunDef` into `GunWiring` +
   `GunPreset`.** Required for the preset pattern above.
   `GunFxServicePolicy::configure()` becomes `configure(wirings,
   presets)`; `HubFxConfigSchema::loadGunPresets()` resolves
   `/gunfx/presets/<name>.yaml` for each `preset:` ref. Tracked in
   `instructions/99-HW-TODO.md` → firmware side.

### Still open

3. **Sound file → AlertSound discovery** — should the AlertSound enum
   be pruned automatically based on SD-side presence (boot scan), or
   stay statically defined and just emit WARNs for missing files?
   I prefer static + WARN (compile-time-safe code).
4. **Per-file vs whole-file save in Studio** — single "Save All" or
   per-file save buttons? Probably both: per-file for fine-grained
   edits, "Save & Apply" for project-wide commit.
5. **Library distribution format** — `.zip` is the obvious choice for
   bundles, but Studio's existing `wails` build doesn't ship a zip
   library. Either add one or define a custom `.sfxbundle` format
   (just a tarball with a manifest).
