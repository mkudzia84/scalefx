# LightFX Modernization (v0.10 → v0.12)

> **Scope:** LightFX firmware (Pico/RP2040), `serial/lightfx` protocol header, Go SDK
> + handlers, Studio LightFX tab + board-layout dialog, per-board YAML config, board
> image analyzer.
>
> **Driver:** Bring LightFX up to the same polish baseline as GearControl — battery
> monitoring, per-board YAML, modern tab layout, native PNG board dialog with
> measured hotspots.
>
> **Versions touched:** Firmware `0.10.0 → 0.12.0`, BUILD `~30 → 38+`.

---

## 1. Why this exists

LightFX was the oldest of the slave controllers and lagged GearControl in three
visible areas:

1. **No battery monitoring** — the VSYS divider was wired but unused.
2. **Studio tab was bottom-row sliders + a generic input bar.** No status tag,
   no live channel pip strip, no "View Diagram" hook, no save-config dialog
   wired through the verifier, no auto-load on connect.
3. **No board-layout dialog.** GearControl's diagram was the most-loved UI
   feature; LightFX users had to read the firmware to know which JST was CH1.

This pass closes those gaps. It is the reference template for the next
controller (GunFX) when it gets its own tab.

---

## 2. Phase summary

| # | Area                       | Outcome                                                                 |
|---|----------------------------|-------------------------------------------------------------------------|
| 1 | Firmware battery cutoff    | New packet `0x5E BATTERY_AUTO_CUTOFF`, RP2040 ADC on GP29, broadcast    |
|   |                            | extended with battery_mV + flags (Rule 11 append-only).                 |
| 2 | Go protocol mirror         | `protocol/lightfx/lightfx.go` constants, `api/lightfx.go` typed SDK,    |
|   |                            | `engine/handlers/lightfx/{types,format,handler}.go` (Rule 19/20).       |
| 3 | Per-board YAML config      | `defaultPath()` → `/lightfx.yaml`, schema gains `battery` block,        |
|   |                            | `BoardConfigDriver<LightConfig>` + `autoLoadOnConnect` (Rule 26).       |
| 4 | Studio tab redesign        | GearControl-style title bar, top-span status card, channel pip strip,   |
|   |                            | live PWM band map + cursor, save-dialog wired through verifier.         |
| 5 | Board-layout dialog        | New `LightFxBoardDialog.svelte` over `media/lightfx_2d.png`. Hotspots   |
|   |                            | measured by `tools/analyze_lightfx_image.py`, NOT eyeballed.            |
| 6 | Slave-mode auto-inference  | Removed manual toggle. `slaveMode = (controllerType === 'hubfx')`.      |
| 7 | Docs                       | This file + controller README version bumps + CLAUDE.md pointer.        |

---

## 3. Firmware: battery cutoff (Phase 1)

### 3.1 Wire format additions

`controllers/lib/sfx_serial/serial/lightfx/lightfx.h` (and its Go mirror):

| Type | Name                  | Direction | Payload                                  |
|------|-----------------------|-----------|------------------------------------------|
| 0x5E | `BATTERY_AUTO_CUTOFF` | client→fw | `enabled:u8`                             |

`STATUS` broadcast appended (Rule 11 — old clients decode the new bytes as 0):

```
... existing 21 bytes ...
battery_mV          : u16 LE        // pack voltage in mV
batteryFlags        : u8            // bit0=lowTriggered, bit1=autoCutoffArmed
```

### 3.2 Firmware changes

- New singleton-style `BatteryMonitor` over `controllers/lib/sfx_peripherals/battery`
  reads `analogRead(PIN_VSENSE)` (GP29) on a 50 ms timer, applies the resistor-divider
  ratio, and exposes `voltage_mV()` + `lowTriggered()`.
- On `BATTERY_AUTO_CUTOFF` enable, when `voltage_mV()` falls below the chemistry's
  per-cell cutoff (LiPo 3.2 V/cell, etc.), all 8 LED channels are explicitly
  disabled with a distinct `ERROR_BATTERY_CUTOFF`. They stay disabled until
  `LED_RESET / LED_ENABLE` re-arms them — **not auto-recovering** on voltage
  rebound (Rule 13: explicit reset required).
- `lightfx_pico.ino:setup()` follows the standard 6-step server boot sequence
  (Rule 7). `batteryMonitor.begin(PIN_VSENSE)` lives at step 2 (hardware init),
  the cutoff hook in `core().onStatusData(...)` at step 4.

### 3.3 Capability bit

LightFX advertises `CoreCapability::CONFIG` (1<<5). It does NOT advertise
`FLASH` / `SD` / `AUDIO` — Studio's file manager will not probe them
(see CLAUDE.md `Capabilities` note).

---

## 4. Per-board YAML (Phase 3)

`/lightfx.yaml` joins the per-board config family — same path-derivation logic
as GearControl/HubFX. The schema's `defaultPath()` is the **single source of
truth**; nothing else hardcodes the filename.

`LightConfig` now requires a `battery` block:

```yaml
battery:
  autoCutoff: true
  chemistry: lipo      # lipo | liion | nimh
  cellCount: 0         # 0 = auto-infer from voltage
```

`HubFxTab.svelte:buildHubFxLightConfig()` was patched in the same commit to
emit a default `battery` block — without that, the type check broke once
`battery` became required.

### 4.1 Hub-side `/lightfx.yaml` (per-domain split, v0.36.0)

On HubFX the same `/lightfx.yaml` schema lives on **hub flash** alongside
`/hubfx.yaml`, `/enginefx.yaml`, and `/gunfx.yaml` — one file per domain
(Rule 26). The SD card holds audio samples only, never config.

The hub schema (`HubLightFxConfigSchema` in
[lightfx_hub_config.h](../controllers/hubfx/esp32s3/src/config/lightfx_hub_config.h))
wraps the **shared** `LightProgramConfig` walker (`light_program_config::fields`)
but **omits the `battery` block** — battery chemistry / cell count arrive at
runtime via the `BATTERY_CONFIG` (0xEE) packet, and the INA226 rail channel
is hardcoded on the hub. The slave-side `LightFxConfigSchema` keeps battery
because the slave persists it.

Wire dispatch goes through
[multi_config_server.h](../controllers/lib/sfx_config/server/multi_config_server.h)
(`MultiConfigServer` + `IConfigStoreFacade` / `ConfigStoreFacadeT<TStore>`).
`CONFIG_RELOAD` / `CONFIG_SAVE` / `CONFIG_STATUS` (0x90 / 0xAC / 0x91) all
accept the existing optional `[pathLen:u8][path:str]` payload and route by
exact path match against each store's `defaultPath()`. Reload without a path
reloads every store; save without a path NACKs (ambiguous). Standalone slaves
are unaffected — they keep `ConfigServerT<TStore>`.

When the hub's `/lightfx.yaml` reloads, its typed `onLoaded` callback calls
`pushLightFxConfigToSlave(cfg, *slaveLightFx())` — a plain function that
walks `LightProgramConfig` and drives `LightFxClient` over USB CDC. There
is exactly one LightFX slave per hub, so no router/orchestrator/applier
indirection is needed.

### 4.2 Slave attach → auto-INIT → config push (v0.38.0)

Detection / activation / config push are driven by the `UsbRegistry`
lifecycle callbacks, not main-loop polling. End-to-end flow on USB plug-in:

1. **USB enumerate** — `EspUsbHost` fires `onMount`; `SlaveManager::process()`
   schedules a discovery scan.
2. **IDENTIFY** — `tryIdentifySlave()` sends IDENTIFY (non-destructive),
   matches by `INIT_READY.serverName` against the descriptor table, binds
   the typed client to the USB index, and calls `setConnected(type, true)`.
3. **Auto-INIT** — when `SlaveDescriptor.autoInit == true` (set on all three
   slave descriptors in HubFX's `initSlaveManager()`), the manager
   immediately sends `INIT(SLAVE)`, awaits `INIT_READY`, and calls
   `setReady(type, true)`.
4. **`onReady` callback fires** — `setReady` is edge-triggered. Per-type
   callbacks registered via `registry.onReady(SlaveType::LightFX, …)` run
   inline. The LightFX callback calls
   `pushLightFxConfigToSlave(lightConfig.data(), *static_cast<LightFxClient*>(c))`
   which sends `setMasterBrightness` → per-servo `servoSettings/servoSet` →
   `landingLightUnbind(0)` → per-group `landingLightBind` over the wire.
5. **`onDisconnect` callback fires** on USB unmount or any
   `setReady(false)` (`SLAVE_INIT` re-init, `REBOOT`, `SHUTDOWN`,
   `BOOTSEL`). LightFX has no held state to release, so no callback is
   registered — the registry's auto-log line is the only side effect.

`UsbRegistry::fireReady` / `fireDisconnect` log every transition at INFO
level, so per-board callbacks only need to register when they have actual
side effects. The previous router/orchestrator/applier stack
(`controllers/lib/sfx_boards/applier/` + `lib/sfx_boards/lightfx/applier/`)
is gone — the LightFX slave is one client, so a plain function does the job.

GunFX and GearControl have no hub-side YAML to push today, so no `onReady`
callback is registered for them. When that changes (GunFX audio routing,
GearControl input mappings), add a `pushGunFxConfigToSlave(cfg, client)` /
`pushGearControlConfigToSlave(cfg, client)` function and wire one
`onReady` callback in the same shape as LightFX. No new abstraction.

---

## 5. Studio tab redesign (Phases 4 + 6)

[LightFxTab.svelte](../app/go/studio/frontend/src/lib/tabs/LightFxTab.svelte) now
mirrors [GearControlTab.svelte](../app/go/studio/frontend/src/lib/tabs/GearControlTab.svelte)
exactly:

- **Title bar** — `↻ Reload  /  💾 Save…  /  🗺 View Diagram` in `.title-actions`,
  plus an error-/warning-count badge driven by the live verifier.
- **Direct-mode warning banner** — appears once when connected directly to a
  LightFX board, dismissible permanently via `localStorage` key
  `lightfx.direct.warning.dismissed` (mirrors GearControl).
- **Top-span status card** — 2-column grid:
  - **Left** — *Input block.* PWM band map (one tinted region per program) +
    a live cursor at `liveInput_us`. In direct mode the source label reads
    `Servo 1 · RC PWM`; in slave mode the channel selector (`CH 1-24`)
    appears in its place.
  - **Right** — *Master / All-Off / channel pips.* Master-brightness slider
    + Set + All Off + a 4×2 strip of pips mirroring `ledLive` (off / ▶ / `bri%`).
- **Header tag** — `▶ <program name>` when a band matches, `via HubFX · CH n`
  in slave mode, `No band match @ X µs` when input is outside all bands,
  `— No signal` otherwise.

### 5.1 Slave-mode is auto-inferred (Phase 6)

The old "Slave Mode" checkbox was removed in favour of:

```ts
$: slaveMode = $connectionInfo.controllerType === 'hubfx'
```

This matches GearControl's behaviour and removes a step where the user could
mismatch the toggle against the real connection. All downstream logic
(`controlsDisabled`, channel-group policy options, input-block routing) reads
`slaveMode` exactly as before.

---

## 6. Board-layout dialog (Phase 5)

[LightFxBoardDialog.svelte](../app/go/studio/frontend/src/lib/dialogs/LightFxBoardDialog.svelte)
follows the GearControlBoardDialog pattern:

- Native PNG (`media/lightfx_2d.png`, mirrored into
  `frontend/src/assets/images/lightfx_2d.png`) used as the SVG background.
- viewBox = native PNG resolution (706 × 445), so hotspot rectangles are in
  raw image-pixel coordinates.
- Each hotspot is a `<g>` with a translucent fill, a label pill, a sublabel
  (pin GPx / footprint id), and a 1-2 line function readout.
- LED hotspots react to `ledLive` — amber when lit/sequencing, dashed grey
  when disabled.
- Battery hotspot reads chemistry / cells / voltage and shows the same
  display logic as the tab's battery card.
- Servo 1 shows the live RC pulse-width when in direct mode.

### 6.1 The analyzer

`tools/analyze_lightfx_image.py` is the LightFX twin of
`tools/analyze_gearcontrol_image.py`. **Do not eyeball coordinates** — re-run
the analyzer if `media/lightfx_2d.png` is regenerated:

```bash
python tools/analyze_lightfx_image.py
```

The pipeline:

1. **White-mask pass** (`R,G,B ≥ 215`) → flood-fill connected components.
   The 8 LED JST housings are the largest white components (~60 × 66 px) on
   the top + bottom edges. USB-C is the largest white component on the right
   edge.
2. **Magenta-mask pass** (R high, B high, G low — same thresholds as
   GearControl) → finds the small SIG silkscreen dots beside each servo header.
   Each row's dot anchors the hotspot's left edge; the box is then extended
   right to cover the full 3-pin housing.
3. **Grey-mask pass** (`90 ≤ R,G,B ≤ 215`, low chroma) → finds the round
   silver/copper battery solder pads. The two pads are merged via
   `union_bbox` into a single combined BAT hotspot.

Output is paste-ready bbox coordinates that go into the dialog's `hotspots[]`
array. The header comment in `LightFxBoardDialog.svelte` documents every
measured value next to its hotspot definition.

### 6.2 Adapting for a future board

When GunFX or another controller gets a `*_2d.png`:

1. `cp tools/analyze_lightfx_image.py tools/analyze_<board>_image.py`.
2. Update `PNG = ...` and the `in_region(...)` filter rectangles for the new
   board's port positions.
3. If the silkscreen uses a colour other than white / magenta / grey, add a
   new mask function (mirror `magenta_mask` / `grey_mask`).
4. Run, paste bboxes into a new `<Board>BoardDialog.svelte` modelled on
   `LightFxBoardDialog.svelte`.

---

## 7. CSS layout primitives now reused across tabs

The redesign expanded the cross-tab style vocabulary. These classes are owned
by the tab files (not `style.css`) but are intentionally identical between
GearControlTab and LightFxTab — copy them verbatim to a new tab:

| Class                    | Purpose                                                        |
|--------------------------|----------------------------------------------------------------|
| `.title-actions`         | Right-aligned action buttons inside `.tab-title-bar`           |
| `.direct-warning`        | Yellow banner shown when wired direct to a slave controller    |
| `.top-span` + `.top-grid`| 2-column status card spanning both content columns             |
| `.header-tag.{ok,warn,error}` | Live status pill in card headers                          |
| `.lfx-band-track` / `.lfx-band-region` / `.lfx-band-cursor` | PWM band map widget |
| `.lfx-channel-strip` + `.lfx-pip` + `.pip-{dot,name,badge}` | Per-channel pips |

`@keyframes pulse` is duplicated per tab (small enough not to warrant a global).

---

## 8. Files touched (reference)

```
controllers/lightfx/pico/src/lightfx_pico.ino     ← battery init, cutoff hook, version bump
controllers/lightfx/pico/README.md                 ← v0.12.0, battery section
controllers/lib/sfx_serial/serial/lightfx/lightfx.h ← 0x5E + STATUS extension
controllers/lib/sfx_peripherals/battery/*          ← shared BatteryMonitor (used by GearControl too)

app/go/protocol/lightfx/lightfx.go                 ← constants mirror
app/go/api/lightfx.go                              ← typed SDK
app/go/engine/handlers/lightfx/{types,format,handler,parsers}.go ← three-file split (Rule 19/20)

app/go/studio/frontend/src/lib/config/light-verifier.ts ← battery block in LightConfig
app/go/studio/frontend/src/lib/config/config-yaml-gen.ts ← battery emit/parse
app/go/studio/frontend/src/lib/tabs/LightFxTab.svelte    ← full redesign
app/go/studio/frontend/src/lib/tabs/HubFxTab.svelte      ← buildHubFxLightConfig() battery default
app/go/studio/frontend/src/lib/dialogs/LightFxBoardDialog.svelte ← new
app/go/studio/frontend/src/assets/images/lightfx_2d.png  ← copy of media/lightfx_2d.png

tools/analyze_lightfx_image.py                     ← new (sibling of analyze_gearcontrol_image.py)
media/lightfx_2d.png                               ← board render (source asset)

instructions/12-LIGHTFX-MODERNIZATION.md           ← this file
```

---

## 9. Things to NOT do when extending

- **Do not embed the YAML filename in client code.** Always read it from
  `defaultPath()` / `configPathFor(controllerType)` (Rule 26).
- **Do not eyeball board hotspots.** Re-run / extend `tools/analyze_*_image.py`
  and paste the output. Eyeballed coordinates drift the moment the PNG is
  re-rendered.
- **Do not bring the slave-mode toggle back.** It was a footgun — the user
  could mismatch it against the connection state. Auto-infer from
  `controllerType` is the canonical pattern.
- **Do not break Rule 11 on the STATUS broadcast.** `battery_mV` /
  `batteryFlags` are append-only; never reorder, never insert in the middle.
