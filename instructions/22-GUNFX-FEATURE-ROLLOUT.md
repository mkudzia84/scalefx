# 22 — GunFX Feature Rollout: Audit + Execution Log

> **Status:** LANDED (2026-05-23). Phases 0–2.9 shipped (foundation + protocol +
> firmware + roles); Phases 3–4 (Go API + Studio `GunFxPanel.svelte`) shipped too
> — only the §8 polish items remain deferred. This file is now the **reference
> record** of the build: §0 architecture is still authoritative, §1 the pre-build
> audit, §7 the landed-status summary, §8 the open polish follow-ups.
>
> Companion to [21-STUDIO-ENGINEFX-PANEL.md](21-STUDIO-ENGINEFX-PANEL.md)
> (panel-design reference) and Rules 34/35/36
> ([.github/copilot-instructions.md](../.github/copilot-instructions.md)).

---

## 0. Architecture clarifications (read first)

These five constraints shape every phase below. Get them wrong and the
data model is wrong.

1. **A "gun" is a logical effect, not a board.** A gun is one entry in
   `/hubfx.yaml`'s `effects.guns:` list. Its ports — muzzle flash, recoil
   servo, smoke heater, smoke fan, yaw servo, pitch servo, trigger input,
   ROF selector input — are claimed from **any connected board**:
   HubFX, GunFX expander, GearControl, LightFX. The GunFX *board* is
   just a generic-port expander optimised for gun-shaped wiring; it has
   no gun-specific firmware (Step 1 of the generic-expander refactor is
   done — see `instructions/15-`). The HubFX runs every gun's state
   machine.

2. **Port pickers iterate the unified port table.** `Candidates(domain,
   slot)` already iterates `m.Ports` across every connected board with
   the right direction, role match, and exclusive-claim filter. The UI
   change is presentational only: group dropdown options by `boardName`
   so the operator can tell "HubFX CH3" from "GunFx-3C4D CH1".

3. **Multi-gun is a list, not a tabbed sub-view.** Up to 4 guns
   (`kMaxGuns=4`). The Studio panel renders one `.card` per gun stacked
   vertically inside the GunFX tab, with an `[+ Add gun]` button and a
   per-card `[× Remove]`. Don't accordion or tab them.

4. **Port voltage is per-port metadata, declared by each board.** HubFX
   CH1–8 are on the 8 V rail; HubFX servo headers are on 5 V; GearControl
   H-bridges are on the battery rail. The expander reports its port
   voltages via the existing `PORT_LIST_RESP` (Rule 11 append-only
   extension), the hub aggregates them, the device model exposes
   `Port.voltageMv`, and Studio shows them in the picker label. The
   firmware uses it for voltage-scaled PWM duty when driving sub-rail
   elements (a 5 V smoke heater on the 8 V rail wants ~63 % duty).

5. **"Simulate" = manual override / puppet mode + verbose status.** Not
   an animated UI preview, not a scripted hardware exercise. It's a
   manual control panel in Studio that drives every GunFx subsystem
   directly (yaw slider, pitch slider, ROF index picker, "hold fire"
   button, smoke toggle) — bypassing RC inputs while active. The
   firmware mirrors live per-element state in a verbose status
   broadcast so the panel always shows ground truth.

6. **Effects use a global tick / clock — never raw `millis()`.** A
   single `sfx_core::EffectClock` singleton latches `millis()` ONCE per
   main-loop pass and every effect (`tick()`, `update()`, motion
   profile, ROF scheduler, fan puff, heater bang-bang) reads `nowMs()` /
   `dtMs()` from it instead. Guarantees that two effects asking
   "what time is it?" in the same tick see the same answer (no off-by-N
   ms drift between subsystems), and gives motion profiles a stable
   `dt` for integration. Phase 0.5 introduces the clock + retrofits the
   existing EngineFx / GearControl / GunFx call sites; Phase 2's GunFx
   rewrite uses it throughout.

7. **No protocol back-compat window — this is greenfield.** All wire-
   format work in this rollout (PORT_LIST_RESP voltage field, the four
   GunFX manual/verbose packets, the GunFx config schema) lands as a
   single atomic firmware↔Go bump. NO Rule-11 append-only acrobatics
   for these new fields; no "legacy short payload" branches in the Go
   decoder. Pack the wire layout tidily — fixed-size structs where
   sensible, length-prefixed strings, no padding. Old hubs/expanders
   are flashed in lockstep when the new firmware version goes out.
   (Rule 11 still governs cross-version firmware compat for FIELDED
   units — i.e. once the rollout is shipped and tagged. This applies
   to in-flight refactor work only.)

---

## 1. Current state (audit summary)

Full audit details in the rollout discussion thread; this is the
compressed delta.

### 1.1 What exists ✅

| Layer | File | Notes |
|---|---|---|
| HubFX firmware service | [effects/gunfx/{gunfx_service.h, .ipp, gunfx_protocol.h, gun_unit.h, .ipp}](../controllers/hubfx/esp32s3/src/effects/gunfx/) | Registered in `hubfx_esp32s3.ino`; supports muzzle flash + recoil + heater + trigger; single-RPM auto-fire |
| Wire protocol | `0xCC–0xD2` (7 packets): FIRE_ONCE, START/STOP_FIRING(rpm), SMOKE_ARM, STATUS, SHOT_EVENT | Go mirror complete in [app/go/protocol/gunfx/gunfx.go](../app/go/protocol/gunfx/gunfx.go) |
| Roles | LedAnimator, ServoActuator, Heater (dual-mode), DcMotor, BiDcMotor, RcPwmInput/SBUS/Jeti | All present in [controllers/lib/sfx_board/roles/](../controllers/lib/sfx_board/roles/) |
| Device-model `DomainGun` | [app/go/devicemodel/types.go](../app/go/devicemodel/types.go) | Slots: `muzzle` (0–2 LedAnimator), `trigger` (0–1 input). Cap-gated on `CapGunFx`. |
| GunFx Pico expander | [controllers/gunfx/pico/](../controllers/gunfx/pico/) | Dumb expander: 3 PWM + 3 servo + battery sensing |
| Audio | 2-DAC-channel mixer, 8 mix tracks via `playAsync(channel, path, fadeIn, mask)` | Sufficient for per-shot samples |

### 1.2 Gaps vs the user spec ❌

| Required feature | Status | Plan reference |
|---|---|---|
| Multi-band ROF selection from a 2nd input channel | not in protocol/firmware/UI | Phase 1 config, Phase 2 arbitration, Phase 4 multi-band overlay |
| ROF items as add/remove list (per-item sound + RPM + band) | not in config | Phase 1 schema, Phase 4 UI |
| Non-overlapping band visualisation on the input bar | not in UI | Phase 4 (Rule 36 extension) |
| Smoke fan port separate from heater | only heater | Phase 1 schema, Phase 2 fan unit |
| Puffing fan synced to shots | not in firmware | Phase 2 |
| Voltage-scaled PWM (element vs port voltage) | not in firmware | Phase 0 (port voltage), Phase 2 (scaling formula) |
| Heater always-on vs bang-bang options | only closed-loop / open-loop | Phase 1 schema (`HeaterMode` enum), Phase 2 driver split |
| Nozzle flash from free LED ports | works via LedAnimator slot; UI gap | Phase 4 |
| Yaw + pitch servo subsystem | not in protocol/firmware/UI | Phase 1 slots + schema, Phase 2 dual-axis profile, Phase 4 UI |
| Per-axis profile (limits / speed / accel / jerk) | partial (ServoActuator has limits + velocity; no accel/jerk) | Phase 2 (extend `MotionProfile`) |
| Manual override / puppet mode | not in protocol/firmware/UI | Phase 1 packets, Phase 2 manual-mode plumbing, Phase 4 control panel |
| Verbose status broadcast | minimal status today | Phase 1 packet, Phase 2 producer, Phase 3 decoder, Phase 4 consumer |
| Studio panel for GunFx | placeholder | Phase 4 (full rebuild) |
| `app_gunfx.go` Wails bindings | absent | Phase 3 |
| Optional features with unclaimed-port surface (yellow warn) | UI gap | Phase 4 |

---

## 2. Phasing

Five sequenced phases. Each phase ships as ONE PR with tests + docs.
Phase N+1 doesn't begin until N is merged and the firmware is flashed +
verified. Estimated calendar: ~5–7 working days end-to-end depending on
how much yak-shaving the voltage abstraction provokes in non-HubFX
boards.

### Phase 0 — Port voltage abstraction (foundation)

**Why first:** every later phase touches the port table, and we don't
want to retrofit the voltage column three times. Phase 0 is small and
self-contained but cross-cuts every board.

**Firmware (`controllers/lib/sfx_board/port_descriptor.h`):**

- Add `uint16_t voltageMv` to the `PwmDescriptor`, `ServoDescriptor`,
  `HBridgeDescriptor`, `LedDescriptor`, `InputDescriptor` records.
- New helper `.with_voltage_mV<N>()` on the port-list builders so a board
  declaration reads naturally:
  ```cpp
  static constexpr auto kPwmPorts = sfx_core::ports::list(
      sfx_core::ports::pwm_array<&HubFxBoard::pwm, 8>()
          .with_vSense_array<&HubFxBoard::vSense>()
          .with_iSense_array<&HubFxBoard::iSense>()
          .with_voltage_mV<8000>());   // ← new
  ```
- Default `voltageMv = 0` means "unknown" — UI treats it as unconstrained
  (no voltage check, no label).
- **Per-board values** (initial pass, revisit per HW):
  - HubFX `kPwmPorts` (CH1–8): **8000** (8 V rail)
  - HubFX `kServoPorts` (IN_2..IN_12): **5000** (5 V servo rail)
  - HubFX `kInputPorts` (IN_1): **3300** (3.3 V GPIO)
  - GunFx Pico `kPwmPorts`: TBD (probably battery rail — declare 0 = unknown, refine in Phase 2 once we instrument the board)
  - GunFx Pico `kServoPorts`: **5000**
  - GearControl `kHBridgePorts`: declare 0 = battery (unknown, depends on pack)

**Protocol (`PORT_LIST_RESP` payload):**

Each port entry gets a new `voltageMv:u16LE` field at a fixed offset (no
Rule-11 append-only acrobatics — greenfield, see §0.7). Document the
exact entry layout in the expander protocol header and bump
`FIRMWARE_VERSION` to make the lockstep flash explicit.

**Go (`app/go/protocol/expander/expander.go` decoder):**

- `Port` struct gains `VoltageMv uint16` field.
- Decoder reads at the documented fixed offset; no legacy-short
  fallback (§0.7).

**Device model (`app/go/devicemodel/types.go`):**

- `Port` struct gains `VoltageMv uint16`.
- New capability token `"VOLTAGE_<N>V"` (e.g. `"VOLTAGE_8V"`) appended
  to `Caps` when `voltageMv > 0` — purely informational, doesn't gate
  candidate filtering. The hard filter is at config-write time (see
  Phase 2 voltage check).

**Studio (`devicemodel.ts` + port-picker components):**

- `Port` interface gains `voltageMv: number`.
- Port-picker option label format: `"<boardName> <hardwareName> (8 V) · <kindName>"` so the operator sees what they're picking.
- A new helper `formatPortRail(voltageMv): string` returns `"5 V"` /
  `"8 V"` / `"3.3 V"` / `""` (when `voltageMv === 0`).

**Tests:**

- Unit test: decoder accepts both old (no voltage byte) and new
  (with voltage) `PORT_LIST_RESP` payloads.
- Unit test: `Port.Caps` includes `"VOLTAGE_8V"` when `voltageMv == 8000`.
- Manual: connect HubFX, run `topo-ports`, verify voltage column.

**Docs:**

- Update `controllers/hubfx/esp32s3/README.md` port table.
- Note the wire-format bump (no Rule 11 work — greenfield).

---

### Phase 0.5 — Global effect clock (foundation, before Phase 2)

**Why:** the rollout adds at least four new tick-driven subsystems per
gun (ROF scheduler, fan puffing, heater bang-bang, yaw+pitch motion
profile). Each calling raw `millis()` makes the timing math fragile —
two subsystems sampling "now" microseconds apart can compute different
deltas. A latched-per-tick clock fixes that with one line per call
site. Audit: 28 raw `millis()` / `micros()` calls across 7 files in
`effects/` today (EngineFx, GearControl, GunFx, landing_lights).

**Firmware — `controllers/lib/sfx_core/effect_clock.h` (NEW):**

```cpp
namespace sfx_core {

class EffectClock {
public:
    /// Latch a fresh time sample. Called ONCE at the top of every
    /// main-loop pass by the BoardServer/HubFx run loop, before any
    /// effect tick. Idempotent within a tick — extra calls are no-ops.
    void latch() {
        const uint32_t t = SFX_MILLIS();
        if (t == _nowMs) return;          // already latched this pass
        _dtMs   = t - _nowMs;
        _nowMs  = t;
    }

    uint32_t nowMs() const { return _nowMs; }
    uint32_t dtMs()  const { return _dtMs;  }     ///< delta since previous latch

    static EffectClock& instance() {
        static EffectClock c;
        return c;
    }
private:
    uint32_t _nowMs = 0;
    uint32_t _dtMs  = 0;
};

}  // namespace sfx_core
```

**Wiring:**

- `hubfx_esp32s3.ino`'s `loop()` calls `sfx_core::EffectClock::instance().latch()`
  before `board.process()` runs. Service policies see a consistent `nowMs()`
  for the entire tick.
- Each effect retrofits `SFX_MILLIS()` / `millis()` reads to
  `sfx_core::EffectClock::instance().nowMs()`. Motion profiles use
  `dtMs()` instead of computing their own deltas.

**Audit + retrofit checklist (Phase 0.5 scope):**

- [ ] `controllers/hubfx/esp32s3/src/effects/enginefx/enginefx_service.ipp` — 3 calls
- [ ] `controllers/hubfx/esp32s3/src/effects/gearcontrol/gearcontrol_service.ipp` — 2 calls
- [ ] `controllers/hubfx/esp32s3/src/effects/gearcontrol/gear.h/.ipp` — 13 calls
- [ ] `controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_service.ipp` — 2 calls
- [ ] `controllers/hubfx/esp32s3/src/effects/gunfx/gun_unit.h/.ipp` — 8 calls
- [ ] `controllers/hubfx/esp32s3/src/effects/landing_lights/landing_light.h` — 1 call

**Rule candidate — Rule 40:**

> Effects MUST read time from `sfx_core::EffectClock::instance()` —
> never raw `millis()` / `micros()` / `SFX_MILLIS()`. The clock latches
> ONCE per main-loop pass so every effect's tick sees the same `nowMs()`
> and a consistent `dtMs()` for delta-time math. Per-effect timers,
> ROF schedulers, motion profiles, fan puff timers, heater bang-bang
> windows — all read from the clock. Non-effect code (drivers, the bus,
> the keepalive, the upload state machine) keeps using raw `millis()`;
> the clock is for the effect layer only. Reference: phase-0.5 of
> `instructions/22-GUNFX-FEATURE-ROLLOUT.md`.

---

### Phase 1 — Protocol & config schema extensions

**Wire packets (HubFx range, append at next free):**

Current GunFx range `0xCC–0xD2` (7 packets, 0xD7–0xD9 already taken by
GearControl per CLAUDE.md collision history). Append into the
**Available** `0xE2–0xED` block:

| ID | Name | Direction | Payload |
|---|---|---|---|
| `0xE2` | GUN_MANUAL_SET | master→ | `[id:u8][flags:u8][yawUs:u16][pitchUs:u16][rofIndex:u8][fireHold:u8][smokeArm:u8][smokeFanBurst:u8]` — flags bitmask says which fields are valid (0x01=yaw, 0x02=pitch, 0x04=rof, 0x08=fire, 0x10=smoke, 0x20=fan) |
| `0xE3` | GUN_MANUAL_RELEASE | master→ | `[id:u8]` → ACK. Returns gun to RC-input-driven mode. |
| `0xE4` | GUN_VERBOSE_STATUS_REQ | master→ | `[id:u8][enable:u8]` → ACK. Subscribes/unsubscribes verbose status broadcasts at ~10 Hz. |
| `0xE5` | GUN_VERBOSE_STATUS | ←device | async TAG_ASYNC: see § 1.4 below |

GUN_VERBOSE_STATUS payload (Rule 11 append-only; v1 layout):

```
[id:u8]
[mode:u8]                       // 0=rc, 1=manual
[firing:u8]
[smokeArmed:u8]
[smokeFanRunning:u8]
[heaterDuty_pct:u8]
[heaterTempCx10:i16]            // 0x7FFF = no sensor
[yawCurrentUs:u16]
[yawTargetUs:u16]
[pitchCurrentUs:u16]
[pitchTargetUs:u16]
[rofIndex:u8]                   // 0xFF = none
[rofSelectorUs:u16]             // last raw value on the ROF channel
[triggerUs:u16]                 // last raw value on the trigger channel
[shotsThisSession:u32]
```

Estimated 26 bytes per gun, well under the 512 B payload cap.

**Config schema (`controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_config.h`, NEW):**

```cpp
struct RofItem {
    char     name[16]     = {};
    uint16_t bandLoUs     = 0;       // 0 = unbounded low
    uint16_t bandHiUs     = 0;       // 0 = unbounded high
    uint16_t rpm          = 600;
    char     soundPath[64] = {};
};

// Phase 4 polish 2026-05-26: heater is open-loop (no temp sensor wired
// on HubFX); element voltage scaling lives on the role layer (Rule 42);
// heater activation channel added (Rule 43 named-channel gate); heater
// mode set is {continuous, cycle} — cycle is gun-layer duty-cycle for
// power conservation (no thermistor required).
struct SmokeConfig {
    PortRef  heaterPort;
    uint16_t heaterElementMv  = 6000;  // rated element voltage (default 6 V smoke cartridge)
    uint8_t  heaterMode       = 0;     // 0=continuous, 1=cycle
    uint16_t heaterCycleOnMs  = 5000;  // CYCLE: on-phase duration
    uint16_t heaterCycleOffMs = 3000;  // CYCLE: off-phase duration
    // Rule 43 activation gate — optional named-channel from /hubfx.yaml inputs[].
    char     heaterActivationInput[kInputNameMax] = {};
    uint16_t heaterActivationThresholdUs  = 1500;
    uint16_t heaterActivationHysteresisUs = 25;

    PortRef  fanPort;
    uint16_t fanElementMv        = 6000;  // rated motor voltage (default 6 V smoke fan)
    uint8_t  fanMode             = 1;     // 0=continuous, 1=pulse  (disabled = unset fanPort)
    uint16_t fanPulseDurationMs  = 100;   // PULSE: sinusoid envelope period
                                          //   pct(t) = 50 + 50*sin(π*t/dur) over t∈[0,dur]
                                          //   100 ms default matches 600 RPM firing period
};

struct AxisProfile {
    bool     enabled    = false;
    PortRef  servoPort;
    PortRef  inputPort;        // matches a channel function
    uint8_t  inputChannel = 0;
    uint16_t minUs       = 1000;
    uint16_t maxUs       = 2000;
    uint16_t centerUs    = 1500;
    bool     inverted    = false;
    uint16_t maxSpeedUsPerSec  = 800;   // µs/sec slew limit
    uint16_t maxAccelUsPerSec2 = 1600;  // µs/sec²
    uint16_t maxJerkUsPerSec3  = 0;     // 0 = jerk disabled
};

struct GunDef {
    uint8_t   id            = 0;
    char      name[16]      = {};

    // Trigger channel
    PortRef   triggerPort;
    uint8_t   triggerChannel = 0;
    uint16_t  triggerThresholdUs  = 1500;
    uint16_t  triggerHysteresisUs = 25;

    // ROF channel (selector for which RofItem is armed)
    PortRef   rofSelectorPort;
    uint8_t   rofSelectorChannel = 0;
    uint8_t   numRofItems    = 0;
    RofItem   rofItems[8]    = {};

    // Muzzle flash (LedAnimator)
    PortRef   muzzleFlashPort;
    uint16_t  flashDurationMs   = 30;
    uint8_t   flashBrightness   = 100;

    // Recoil (ServoActuator on a dedicated servo)
    PortRef   recoilServoPort;
    uint16_t  recoilCenterUs    = 1500;
    uint16_t  recoilJerkUs      = 200;
    uint16_t  recoilHoldMs      = 80;

    // Smoke + fan
    SmokeConfig smoke;

    // Yaw + pitch (each independently optional)
    AxisProfile yaw;
    AxisProfile pitch;
};

struct GunFxConfig {
    bool      enabled       = false;
    uint8_t   numGuns       = 0;
    GunDef    guns[kMaxGuns] = {};
};
```

**YAML (`/gunfx.yaml`) shape:**

```yaml
schemaVersion: 1
enabled: true
guns:
  - id: 0
    name: "Main gun"
    trigger:
      port:    { board: hub, kind: input, idx: 0 }
      channel: 0
      thresholdUs:  1500
      hysteresisUs: 25
    rofSelector:
      port:    { board: hub, kind: input, idx: 0 }
      channel: 1
      items:
        - { name: burst,  band: [900, 1200],  rpm: 120, sound: /sounds/gun/burst.wav }
        - { name: normal, band: [1200, 1600], rpm: 600, sound: /sounds/gun/fire.wav }
        - { name: rapid,  band: [1600, 2000], rpm: 900, sound: /sounds/gun/rapid.wav }
    muzzleFlash:
      port: { board: hub, kind: pwm, idx: 0 }     # any LedAnimator-capable port
      durationMs: 30
      brightness: 100
    recoil:
      port: { board: gunfx-3c4d, kind: servo, idx: 0 }
      centerUs: 1500
      jerkUs:   200
      holdMs:   80
    smoke:
      heater:
        port: { board: hub, kind: pwm, idx: 1 }
        elementMv: 6000          # 6 V heater on the 8 V rail → 75% duty (linear, role-applied)
        mode: cycle              # continuous | cycle
        cycleOnMs:  5000         # cycle: 5 s on
        cycleOffMs: 3000         # cycle: 3 s off (≈ 63 % duty cycle, 8 s period)
        activation:              # Rule 43 — optional RC gate
          input: smoke_arm
          thresholdUs:  1500
          hysteresisUs: 25
      fan:
        port: { board: hub, kind: pwm, idx: 2 }
        elementMv: 6000
        mode: pulse                 # continuous | pulse (sinusoidal envelope per shot)
        pulseDurationMs: 100        # one sinusoid lasts this long (≈ firing period)
    yaw:
      enabled: true
      servoPort: { board: hub, kind: servo, idx: 0 }
      inputPort: { board: hub, kind: input, idx: 0 }
      inputChannel: 2
      range: [1100, 1900]
      center: 1500
      maxSpeedUsPerSec:  800
      maxAccelUsPerSec2: 1600
      maxJerkUsPerSec3:  0
    pitch:
      enabled: true
      servoPort: { board: hub, kind: servo, idx: 1 }
      inputPort: { board: hub, kind: input, idx: 0 }
      inputChannel: 3
      range: [1300, 1700]
      center: 1500
      maxSpeedUsPerSec:  600
      maxAccelUsPerSec2: 1200
      maxJerkUsPerSec3:  0
```

**Device model — `DomainGun` slot expansion:**

```go
{
    ID: DomainGun, Label: "Gun (GunFX)", Cap: core.CapGunFx,
    Slots: []Slot{
        {Key: "trigger",      Label: "Fire trigger",    RoleKinds: inputRoleKinds,              Direction: DirInput,  Min: 0, Max: 1, Optional: true,  Shared: true},
        {Key: "rofSelector",  Label: "ROF selector",    RoleKinds: inputRoleKinds,              Direction: DirInput,  Min: 0, Max: 1, Optional: true,  Shared: true},
        {Key: "muzzleFlash",  Label: "Muzzle flash",    RoleKinds: []byte{roles.KindLedAnimator}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},   // one per gun, up to 4 guns
        {Key: "recoilServo",  Label: "Recoil servo",    RoleKinds: []byte{roles.KindServoActuator}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
        {Key: "smokeHeater",  Label: "Smoke heater",    RoleKinds: []byte{roles.KindHeater, roles.KindDcMotor}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
        {Key: "smokeFan",     Label: "Smoke fan",       RoleKinds: []byte{roles.KindDcMotor},    Direction: DirOutput, Min: 0, Max: 4, Optional: true},
        {Key: "yawServo",     Label: "Yaw servo",       RoleKinds: []byte{roles.KindServoActuator}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
        {Key: "pitchServo",   Label: "Pitch servo",     RoleKinds: []byte{roles.KindServoActuator}, Direction: DirOutput, Min: 0, Max: 4, Optional: true},
        {Key: "yawInput",     Label: "Yaw channel",     RoleKinds: inputRoleKinds,              Direction: DirInput,  Min: 0, Max: 4, Optional: true,  Shared: true},
        {Key: "pitchInput",   Label: "Pitch channel",   RoleKinds: inputRoleKinds,              Direction: DirInput,  Min: 0, Max: 4, Optional: true,  Shared: true},
    },
}
```

The `Max` is `4` (one per gun) on the per-gun output slots. The model
enforces exclusive output claims, so two guns can't both pick the same
muzzle-flash port — but they CAN pick different LedAnimator ports on
different boards.

---

### Phase 2 — HubFX firmware

**File-by-file changes in
`controllers/hubfx/esp32s3/src/effects/gunfx/`:**

1. **`gunfx_config.h` (NEW)** — the struct definitions from § 1 above.

2. **`gun_unit.h` + `.ipp`** — rewrite to use `GunDef` from
   `gunfx_config.h`. Replace the existing flat fields with embedded
   `SmokeConfig`, `AxisProfile`, `RofItem[]`. New responsibilities per
   `GunUnit::tick()`:
   - Read `triggerPort` channel → threshold-with-hysteresis → fire flag.
   - Read `rofSelectorPort` channel → find first `RofItem` whose band
     contains the value → that index is armed. Out-of-band → no item
     armed, fire flag suppressed.
   - When armed + fire flag: schedule shots at `rofItems[i].rpm` (interval
     = 60000 / rpm ms). Each shot fires the muzzle flash, plays
     `rofItems[i].soundPath`, optionally pulses recoil + fan puff.
   - **Voltage scaling** lives on the ROLE LAYER (Rule 42, Phase 4
     polish 2026-05-26).  The gun service pushes `element_mv` to
     `HeaterRole` / `DcMotorRole` via `HEATER_SET_ELEMENT` /
     `MOTOR_SET_ELEMENT` after `claimPorts()`; the role internally
     calls `sfx_core::scaleDuty(pct, portMaxDuty, portRailMv, element)`
     when it commands the PWM port.  Gun code never sees voltages —
     only intent (`commandHeater(on)`, `commandFanPct(100)`).
   - **Heater behaviour** (open-loop — 2026-05-26):
     HubFX has no temperature sensor wired to the smoke heater, so
     the heater is open-loop with two modes set by `heaterMode`:
     - `continuous` — drive at element-scaled duty whenever
       `_smokeArmed && _heaterActive` (Rule 43 gate; unbound channel
       ⇒ permanently allowed); OFF otherwise.
     - `cycle` — gun-layer duty-cycle: while smoke is armed, drive
       the heater for `heaterCycleOnMs`, then off for
       `heaterCycleOffMs`, repeating.  Used for power conservation
       or to limit cartridge temperature without a thermistor.  The
       phase clock marches independently of the Rule 43 activation
       gate (commandHeater() ANDs the gate at the wire boundary, so
       a gated-off ON-phase produces no wire-out; the next phase
       flip happens on schedule).
     Voltage scaling against the port rail happens on the role layer
     (`HeaterRole::tick()` → `scaleDuty()`).  Bang-bang (closed-loop
     thermostat) is intentionally absent — the role implementation
     still supports it, but the GunFx config layer doesn't expose it
     until temperature sensor hardware lands.
   - **Fan modes** (canonical 2-mode set):
     - `continuous`: run at element-rated voltage while firing AND
       smoke armed.
     - `pulse`: sinusoidal envelope per shot — fan idles at **50 %
       base** while firing+armed, then on every shot ramps to
       **100 % peak** at mid-duration and back to 50 % along
       `pct(t) = 50 + 50 × sin(π × t / fanPulseDurationMs)`.  Default
       100 ms ≡ 600 RPM firing period, so the sinusoid completes
       once per shot at default cadence.  Faster ROFs collapse
       adjacent envelopes (each shot restarts the clock); motor
       inertia smooths the resulting waveform.  Wire chatter is
       rate-limited to ~50 Hz (`kFanUpdatePeriodMs = 20 ms`).
     - "Fan disabled" is encoded by leaving `fanPort` empty — every
       fan command early-returns on that.  No separate `off` mode.

3. **Yaw/pitch axis** — each `AxisProfile` instantiates a
   `MotionProfile1D` that consumes the input µs and produces a target
   µs constrained by `(maxSpeed, maxAccel, maxJerk)`. The profile runs
   per tick and writes to the bound `ServoActuator`. Extend the existing
   trapezoidal `MotionProfile` in `controllers/lib/sfx_board/motion/`
   (or create one if it doesn't have jerk) — jerk = 0 falls back to
   trapezoidal.

4. **Manual override mode** — `GunUnit::setManualOverride(GunManualState
   s)` enters manual mode: the per-tick reader skips the trigger /
   ROF / yaw / pitch channel reads and uses the manual state instead.
   `clearManualOverride()` returns to RC. While in manual mode, the
   verbose status broadcast reflects what the operator is driving.
   Triggered by GUN_MANUAL_SET / GUN_MANUAL_RELEASE packets.

5. **Verbose status producer** — `GunFxService::handle(0xE4)` toggles
   a per-gun `verboseEnabled` flag. When enabled, `GunUnit::tick()`
   accumulates state and `GunFxService::update()` broadcasts
   GUN_VERBOSE_STATUS at ~10 Hz (configurable interval). Cost: one
   ~26 B packet per gun per 100 ms = ~2 KB/s for 4 guns. Trivial for
   USB CDC at 6 Mbps.

6. **`gunfx_service.ipp` `applyConfig()`** — parses
   `GunFxConfig`, attaches each `GunDef`'s ports via the role layer
   (`attachRole(servoPort, ServoActuator)`, etc.), claims the slots in
   the device model, and validates wiring (yaw + pitch must be
   independent ports, fan voltage compatible, etc.). Returns warnings
   for non-fatal issues, errors for fatal ones — wired to the same
   `applyHubFxConfig` callback EngineFx uses.

7. **Bump `FIRMWARE_VERSION` to `2.11.0-hubfx`** (MINOR — additive
   protocol). `BUILD_NUMBER` auto-increments via build script.

---

### Phase 3 — Go API + Wails bindings

1. **`app/go/protocol/gunfx/gunfx.go`** — add packet constants for
   0xE2–0xE5, builder funcs (`CmdManualSet`, `CmdManualRelease`,
   `CmdVerboseStatusReq`), decoder for `DecodeVerboseStatus`.

2. **`app/go/api/gunfx.go`** — extend `GunFxApi` with:
   - `ManualSet(id uint8, state GunManualState) CommandResult`
   - `ManualRelease(id uint8) CommandResult`
   - `VerboseStatusSubscribe(id uint8, enable bool) CommandResult`
   - Async event handler for VERBOSE_STATUS — routes to a per-id
     observer registered by Studio.

3. **`app/go/engine/handlers/gunfx/`** — `types.go` gets
   `VerboseStatus` struct (mirrors firmware), `Decode...`. `handler.go`
   gains `verbose-status`, `manual`, `manual-release` CLI commands
   under the `gun:` prefix (Rule 30). `format.go` renders verbose
   status to the CLI.

4. **`app/go/studio/app_gunfx.go` (NEW)** — Wails bindings:
   ```go
   func (a *App) LoadGunFxConfig() (GunFxConfigT, error)
   func (a *App) SaveGunFxConfig(cfg GunFxConfigT) error  // → YAML + ReloadPath
   func (a *App) GunFxStatus() (GunFxStatusT, error)      // light status (per-gun firing/armed)
   func (a *App) GunManualSet(id uint8, s GunManualStateT) error
   func (a *App) GunManualRelease(id uint8) error
   func (a *App) GunVerboseSubscribe(id uint8, on bool) error
   ```
   Emits Wails events:
   - `gunfx:verbose:<id>` — per-gun verbose status struct
   - `gunfx:shot:<id>` — async shot event (existing)

5. **`app/go/studio/app_engine.go` pattern** — copy
   `LoadEngineConfig` / `SaveEngineConfig` round-trip: download
   `/gunfx.yaml` from flash → parse → return; on save, serialise →
   upload to flash → `ConfigReloadPath("/gunfx.yaml")`.

---

### Phase 4 — Studio panel

**File: `app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte`** —
full rebuild. Companion: `app/go/studio/frontend/src/lib/gunfx.ts`
(stores + Wails wrapper helpers, mirrors `effects.ts`).

#### 4.1 Panel anatomy (one card per gun)

```
┌─ GunFX ──────────────────────────────────────[☑ Enabled][+ Add gun]┐
│                                                                    │
│ ┌─ Main gun ──────────────────────────────────────[× Remove]──────┐│
│ │ STATE [firing@600] [armed:normal]   [in sync] [Apply] ‖ [⏵ Test]││  ← status-row (Rule 35)
│ │                                                                 ││
│ │ ── TRIGGER ─────────────────────────────────────────────────── ││
│ │ ┌─ Channel-setup cluster (Rule 36) ──────────────────────────┐ ││
│ │ │ INPUT  [CH1 · gun_trigger          ▾ ]                     │ ││
│ │ │ FIRES WHEN CHANNEL ≥ [1500] µs ± [25] µs hysteresis        │ ││
│ │ │ ▓▓▓▓▓▓▓▓▓▓▓▓▓▓│░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │ ││
│ │ │ 1532µs live · 1500µs threshold · ±25µs hyst · 1000–2000µs │ ││
│ │ └────────────────────────────────────────────────────────────┘ ││
│ │                                                                 ││
│ │ ── RATE OF FIRE ─────────────────────────────────────────────── ││
│ │ ┌─ Channel-setup cluster (Rule 36 + multi-band overlay) ─────┐ ││
│ │ │ INPUT  [CH2 · gun_rof             ▾ ]                      │ ││
│ │ │ [▓▓BURST▓▓│ NORMAL │RAPID▓▓▓▓▓▓▓▓│ ⌬ 1850µs ARMED:RAPID]  │ ││  ← non-overlapping coloured bands
│ │ │  900-1200   1200-1600  1600-2000                           │ ││
│ │ └────────────────────────────────────────────────────────────┘ ││
│ │   [#1 burst  band:[900-1200]µs  120 RPM  /sounds/burst.wav  [...]Clear] ││
│ │   [#2 normal band:[1200-1600]µs 600 RPM  /sounds/fire.wav   [...]Clear] ││
│ │   [#3 rapid  band:[1600-2000]µs 900 RPM  /sounds/rapid.wav  [...]Clear] ││
│ │                                              [+ Add ROF item]   ││
│ │                                                                 ││
│ │ ── MUZZLE FLASH ───────────────────────────────────────────── ││
│ │ Port [HubFX CH3 (8V) · pwm/LedAnimator    ▾ ]   Duration [30]ms ││
│ │ Brightness [100]%                                                ││
│ │                                                                 ││
│ │ ── RECOIL ───────────────────────────────────────────────────── ││
│ │ Servo [HubFX SRV1 (5V)                    ▾ ]                  ││
│ │ Center [1500]µs · Jerk [200]µs · Hold [80]ms                   ││
│ │                                                                 ││
│ │ ── GUN SMOKE · sibling card ───[smoke armed][🔥 heating][💨 fan]──┐│
│ │ Activation channel  [SBUS ch 7 · smoke_arm ▾]  Thr [1500]µs  Hyst 25│
│ │ ┌─ HEATER ─────────────────┐  ┌─ FAN ─────────────────────────┐ ││
│ │ │ Port [HubFX CH4 (8V) ▾]  │  │ Port [HubFX CH5 (8V) ▾]       │ ││
│ │ │ Element [6000]mV         │  │ Element [6000]mV              │ ││
│ │ │ Mode [cycle ▾]            │  │ Mode [pulse (sinusoidal) ▾]   │ ││
│ │ │ On [5000]ms  (5.0 s)     │  │ Duration [100]ms (0.10 s)     │ ││
│ │ │ Off [3000]ms (3.0 s)     │  │ envelope: 50% → 100% → 50%    │ ││
│ │ │ average duty: 63%        │  └───────────────────────────────┘ ││
│ │ └──────────────────────────┘                                     ││
│ │ Header: [▶ Smoke ON][■ Smoke OFF]  ← simulate cluster              ││
│ │                                                                 ││
│ │ ── YAW (optional) ─────────────────────────[☑ Enabled]──────── ││
│ │ ┌─ Channel-setup cluster (axis variant) ─────────────────────┐ ││
│ │ │ INPUT  [CH5 · gun_yaw            ▾ ]                       │ ││
│ │ │ ▓▓▓▓▓▓▓▓▓▓▓▓│CENTER│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░ │ ││  ← min/center/max overlay
│ │ │ 1700µs · range [1100–1900]µs · center [1500]µs            │ ││
│ │ └────────────────────────────────────────────────────────────┘ ││
│ │ Servo [HubFX SRV2 (5V)  ▾ ]   Inverted [☐]                    ││
│ │ Max speed [800]µs/s · Accel [1600]µs/s² · Jerk [0]µs/s³        ││
│ │                                                                 ││
│ │ ── PITCH (optional) ────────────────────────[☑ Enabled]────── ││
│ │ … same shape as Yaw, separate channel + servo                  ││
│ │                                                                 ││
│ │ ── MANUAL CONTROL (simulate) ─────────────[☐ Override RC]──── ││
│ │ YAW   [────────●────────]  1500µs   →  current 1487µs          ││
│ │ PITCH [────●────────────]  1300µs   →  current 1302µs          ││
│ │ ROF   [#2 normal ▾]   FIRE [● HOLD]   SMOKE [☐]   FAN burst [⚡]││
│ │ Live: firing=YES @600  heater=72%@142°C  yaw→1487  pitch→1302  ││  ← verbose status echo
│ │                                                                 ││
│ └─────────────────────────────────────────────────────────────────┘│
└────────────────────────────────────────────────────────────────────┘
```

Cross-cuts every rule already established:

- **Rule 34** — every input/select uses `.field-input` + size modifier;
  buttons follow the canonical row order; voltage labels in picker
  options; warning chips on unclaimed-port sections.
- **Rule 35** — Apply gated on dirty + errors; Start/Test gated the
  same; manual-control checkboxes are operational actions (also
  gated on dirty + errors); validation surfaces twice (field + section).
- **Rule 36** — trigger + ROF + yaw + pitch all use the channel-setup
  cluster. ROF extends it with multi-band overlay (see § 4.2 below);
  yaw/pitch extend it with a single highlighted "current target" range
  (see § 4.3).

#### 4.2 NEW: Multi-band ROF overlay on the channel bar (Rule 36 extension)

When the channel cluster's `bands` prop is non-empty, render coloured
non-overlapping zones in place of the single threshold marker. Each
zone:

- `left: usToPct(band.lo)%, width: usToPct(band.hi) - usToPct(band.lo)`
- Colour cycles through a palette (`--gunfx-band-1` … `--gunfx-band-4`)
  defined once in `style.css` — same vibe as the EngineFx hysteresis
  band but distinct hues per item.
- Label text inside the zone (`burst`, `normal`, `rapid`) when the zone
  is ≥ 8 % wide; otherwise tooltip-only.
- The live fill renders normally; the band the live value falls into
  glows brighter to show "armed".
- Gap zones (between bands, or before / after) render as the bare
  track — operator sees the dead-band where no ROF item is armed.

Validation:

- Bands **must not overlap** — visualised by a red diagonal-stripe
  hatch over the overlapping region + `⚠ overlap` chip on the section
  head.
- Bands **must monotonically increase** — `band.lo < band.hi`; out-of-order
  rows highlight red.
- At least one band when ROF channel is bound (or skip ROF and use
  default RPM from the trigger).

#### 4.3 NEW: Range-mapped axis overlay (yaw / pitch)

For yaw / pitch the channel isn't a threshold trigger; it's a position
input mapped through a `[minUs, maxUs]` range. Cluster variant:

- Two faint vertical markers at `usToPct(minUs)` and `usToPct(maxUs)`
  (axis stops).
- One solid marker at `usToPct(centerUs)` (servo neutral).
- Filled band between minUs and maxUs shows the active travel range;
  out-of-range portions of the bar are dimmed.
- Legend: `<live> µs · range [min–max] µs · center <centerUs> µs ·
  inverted [☐/☑]`.

#### 4.4 NEW: Optional sections + unclaimed-port warnings

Smoke heater, smoke fan, nozzle flash, yaw, pitch are all **optional**.
Per Rule 35 §4 (required vs optional): empty + optional = valid (no
error). But when the operator *enables* an optional section and there
are no unclaimed candidate ports available, surface a **yellow
warning**:

- `.section-head.section-warn` (amber instead of red).
- `⚠ no free PWM port` chip.
- Hint line below the head: `"Need 1 free PWM port. Free a port in the
  IO tab, or disable this section."`

Distinct from errors — yellow doesn't block Apply. It does flip the
status-row `dirty-flag` to a soft warning: `"warnings: 2 sections need
ports"`.

#### 4.5 Manual control subsection (the "simulate" panel)

```svelte
<div class="section-head">Manual control (simulate)
    <label class="enable-toggle">
        <input type="checkbox" bind:checked={manualOverride}
               on:change={onToggleManual} />
        <span>{manualOverride ? 'OVERRIDING RC' : 'RC driven'}</span>
    </label>
</div>
```

While `manualOverride` is on:
- Yaw / pitch sliders push `GUN_MANUAL_SET` debounced ~50 ms; live µs
  echoed in the verbose-status panel.
- `FIRE [●HOLD]` button sends `manualSet(.fireHold=1)` on mousedown,
  releases on mouseup — true continuous fire, releases via firmware
  auto-release on disconnect.
- `ROF [...]` dropdown sets `rofIndex` field of manual state.
- `SMOKE` checkbox sends `manualSet(.smokeArm=1/0)`.
- `FAN burst` button sends `manualSet(.smokeFanBurst=1)` (one-shot
  pulse of `fanPuffMs`).
- Verbose-status subscription auto-enabled while manual; auto-released
  when the panel unmounts or the user toggles back to RC.

The manual section is gated by Rule 35 like everything else: while the
form is dirty or has errors, the override toggle is disabled with
tooltip `"Apply changes before simulating"`. Otherwise pushing the
override would test the OLD firmware config.

#### 4.6 NEW: Cross-board port picker grouping (architecture §0.2)

Port-picker `<select>` options grouped via `<optgroup label="…">` per
board. The grouping comes free with the device-model `Port.BoardName`;
new in this PR is the `<optgroup>` shape and the voltage suffix:

```svelte
{#each boardsWithCandidates as b}
    <optgroup label="{b.name} ({b.guid})">
        {#each b.ports as p}
            <option value={p.ref}>
                {p.hardwareName} ({formatPortRail(p.voltageMv)}) · {p.kindName}
                {p.roleName ? `→ ${p.roleName}` : ''}
            </option>
        {/each}
    </optgroup>
{/each}
```

If a section needs N ports of the same kind and only M < N are
available across all boards, the picker disables the section and surfaces
the yellow warning (§ 4.4).

---

## 3. New rule candidates (drafts)

These would land in `.github/copilot-instructions.md` and CLAUDE.md as
part of Phase 4. Drafted here so reviewers can pushback before they're
baked in.

### Rule 38 — Multi-band channel overlay (Rule 36 extension)

> Where a channel selects between N discrete items (ROF presets, sound
> modes, gear sets), render the items as **non-overlapping coloured
> bands** on the live bar instead of a single threshold marker. Each
> band gets a stable colour from a palette
> (`--gunfx-band-1..N` / similar — define once in `style.css`). Live
> value sits in exactly one band (or in the gap = no item armed);
> the armed band glows brighter. Bands must not overlap and must
> monotonically increase — visualise overlaps as red diagonal hatch +
> `⚠ overlap` chip on the section head. Add/remove items rebalances
> the bands automatically (no overlap auto-resolution; surface as
> error). Reference: GunFxPanel ROF cluster.

### Rule 39 — Optional-section unclaimed-port warning

> Optional sections whose ports are unfindable surface as **yellow
> warnings, not errors**. Section head turns amber, a `⚠ no free <kind>
> port` chip appears, a hint line below the head tells the operator
> what to do. Yellow does NOT gate Apply (the operator can save a
> config with an unwired optional section); it does feed a
> `warnings: N` status flag visible next to the dirty indicator. Use
> distinct from errors (which DO gate Apply per Rule 35).

### Rule 41 — Manual override / puppet-mode panel

> Operational effect panels MAY expose a **manual override / puppet
> mode** subsection that drives every firmware-side input directly
> from the GUI (sliders for axis inputs, buttons for trigger / smoke,
> dropdowns for discrete selectors). When enabled:
> - The firmware suspends RC-channel reads for the bound subsystems
>   and reads the manual state instead (one packet per slider change,
>   debounced ~50 ms).
> - The firmware enables **verbose status broadcasts** (~10 Hz) so the
>   panel mirrors live per-element state (servo positions, duty cycles,
>   temperatures).
> - The override toggle is gated by Rule 35 the same as Apply / Start
>   (dirty draft + errors disable it) — pushing the override on a stale
>   draft would test the old config.
> - The verbose subscription is released automatically on panel
>   unmount / disconnect / explicit RC return. The firmware always
>   reverts to RC when an explicit RELEASE packet arrives OR after a
>   bounded timeout (5 s no MANUAL_SET → auto-release) so a Studio
>   crash never leaves a gun in puppet mode.
> Reference: GunFxPanel manual-control subsection.

---

## 4. Suggested PR sequencing

| PR | Phase | Scope | Reviewable in |
|---|---|---|---|
| #1 | 0 | Port voltage abstraction (firmware + Go + Studio). Touches every board's port table but each change is mechanical. | ~1 hour |
| #1b | 0.5 | Global `EffectClock` singleton + retrofit 28 `millis()` call sites across effects/. Pre-req for Phase 2 motion-profile math. | ~45 min |
| #2 | 1 | Wire protocol packets (0xE2–0xE5) + Go mirror + Wails binding stubs returning ErrNotImplemented. Bumps `FIRMWARE_VERSION` to 2.11.0-hubfx. | ~30 min |
| #3 | 2 | HubFX firmware: GunFxConfig schema + gun_unit rewrite + ROF arbitration + smoke modes + voltage scaling + yaw/pitch profile + manual mode + verbose status. The biggest PR. | ~2–3 hours |
| #4 | 3 | `app_gunfx.go` Wails bindings; GunFxApi extensions; CLI commands; YAML round-trip. | ~1 hour |
| #5 | 4a | Studio: GunFxPanel anatomy + trigger cluster + ROF cluster (multi-band) + sound rows. NO manual control yet. | ~2 hours |
| #6 | 4b | Studio: muzzle/recoil/smoke sections + voltage labels + unclaimed-port warnings + Rule 38. | ~1 hour |
| #7 | 4c | Studio: yaw + pitch sections + range-mapped axis overlay. | ~1 hour |
| #8 | 4d | Studio: manual control subsection + verbose status subscriber + Rule 39. | ~1.5 hours |
| #9 | docs | Rules 37/38/39 to copilot-instructions.md; CLAUDE.md condensed; instructions/22 final pass with "landed" notes; instructions/20 link to 22. | ~30 min |

Total: ~10–12 hours of focused work. Each phase is independently
shippable — if Phase 2 takes longer, Phases 0/1/3/4 can ship behind
it without firmware bumps.

---

## 5. Risks / open questions

1. **Voltage scaling formula default.** Linear (`duty = V_e / V_p`) is
   correct for resistive elements (heater = nichrome). Quadratic is
   only right when you care about power, not voltage. Plan currently
   exposes both with linear as default; needs HW validation.
2. **Verbose status at 10 Hz for 4 guns = ~80 packets/sec.** Trivial
   for USB CDC at 6 Mbps but adds load to the hub's main loop. Bench
   on a real board before committing to 10 Hz — back off to 5 Hz if
   the input dispatcher slows.
3. **Heater bang-bang without a temp sensor** — the spec says
   "always on or bang bang". Bang-bang requires a sensor; we'll log a
   warning + fall back to always-on. Should we surface this as a UI
   warning instead of silently degrading?
4. **Yaw/pitch limits and a misconfigured servo** — there's no
   firmware-side travel-stop sanity check. A wrong `maxUs` can drive
   a servo into a mount. The manual-control panel sliders are clamped
   to the configured range, which helps; we should add a config-time
   warning if `(maxUs - minUs) > 1000` µs ("range > 1 ms — confirm
   servo can physically travel that far").
5. **GunFx pico expander port voltages are unknown.** Phase 0 declares
   them `0` (unknown); UI shows no voltage label. Refine when the
   board is instrumented or measured.
6. **Multi-gun resource contention** — 4 guns × (yaw+pitch+recoil) = 12
   servos. HubFX has 11 servo headers (IN_2..IN_12). Servos can spill
   over to the GunFx expander or to gear ports as long as the model
   says they're free. The Phase 4 picker enforces this; no firmware
   change needed.

---

## 6. Reference paths (for the implementer)

- Existing GunFx firmware:
  [controllers/hubfx/esp32s3/src/effects/gunfx/](../controllers/hubfx/esp32s3/src/effects/gunfx/)
- Existing GunFx protocol:
  [controllers/lib/sfx_serial/serial/gunfx/gunfx.h](../controllers/lib/sfx_serial/serial/gunfx/gunfx.h)
  + [app/go/protocol/gunfx/gunfx.go](../app/go/protocol/gunfx/gunfx.go)
- EngineFx panel reference (the model to mirror):
  [21-STUDIO-ENGINEFX-PANEL.md](21-STUDIO-ENGINEFX-PANEL.md)
- Device-model `DomainGun`:
  [app/go/devicemodel/types.go](../app/go/devicemodel/types.go)
- Port descriptors:
  [controllers/lib/sfx_board/port_descriptor.h](../controllers/lib/sfx_board/port_descriptor.h)
- HubFX board sketch (port-list declarations + voltage rail wiring):
  [controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino](../controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino)
- GunFx pico expander:
  [controllers/gunfx/pico/](../controllers/gunfx/pico/)
- Generic-expander refactor plan:
  [15-GENERIC-EXPANDER-REFACTOR.md](15-GENERIC-EXPANDER-REFACTOR.md)
- Studio EnginePanel (the panel to clone):
  [app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte)

---

## 7. Phases — Landed Status

### Phase 0 — Landed (2026-05-23)
- Port voltage abstraction (descriptors + bindings + HubFX tagging + PORT_LIST_RESP wire change).
- Go `Port.VoltageMv` + devicemodel `VOLTAGE_<N>V` cap token + Studio `formatPortRail()` + GUI surfacing.
- **Rule 37** documented.

### Phase 0.5 — Landed (2026-05-23)
- `sfx_core::EffectClock` singleton; 11 `millis()` call sites retrofitted in `effects/`.
- **Rule 40** documented.

### Phase 1 — Landed (2026-05-23)
- Wire packets `0xE2..0xE5` (MANUAL_SET, MANUAL_RELEASE, VERBOSE_STATUS_REQ, VERBOSE_STATUS).
- `gunfx_config.h` with `RofItem` / `SmokeConfig` / `GunAxis` / `GunSpec` / `GunFxConfig`.
- Shared `sfx_core::ServoMotionProfile` (`sfx_board/motion/servo_motion_profile.h`) — generalised from gun-specific.
- Go protocol mirror + `client.Gun.ManualSet/ManualRelease/VerboseStatusSubscribe` + `Events.OnGunVerboseStatus`.
- `DomainGun` expanded to 10 slots; `app_gunfx.go` Wails stubs (firmware NACKs `NOT_IMPLEMENTED`).

### Phase 2 — Landed (2026-05-23)
- **Element scaling moved to role layer** (per user feedback — generic concerns at the right level of the stack).
  - `sfx_core::ElementConfig` + `scaleDuty()` in [sfx_board/element/element_scaling.h](../controllers/lib/sfx_board/element/element_scaling.h).
  - `HeaterRole` + `DcMotorRole` gain `setElement(cfg)` / `setPortRailMv(mv)` / intent-level `setDrivePct` / `setPct`.
  - `attachHeater` / `attachDcMotor` read element bytes from the role-attach config; `binding.voltageMv` feeds the rail automatically.
  - **Rule 42** documented (initially for element scaling, generalised in Phase 2.9).
- `sfx_core::MotionProfile1D` runtime integrator + `ServoMotionProfile` config consolidated in [sfx_board/motion/motion_profile.h](../controllers/lib/sfx_board/motion/motion_profile.h) — trapezoidal speed/accel slew + optional jerk-bounded S-curve, decel lookahead. (Initially split into two files; consolidated in Phase 2.9.x cleanup since config + runtime are tightly coupled.) Initially consumed by gun yaw/pitch in `gun_unit.ipp`; Phase 2.9 promoted it into `ServoActuatorRole` so every servo consumer benefits.
- `gun_unit.h` + `.ipp` rewritten — consumes `GunSpec`, implements ROF band arbitration, smoke fan modes (off / continuous / puff-per-shot / puff-on-fire-active), yaw/pitch (Phase 2 via local `MotionProfile1D`, Phase 2.9 via role's integrator), manual override with 5 s auto-release (`kManualTimeoutMs`).
- `gunfx_service.h` + `.ipp` rewritten — claims all 10 slots, subscribes per-gun trigger (Boolean) + ROF + yaw + pitch inputs (Raw µs) via the InputDispatcher, dispatches MANUAL_SET / MANUAL_RELEASE / VERBOSE_STATUS_REQ, emits VERBOSE_STATUS at ~10 Hz per subscribed gun.
- YAML parser: `config/gunfx_config.h` (`GunFxConfigSchema` / `GunFxYamlConfig` — distinct include guard `HUBFX_GUNFX_CONFIG_YAML_H` to avoid collision with the firmware-side header). `applyGunFxConfig<>()` in `apply_hubfx_config.h`. New `kGunFx` ConfigStoreSlot wired in `hubfx_esp32s3.ino` setup().
- Firmware: **HubFx-6DA4 v2.11.0-hubfx build 189**, 752 KB flash, 71 % RAM.

### Phase 2.9 — Landed (2026-05-23)  ServoActuatorRole owns motion profile
- **The double-integrator anti-pattern eliminated.** Pre-2.9 setup had GunFx running its own `MotionProfile1D` inside `gun_unit.ipp::tickAxis()` and then handing the integrated µs to `ServoActuatorRole`, which re-slewed it with its minimal velocity-only ramp. Two integrators in series produced wrong slew behaviour.
- [`ServoActuatorRole`](../controllers/lib/sfx_board/roles/servo_actuator_role.h) now OWNS the `MotionProfile1D` integrator. Gains `setProfile(ServoMotionProfile)`, retires the legacy `_maxVelocity_us_per_s` ramp (the `setMaxVelocity_us_per_s` setter stays as a back-compat shim writing to `profile.maxSpeedUsPerSec`).
- `RoleServicePolicy::attachServoActuator` payload extended (Rule 11 append):
  `[minUs][maxUs][maxSpeed][reversed][centerUs][maxAccel][maxJerk]`. Old shorter payloads still attach with zero accel/jerk → velocity-only behaviour as before.
- `gun_unit.h/.ipp` drops its local `MotionProfile1D`; yaw/pitch tick becomes a one-line `commandServoTargetUs(port, target)` with stable-input change suppression. The role's integrator now does all shaping. (`GunAxis.profile` field in `GunSpec` is kept for the centerUs fallback but is no longer the canonical source — the role's profile, set via the port-attach YAML, is.)
- **Rule 42 generalised** to "Actuator mechanism on the role layer" — covers both element scaling AND motion profile under one principle. The condensed CLAUDE.md entry restates the rule.
- Firmware bumped to **v2.12.0-hubfx build 192**, 752 KB flash, 73 % RAM. Smoke-test passed.

#### Phase 2.9 also resolved one open issue from §8:
- ~~Issue #4 (`/gunfx.yaml` reload doesn't re-subscribe dispatcher inputs)~~ is unchanged — still a Phase 4 enhancement — but the related concern about the GunFx panel surfacing motion-profile fields is now moot: the profile lives on the port-role row instead. Phase 4 Studio surfaces it there.

### Phase 3 / Phase 4 — Landed
Phase 3 (Go API + Wails bindings — [app/go/client/gunfx.go](../app/go/client/gunfx.go), [app/go/protocol/gunfx/gunfx.go](../app/go/protocol/gunfx/gunfx.go)) and Phase 4 (Studio panel — [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte), the reference impl for Rules 38/39/41/45/47/48/49) shipped. The §8 items below are the remaining polish follow-ups.

---

## 8. Open issues from Phase 2 (deferred to follow-ups)

1. **Fan PWM duty currently uses a stopgap conversion** in `gun_unit.ipp::commandFanPct()` — multiplies pct × 40 to approximate the PCA9685's 4096-count duty range. Should be replaced by a clean `MOTOR_SET_PCT` role wire packet (cleaner intent surface) that the role decodes via its known `portMaxDuty`. Tracked in Phase 4 polish.
2. **Verbose status `smokeFanRunning` mirror is a stopgap** — `(smokeArmed && firing)` instead of reading actual fan-on state. Add an accessor on `GunUnit` (`fanRunning()`) in Phase 4.
3. **Trigger raw µs not currently observed** — the trigger is subscribed as Boolean only, so the verbose status's `triggerUs` field stays at 0. Phase 4 polish: dual-subscribe the trigger (Boolean for edge + Raw for mirror) if Studio needs the live µs trace.
4. **Config reload doesn't re-subscribe dispatcher inputs** — the per-gun subscriptions are set up at `begin()`, so changing ports via `/gunfx.yaml` reload requires a reboot. Phase 4 enhancement.
