# 23 — Studio Widget Catalog

> One-stop reference for every reusable Studio UI pattern. When building
> a new tab or panel, **start here** — find the closest pre-existing
> widget, copy the markup, and only deviate when the design rule
> explicitly allows it. Cross-references to the formal rules in
> [`.github/copilot-instructions.md`](../.github/copilot-instructions.md);
> this file is the practical handbook with full snippets.
>
> Companion docs:
> [20-STUDIO-DEVICE-MODEL.md](20-STUDIO-DEVICE-MODEL.md) (the data model
> these widgets render),
> [21-STUDIO-ENGINEFX-PANEL.md](21-STUDIO-ENGINEFX-PANEL.md) (the
> canonical reference panel implementing most of these patterns).

---

## How to use this catalog

When a user story comes in ("add a temperature graph to the heater
tab"), the path is:

1. **Find the rule** that governs the widget — Rule 34 for layout, 35
   for validation, 36 for channel-gated triggers, 42 for actuator
   mechanism, 43 for named channels.
2. **Find the widget pattern** here (this file).
3. **Find the canonical implementation** in the reference panel cited
   at the top of each pattern.
4. **Copy the markup + the CSS classes**, change only what your panel
   needs. Don't re-roll the styling — Rule 34 says one design language.

When the catalog doesn't have your widget yet, build it once in the
nearest existing panel, then come back and add it here so the next
panel can crib it.

---

## Index

1. [Panel anatomy](#1-panel-anatomy) — card / header / status row / sections
2. [Status row](#2-status-row) — state pill + Apply + operate cluster (Rule 35, Rule 48)
3. [Field row](#3-field-row) — label + input + unit + action buttons (Rule 34)
4. [Button cluster](#4-button-cluster) — browse left, clear right, alignment slots (Rule 34)
5. [Validation surfacing](#5-validation-surfacing) — error/warn at field + section + Apply (Rule 35)
6. [Channel-toggle cluster](#6-channel-toggle-cluster) — channel + threshold + hyst + live bar overlays (Rule 36 — `ChannelToggleCluster.svelte`)
7. [Named-channel picker](#7-named-channel-picker) — pick from `/hubfx.yaml inputs[]` by name (Rule 43)
8. [Cross-board port picker](#8-cross-board-port-picker) — output ports with rail-voltage labels + operator-alias name (Rule 34, Rule 37)
9. [File picker](#9-file-picker) — `pickFile({ targets })` parametrised by backend (Rule 34)
10. [Dirty-flag indicator + draft store pattern](#10-dirty-flag-indicator) — `engineConfig` / `engineDraft` / `engineDirty` (per-effect) + signal-based `markHubDirty()` (hub IO)
11. [Servo widget + calibration dialog](#11-servo-widget--calibration-dialog) — compact row (REV toggle + ⚙ Calibrate popup), profile stored per-port (Rule 42 / 44)
12. [Element scaling editor](#12-element-scaling-editor) — heater / DC-motor element voltage + scaling, on the port-role row (Rule 42)
13. [Verbose-status event subscriber](#13-verbose-status-event-subscriber) — live ~10 Hz mirror (Phase 4 staple for manual-mode panels)
14. [Add/remove list (ROF items, gun units, …)](#14-addremove-list) — operator-authored arrays in a draft store
15. [Sound row + speaker-routing button](#15-sound-row--speaker-routing-button) — shared `SoundRow.svelte` for every WAV-path picker (Rule 47)
16. [Multi-band channel cluster](#16-multi-band-channel-cluster) — `ChannelBandCluster.svelte` for N-of-M selectors (Rule 38)
17. [Operational action cluster](#17-operational-action-cluster) — `.op-cluster` split-button for primary-action + optional picker + Stop (Rule 48)
18. [Modular config sources + global Apply](#18-modular-config-sources) — `DirtySource` + `ConfigToolbar` aggregate (Rule 46)
19. [Servo I/O status widget](#19-servo-io-status-widget) — live signal-in bar + servo-out track (RED actual + YELLOW target lines), `ServoIoWidget.svelte` (Rule 42)

---

## 1. Panel anatomy

The canonical effect-panel layout is documented in full in
[21-STUDIO-ENGINEFX-PANEL.md § 1](21-STUDIO-ENGINEFX-PANEL.md). The
shape, top-to-bottom:

```
┌─ Card (.card) ──────────────────────────────────────────────────────┐
│ ▣ Title                                          [☑ Enabled toggle] │  ← .card-header (Rule 34)
├─────────────────────────────────────────────────────────────────────┤
│ STATE [pill]  [resolve errors above]  [Apply] ‖ [▶ Start] [■ Stop] │  ← status-row  (Rule 35)
│                                                                     │
│ ┌─ Channel-setup cluster ───────────────────────────────────────┐  │
│ │ … channel selector → trigger params → live bar with markers  │  │  ← Rule 36
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                     │
│ ── SECTION NAME ─────────────────────────────────────────────────── │  ← .section-head
│ field row …                                                         │
│ field row …                                                         │
│                                                                     │
│ ── ANOTHER SECTION ─────────────────────────────────────────────── │
│ …                                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

**Required classes** (defined once in [`style.css`](../app/go/studio/frontend/src/style.css), never re-skinned):

- `.card` — outer container
- `.card-header` — title + actions on one row
- `.tab-content` — outermost wrapper providing padding + overflow
- `.section-head` — visually divides logical sections within a card
- `.form-row` — label + control row
- `.form-grid.cols-2` (or `.cols-3`) — stacked field grid
- `.field-label`, `.field-input` (+ `.narrow` / `.wide`), `.field-hint`
- `.empty-state` — "nothing here yet" banner
- `.banner.err` / `.banner.note` — error / informational ribbon

Reference: [EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte).

---

## 2. Status row

The operational header inside an effect card: state pill (current
state), dirty-flag indicator, Apply, divider, operational buttons.
Both Apply and the operational buttons gate on `busy || !dirty || hasErrors`
(Rule 35).

```svelte
<div class="status-row">
    <div class="status">
        <span class="status-label">State</span>
        <span class="state-pill state-{state}">{state}</span>
    </div>
    <div class="controls">
        <span class="dirty-flag" class:on={$dirty} class:err={hasErrors}>
            {hasErrors ? 'resolve errors above' : $dirty ? 'unapplied changes' : 'in sync'}
        </span>
        <button class="small primary" on:click={onApply}
                disabled={busy || !$dirty || hasErrors}
                title={hasErrors ? 'Fix validation errors before applying' : 'Write … + reload'}>Apply</button>
        <span class="ctrl-sep" aria-hidden="true"></span>
        <button class="small" on:click={onStart}
                disabled={busy || $dirty || hasErrors}>▶ Start</button>
        <button class="small" on:click={onStop} disabled={busy}>■ Stop</button>
    </div>
</div>
```

CSS — copy the `.dirty-flag`, `.ctrl-sep`, `.state-pill` rules from
[`EnginePanel.svelte`](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte).
Reference rule: **Rule 35 §6, §7**.

---

## 3. Field row

A label + control row.

```svelte
<div class="form-row">
    <span class="field-label">Threshold</span>
    <input class="field-input narrow" type="number" min="800" max="2200" step="10"
           value={cfg.thresholdUs}
           on:change={(e) => { cfg.thresholdUs = numValue(e); mark() }} />
    <span class="unit">µs</span>
    <span class="trigger-pm">±</span>
    <input class="field-input narrow" type="number" min="0" max="500" step="5"
           value={cfg.hysteresisUs}
           on:change={(e) => { cfg.hysteresisUs = numValue(e); mark() }} />
    <span class="unit">µs hysteresis</span>
</div>
```

**Conventions:**

- Verb-led field labels read like sentences (`Fires when channel ≥`,
  `Brightness`, `Channel`) — never raw `Threshold (µs)`.
- Units go in `.unit` spans (small dim mono font); never in the input
  placeholder.
- The `±` between paired fields uses `.trigger-pm` (centered, bold
  dim).
- A control should not exceed 200 px wide. Use `.field-input.narrow`
  (~70 px) for numbers, `.field-input.wide` for paths / dropdowns.

Reference rule: **Rule 34**.

---

## 4. Button cluster

When a row has multiple action buttons, they line up in canonical
order: browse / picker / sub-dialog (`…`, `⚙ Calibrate…`) leftmost,
clear / destructive (`Clear`, never `None`) rightmost. Required rows
reserve the rightmost slot with a `visibility: hidden` spacer so the
columns line up across stacked rows.

```svelte
<div class="form-row">
    <input class="field-input wide" type="text" placeholder="/sounds/…" value={cfg.sound} … />
    <button class="small btn-slot" on:click={browse}>…</button>
    {#if optional}
        <button class="small btn-slot" on:click={clear} disabled={!cfg.sound}>Clear</button>
    {:else}
        <span class="btn-slot btn-spacer" aria-hidden="true"></span>
    {/if}
</div>
```

CSS:

```css
.btn-slot   { width: 64px; min-width: 64px; box-sizing: border-box;
              text-align: center; flex-shrink: 0; }
.btn-spacer { display: inline-block; height: 28px; visibility: hidden; }
```

Reference rule: **Rule 34** (Row-button order + alignment). Reference
panel: [EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte)
sound rows.

---

## 5. Validation surfacing

Errors land in **three** places: at the field/row, at the section
header, and on the Apply button.

**Field/row** — red border + light-red background + inline `⚠ <reason>`:

```svelte
<div class="sound-row" class:invalid={!!err}>
    <div class="form-row">…</div>
    {#if err}<div class="row-err">⚠ {err}</div>{/if}
</div>
```

```css
.sound-row.invalid { border-color: var(--error); background: rgba(255,80,80,0.06); }
.row-err           { font-size: 11px; color: var(--error); margin: 3px 0 0 80px;
                     font-family: var(--font-mono); }
```

**Section head** — turns red + grows a chip:

```svelte
<div class="section-head" class:section-error={soundsHaveErrors}>
    Sounds {#if soundsHaveErrors}<span class="section-err-tag">missing files</span>{/if}
</div>
```

**Apply button** — `disabled={busy || !$dirty || hasErrors}` with the
`.dirty-flag` switching to a red `resolve errors above` label (see
§2). Operational actions like Start / Trigger / Test gate the same way.

**Read AND write** — the validator runs both when the operator types
AND when the config is loaded from the device. A malformed YAML pulled
from flash shows the same red rows the operator would see while
editing.

Reference rule: **Rule 35**. Reference panel:
[EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte)
sound-row validation.

---

## 6. Channel-toggle cluster

The canonical 4-row cluster for any RC-channel-gated boolean action
(engine throttle, gun trigger, future LightFx mode-switch …).
Factored into the **shared component**
[`ChannelToggleCluster.svelte`](../app/go/studio/frontend/src/lib/components/ChannelToggleCluster.svelte) —
panels import it, never inline the 60-line markup again (Rule 36
violation if they do).

```svelte
<script lang="ts">
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'
</script>

<ChannelToggleCluster
    channelLabel="Driver channel"
    emptyOption="— manual only —"
    options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
    inputId={cfg.toggle.input}
    thresholdUs={cfg.toggle.thresholdUs}
    hysteresisUs={cfg.toggle.hysteresisUs}
    liveUs={liveUs?.us ?? null}
    liveValid={liveUs?.valid ?? false}
    busy={busy}
    actionVerb="Fires"
    onChange={(n) => {
        cfg.toggle.input         = n.inputId
        cfg.toggle.thresholdUs   = n.thresholdUs
        cfg.toggle.hysteresisUs  = n.hysteresisUs
        mark()
    }} />
```

The widget owns the four-row layout (selector → verb-led trigger
settings → `.bar.tall` with threshold mark + hysteresis band + live
fill + NO-SIGNAL stripe → colour-coded legend). The caller supplies
the named-channel option list, the live µs lookup, and the change
handler that pushes back into its draft.

**Colour semantics** (the widget paints these; do NOT override):
- `--success` (green) — live fill + legend `live` label
- `--error`   (red)   — threshold marker + legend `threshold` label
- `--warning` (amber) — hysteresis band + legend `hysteresis` label
- striped track — NO SIGNAL state (live frame invalid)

Reference rule: **Rule 36**. Call sites:
[EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte) (engine on/off),
[GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) (per-gun trigger).
Full walkthrough: [21-STUDIO-ENGINEFX-PANEL.md § 2](21-STUDIO-ENGINEFX-PANEL.md).

---

## 7. Named-channel picker

**Rule 43**: effects pick channels by name from `/hubfx.yaml`'s
`inputs:` block. Never raw port + channel.

The shared helper builds a flat option list from the device model.
Both EnginePanel and GunFxPanel define it as a script-local `chanOpts`
derived store; the code is small enough to copy rather than abstract:

```svelte
<script lang="ts">
    type ChanOpt = { fnId: string; label: string }
    $: chanOpts = collectChannels($deviceModel)
    function collectChannels(_dm: typeof $deviceModel): ChanOpt[] {
        const fns = new Map($deviceModel.channelFunctions.map(f => [f.id, f.label] as const))
        const out: ChanOpt[] = []
        for (const inp of $deviceModel.inputs) {
            for (const c of inp.channels) {
                if (c.function === 'unassigned') continue
                out.push({
                    fnId: c.function,
                    label: `CH${c.channel + 1} · ${fns.get(c.function) ?? c.function}`,
                })
            }
        }
        return out
    }
</script>

<div class="form-row">
    <span class="field-label">Trigger channel</span>
    <select class="field-input wide" value={cfg.input}
            on:change={(e) => { cfg.input = selValue(e); mark() }}>
        <option value="">— none (manual only) —</option>
        {#each chanOpts as o}
            <option value={o.fnId}>{o.label}</option>
        {/each}
    </select>
</div>
```

The DTO field holding the choice is a **single `string`** (the
function id), not a `PortRef` + channel pair. Firmware-side resolution
happens in the apply translator (`applyXxxConfig<>(board, cfg, hub)`
calls `findInputByName(hub, name)`).

Reference rule: **Rule 43**. References:
[EnginePanel.svelte chanOpts](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte),
[GunFxPanel.svelte chanOpts](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte).

---

## 8. Cross-board port picker

**For OUTPUT ports** (muzzle flash LED, yaw/pitch servo, smoke heater) —
those ARE per-effect wiring, so they DO live in the effect's YAML as
`PortRef`. The picker iterates the unified port table across every
connected board (`$deviceModel.ports`) and groups by board:

```svelte
<select class="field-input wide" value={portRefToKey(cfg.muzzleFlashPort)}
        on:change={(e) => setPort('muzzleFlashPort', parsePortOption(selValue(e), 'pwm'))}>
    <option value="">— none —</option>
    {#each portsOfKind('pwm', 'output') as p}
        <option value={refOptValue(p)}>{refOptLabel(p)}</option>
    {/each}
</select>
```

Helpers:

```ts
function portsOfKind(kindName, direction) {
    return $deviceModel.ports.filter(p => p.kindName === kindName && p.direction === direction)
}
function refOptValue(p: Port): string { return `${p.ref.guid}|${p.kindName}|${p.ref.index}` }
function refOptLabel(p: Port): string {
    // Show the operator-assigned alias FIRST when set — that's the
    // name the operator thinks in.  Falls back to the silkscreen
    // hardware label (CH3, IN1, …) when no alias is configured.
    // Reactive on $deviceModel.ports, so renaming a port on the IO
    // tab propagates through every picker without a panel reload.
    const rail  = formatPortRail(p.voltageMv)
    const alias = p.name && p.name.trim()
    const head  = alias ? `${alias} (${p.hardwareName})` : p.hardwareName
    return `${p.boardName ?? 'Hub'} · ${head}${rail ? ` · ${rail}` : ''}`
}
function portRefToKey(r: PortRefT): string {
    if (!r || !r.guid || !r.kind) return ''
    return `${r.guid}|${r.kind}|${r.idx}`
}
```

**Required:** label includes the port's rail voltage (Rule 37) so the
operator spots voltage mismatches in the picker.

> **Don't** use this for input channels — those are named (Rule 43).
> This pattern is only for output ports.

Reference rule: **Rule 34** (cross-board picker labels), **Rule 37**
(rail voltage in labels). Reference panel:
[GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte).

---

## 9. File picker

`pickFile({ targets })` is the only way to pick a file path. The
backend (`flash` / `sd` / `both`) is **mandatory** — sound files live
on SD, configs live on flash; the picker hides the disallowed tab
entirely so the operator can't accidentally browse the wrong
filesystem.

```ts
import { pickFile } from '../filepicker'

async function browsePath(field) {
    const p = await pickFile({ targets: 'sd' })   // always pass the narrowest target
    if (p != null) { cfg[field] = p; mark(); scheduleValidate() }
}
```

Reference rule: **Rule 34**. Reference:
[filepicker.ts](../app/go/studio/frontend/src/lib/filepicker.ts).

---

## 10. Dirty-flag indicator + draft store pattern

Every effect tab maintains a **triad** of stores:

```ts
export const xxxConfig = writable<XxxConfigT>(emptyConfig())   // mirrors firmware
export const xxxDraft  = writable<XxxConfigT>(emptyConfig())   // what the operator typed
export const xxxDirty  = derived(
    [xxxConfig, xxxDraft],
    ([cfg, draft]) => !deepEqual(cfg, draft),
)
```

**Critical pitfall — the `wasDirty` snapshot.** Loaders MUST snapshot
the dirty flag BEFORE mutating `xxxConfig`, otherwise the derived flag
becomes `true` on assignment and the loader never seeds the draft:

```ts
async function loadXxxConfig() {
    const n = await DownloadConfig()
    const wasDirty = get(xxxDirty)     // <-- SNAPSHOT FIRST
    xxxConfig.set(n)
    if (!wasDirty) xxxDraft.set(structuredClone(n))
}
```

Save path runs in reverse and re-mirrors:

```ts
async function saveXxxConfig() {
    const cfg = get(xxxDraft)
    await UploadConfig(cfg)
    xxxConfig.set(structuredClone(cfg))   // optimistic — server reload is fire-and-confirm
}
```

Reference rule: **Rule 35**. References:
[effects.ts (engine draft)](../app/go/studio/frontend/src/lib/effects.ts),
[gunfx.ts (gun draft)](../app/go/studio/frontend/src/lib/gunfx.ts).
Full walkthrough:
[21-STUDIO-ENGINEFX-PANEL.md § 5](21-STUDIO-ENGINEFX-PANEL.md).

---

## 11. Servo widget + calibration dialog

**Rule 42 storage + Rule 44 editing surface:** the profile DATA lives
in `/hubfx.yaml`'s `ports[]` block (canonical, per-port, because it's
a property of the physical servo — min/max are mechanical end-stops,
speed/accel match the servo's spec sheet).  The EDITING surface is the
same shared `ServoWidget` in two places, both opening the same dialog
and persisting via the same `SetPortProfile` path (no duplicate data):
- **Feature panels** (GunFx Turret section, future EngineFx servo
  binding) — tune the servo where you wire the feature.
- **The IO tab** — `ServoWidget` renders on a full-width `.servo-cal-row`
  directly under each hub-local servo port row in `PortRoleTab.svelte`
  (always visible, no expander), so you can calibrate a servo at the port
  even before it's bound to any effect.  It's a SUB-ROW, not inline on the
  dense port row — the port row is a non-wrapping flex line that would clip
  the widget off the right edge.  (Heater / DC-motor element scaling stays
  under the `⚙ Tune` expander in `PortRoleConfig.svelte`; servos are NOT in
  `hasRoleConfig`.  Hub-local ports carry GUID `""` — the wire's "this is
  the hub" sentinel; the CLI's `[6D60]` label is derived from the device
  name, not the GUID field.)  Added 2026-06-01.

> ⚠️ **Reactivity:** a panel that feeds `ServoWidget` via a helper like
> `profileForPort(port)` MUST make that helper reactive on `$deviceModel`
> (rebuild the closure in a `$:` block — the `makeLiveUsFor` pattern), or
> the summary freezes on the pre-Save profile after a Calibrate→Save.
> A plain function that reads `$deviceModel` *inside its body* is invisible
> to Svelte. The IO tab is safe because it reads `p.profile` straight from
> the `{#each $deviceModel.ports}` it already iterates.

> The old inline `ServoProfileEditor.svelte` (a sectioned min/max/
> speed/accel/jerk form) was **retired** (2026-05-24).  The feature row
> now stays compact: a one-line [`ServoWidget.svelte`](../app/go/studio/frontend/src/lib/components/ServoWidget.svelte)
> with a `↔ Reversed` toggle + a `⚙ Calibrate…` button that opens the
> shared live-jog popup [`ServoCalibrationDialog.svelte`](../app/go/studio/frontend/src/lib/dialogs/ServoCalibrationDialog.svelte)
> (Rule 28).  Saving in the popup persists via `SetPortProfile` — the
> same wire path as before.

**Component:** [ServoWidget.svelte](../app/go/studio/frontend/src/lib/components/ServoWidget.svelte)
— compact form row; opens [ServoCalibrationDialog.svelte](../app/go/studio/frontend/src/lib/dialogs/ServoCalibrationDialog.svelte)
(live jog + Limits / Direction / Motion Profile + debounced cfg push,
Save/Cancel) on `⚙ Calibrate…`.

```svelte
<div class="form-row">
    <span class="field-label">Motion profile</span>
    <ServoWidget
        port={axis.servoPort}
        portLabel="{which} servo"
        profile={profileForPort(axis.servoPort)}   <!-- $deviceModel.ports[i].profile -->
        busy={busy} />
</div>
```

```ts
// Frontend helper (one per panel):
function profileForPort(port: PortRefT): ProfileT {
    // walks $deviceModel.ports for a matching ref, returns the
    // attached profile or a fresh default
}
// ServoCalibrationDialog Save → SetPortProfile(guid, kind, idx, prof):
// updates the overlay, live-pushes via Roles.ServoSetProfile, marks
// /hubfx.yaml dirty for the next SaveHubConfig.
```

Props (ServoWidget):
- `port` — `PortRefT` for the servo this row drives
- `portLabel` — human label shown on the row + dialog header
- `profile` — `ServoMotionProfileT` ({minUs, maxUs, centerUs, reversed,
  maxSpeedUsPerSec, maxAccelUsPerSec2, maxJerkUsPerSec3}); pass by value
- `busy` — disables the toggle + Calibrate button

The dialog is pure UI: it validates locally, jogs live, debounce-pushes
cfg, and only persists on Save.  The **parent owns the persisted draft**;
the dialog talks to the firmware directly via `SetPortProfile`.

**Persistence + apply path:**
1. Studio's `portProfiles` overlay (keyed by `PortRef`) is populated on connect from `/hubfx.yaml`'s `ports[]` block (`LoadHubConfig`) and surfaced through `$deviceModel.ports[i].profile`.
2. Edit fires `SetPortProfile(guid, kind, idx, profile)`:
   - updates the overlay,
   - live-pushes via `Roles.ServoSetProfile` (best-effort hub-local; the role applies it without losing the in-flight target),
   - emits `devicemodel:changed` so the editor re-renders with the new value reflected.
3. `SaveHubConfig` writes the profile back into the `ports[]` entry as a `profile: { … }` block; firmware reload re-parses + re-attaches with the new payload.
4. On hub boot, the firmware reads `/hubfx.yaml` → `PortMapping.profile` → serialises into the role-attach cfg payload → `RoleServicePolicy::attachServoActuator` applies it during attach.

Wire commands wrapped by Wails:

| Command | Purpose |
|---|---|
| `client.Roles.ServoGetProfile(portIdx)` | Read (live-tune fetch) |
| `client.Roles.ServoSetProfile(portIdx, profile)` | Push (atomic; in-flight target preserved) |

Reference rules: **Rule 42** (storage) + **Rule 44** (editing surface).
References:
[ServoWidget.svelte](../app/go/studio/frontend/src/lib/components/ServoWidget.svelte),
[ServoCalibrationDialog.svelte](../app/go/studio/frontend/src/lib/dialogs/ServoCalibrationDialog.svelte),
[GunFxPanel.svelte Turret section](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte),
[app_devicemodel.go `SetPortProfile`](../app/go/studio/app_devicemodel.go),
[app_hubconfig.go `yamlPortBinding.Profile`](../app/go/studio/app_hubconfig.go),
[hubfx_config.h `PortMapping.profile` + `parseServoProfile`](../controllers/hubfx/esp32s3/src/config/hubfx_config.h),
[apply_hubfx_config.h `attachPortRolesForGuid`](../controllers/hubfx/esp32s3/src/config/apply_hubfx_config.h),
[role_service.cpp `attachServoActuator`](../controllers/lib/sfx_board/server/role_service.cpp).

---

## 12. Element scaling editor

**Rule 42**: for sub-rail elements (5 V heater on 8 V rail, 6 V fan on
12 V battery), voltage scaling lives on `HeaterRole` / `DcMotorRole`,
NOT inside the effect that drives them. The widget lives on the **IO
tab's port-role row** for any pwm port whose role is `Heater` or
`DcMotor`.

Live-tune wire commands:

| Command | Purpose |
|---|---|
| `client.Roles.HeaterGetElement(portIdx)` | Read heater element + drive_pct + hyst |
| `client.Roles.HeaterSetElement(portIdx, e)` | Push heater config |
| `client.Roles.MotorGetElement(portIdx)` | Read DC motor element |
| `client.Roles.MotorSetElement(portIdx, e)` | Push motor element |

GET responses include the read-only `portRailMv` so Studio shows the
operator both element and rail in one round-trip.

Reference rule: **Rule 42**. References:
[element_scaling.h](../controllers/lib/sfx_board/element/element_scaling.h),
[heater_role.h](../controllers/lib/sfx_board/roles/heater_role.h),
[dc_motor_role.h](../controllers/lib/sfx_board/roles/dc_motor_role.h),
[client/roles.go](../app/go/client/roles.go).

---

## 13. Verbose-status event subscriber

For "live mirror" panels (gun manual control, gear status, …) the
firmware emits ~10 Hz async status broadcasts; Studio subscribes via a
Wails event and renders the latest snapshot.

Wiring pattern (in `lib/<effect>.ts`):

```ts
let verboseRegistered = false
let cancelVerbose: (() => void) | null = null

export function installVerboseListener(): void {
    if (verboseRegistered) return
    verboseRegistered = true
    cancelVerbose = EventsOn('gun:verbose', (ev: GunVerboseStatusT) => {
        verboseStore.update(m => ({ ...m, [ev.id]: ev }))
    })
}
export function uninstallVerboseListener(): void {
    if (cancelVerbose) cancelVerbose()
    EventsOff('gun:verbose')
    verboseRegistered = false
    cancelVerbose = null
}
```

Panel calls `installVerboseListener()` on mount and `uninstallVerboseListener()`
on unmount; the subscription survives tab switches (one listener for
the whole Studio session).

Reference: [gunfx.ts](../app/go/studio/frontend/src/lib/gunfx.ts).
Future rule pointer: **Rule 41 (reserved, GunFX Phase 4d)** — manual
override / puppet-mode panel.

---

## 14. Add/remove list

For operator-authored arrays in the draft (ROF items, gun units,
landing-light entries, …):

```svelte
<button class="small" on:click={() => addItem()} disabled={busy || cfg.items.length >= MAX}>
    + Add item
</button>

{#each cfg.items as item, i (i)}
    <div class="item-row">
        <span class="item-idx">#{i + 1}</span>
        <input class="field-input" value={item.name}
               on:input={(e) => updateItem(i, 'name', inputValue(e))} />
        … other per-item fields …
        <button class="small danger" on:click={() => removeItem(i)} disabled={busy}>×</button>
    </div>
{/each}
```

Mutator helpers in the lib module:

```ts
export function addItem(): void {
    draft.update(c => ({ ...c, items: [...c.items, defaultItem()] }))
}
export function removeItem(i: number): void {
    draft.update(c => ({ ...c, items: c.items.filter((_, j) => j !== i) }))
}
export function updateItem(i: number, key: keyof Item, val: any): void {
    draft.update(c => ({
        ...c,
        items: c.items.map((it, j) => j === i ? { ...it, [key]: val } : it),
    }))
}
```

CSS — flat row with index chip on the left, destructive button on the
right (Rule 34 row-button order). Reference:
[GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte)
ROF-item rows.

---

## 15. Sound row + speaker-routing button

Every panel that pairs a WAV file path with stereo routing — engine
starting/looping/stopping, GunFx per-ROF sound, future LightFx mode
sounds — uses the shared component
[`SoundRow.svelte`](../app/go/studio/frontend/src/lib/components/SoundRow.svelte).
Owns the label + wide input + browse `…` + Clear (or hidden spacer)
+ the speaker-routing button. **Required by Rule 47** — inlining the
markup again is a violation.

```svelte
<script lang="ts">
    import SoundRow from '../components/SoundRow.svelte'
</script>

<SoundRow
    label={f === 'running' ? 'looping' : f}
    placeholder={optional ? '/sounds/…  (optional)' : '/sounds/…  (required)'}
    value={cfg.sounds[f]}
    outputMask={maskFromOutput(cfg.output)}
    busy={busy}
    required={!optional}
    error={err}
    onPathChange={(v) => { cfg.sounds[f] = v; mark(); scheduleValidate() }}
    onMaskChange={setOutputMask}
    onBrowse={() => browsePath(f)}
    onClear={() => clearSound(f)} />
```

**With a leading slot** (GunFx uses this to align the row with the
`#N` badge on the row above):

```svelte
<SoundRow … >
    <span slot="lead" class="rof-idx-pill placeholder" aria-hidden="true"></span>
</SoundRow>
```

**Speaker-routing helpers** live in
[`speaker_routing.ts`](../app/go/studio/frontend/src/lib/components/speaker_routing.ts) —
single source of truth so the wire-format mask + the user-facing
label + the colour cue never drift:

```ts
import {
    MASK_LEFT, MASK_RIGHT, MASK_STEREO,      // 0x01 / 0x02 / 0x03 — match firmware AudioChannel
    cycleOutputMask,                         // Stereo → Left → Right → Stereo
    speakerLabel,                            // 'left' / 'right' / 'stereo' (tooltips, aria)
    routeShortLabel,                         // 'L'    / 'R'    / 'L+R'    (on-button text)
    speakerIcon,                             // inline SVG glyph (mute-wave variant)
    speakerStateClass,                       // 'route-left' / 'route-right' / 'route-stereo'
} from '../components/speaker_routing'
```

**UX invariants** (enforced by the component, do not override):

- The speaker button STAYS enabled when the sound path is empty —
  routing is a property of the slot, not the file. The operator can
  pre-pick where the next browsed file will play.
- Click cycles Stereo → Left → Right → Stereo. Closed-state shows
  icon + short label (`L+R` / `L` / `R`).
- Button order: browse `…` LEFT, Clear MIDDLE, speaker `L+R` RIGHTMOST.
  Required rows reserve the Clear slot with a hidden `.btn-spacer` so
  the speaker column always aligns.
- Colour cue per state (matches the band-cluster legend palette):
  stereo = `--accent`, left = `--warning`, right = `--success`.

Reference rule: **Rule 47**. Call sites:
[EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte)
(three rows bind to one engine-level `cfg.output` via
`maskFromOutput`/`outputFromMask` — engine plays one sound at a time),
[GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte)
(per-ROF `outputMask`).

---

## 16. Multi-band channel cluster

Rule 36 extension for **N-of-M discrete selectors** — gun rate-of-fire,
future LightFx program selector, alert-bank picker. Shared component
[`ChannelBandCluster.svelte`](../app/go/studio/frontend/src/lib/components/ChannelBandCluster.svelte).

```svelte
<script lang="ts">
    import ChannelBandCluster from '../components/ChannelBandCluster.svelte'
    type BandItem = { loUs:number; hiUs:number; name:string;
                      meta?:string; color:string; armed:boolean }
</script>

<ChannelBandCluster
    channelLabel="Selector channel"
    emptyOption="— none (use item #1 always) —"
    options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
    inputId={gun.rof.input}
    bands={buildBands(gun.rof.items, liveUs, liveValid)}
    overlapIndices={detectBandOverlaps(gun.rof.items)}
    liveUs={liveUs}
    liveValid={liveValid}
    busy={busy}
    onInputChange={(v) => setRofField(gun.id, 'input', v)} />
```

**What the widget owns** (don't reimplement):

- Selector dropdown (named-channel options, `— none —` first).
- Live bar with per-item coloured zone, live µs marker, NO-SIGNAL
  stripe, overlap-error diagonal hatch (when `overlapIndices` is
  non-empty), legend showing live µs + item count + overlap count.
- **Source-order paint with `z-index: bands.length - idx`** so item
  #1 lands ON TOP of later siblings. This is the fix for the
  "first item invisible behind a `[0,0]` catch-all" bug — Svelte 4
  reverse-paint with keyed-each kept stale styles on the topmost
  block. Source order + inline z-index sidesteps reconciliation.
- **Unbounded bands** (`loUs == 0` or `hiUs == 0`) get a diagonal-
  stripe overlay + `∞` tag so a catch-all item is visible even when
  a narrower sibling covers part of the bar.

**Companion validation the panel MUST surface** (Rule 38 + Rule 35):

- Per-item errors NEAR each item row — overlap with sibling,
  inverted `hi <= lo`, value-out-of-range, unbounded-with-explicit-
  siblings warning.
- Section header chip aggregating the error count.
- Section header turns RED on any item-level error (gates global
  Apply via the source's `hasErrors`).

**Smart auto-populate on add** (Rule 38, hard requirement): the panel's
`addItem` mutator MUST seed a non-overlapping band. Algorithm:

1. Find the largest gap in `[1000, 2000]` between sorted existing bands.
2. If gap ≥ 100 µs, return that gap (rounded to 10 µs).
3. Otherwise slice the widest existing band in half (caller trims
   the source to the midpoint).
4. Pathological fallback: `[1800, 2000]`.

Defaulting to `[0, 0]` makes every new item span the whole bar →
stacks invisibly under siblings. Reference implementation:
[`suggestNextRofBand` in gunfx.ts](../app/go/studio/frontend/src/lib/gunfx.ts).

Reference rule: **Rule 38**. Call site:
[GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte)
ROF section. New consumers (LightFx programs, alert banks) plug in
straight away — different `bands[]` payload, same chrome.

---

## 17. Operational action cluster

Connected segment group for primary effect actions (Fire/Stop with
optional picker, Start/Stop). Replaces a row of loose buttons + a
separate select with a SINGLE visual unit so emergency stops sit next
to the action that produced them — no cross-row hunting for the
cutoff. Defined as **global** CSS classes in
[`style.css`](../app/go/studio/frontend/src/style.css), so any panel
can adopt the look without re-rolling.

```svelte
<!-- GunFx: Fire + ROF picker + Stop as one control -->
<div class="op-cluster">
    <button class="oc-btn oc-primary"
            on:click={() => gunStartFiringWithRof(gun.id, 0, pickRofForGun(gun))}
            disabled={busy || $gunfxDirty || gun.rof.items.length === 0}
            title="Start auto-fire at the picked ROF (or RC-armed)">▶ Fire</button>
    <select class="oc-picker"
            value={pickRofForGun(gun)}
            on:change={(e) => setRofPick(gun.id, Number(selValue(e)))}
            disabled={busy || gun.rof.items.length === 0}
            title="…">
        <option value={ROF_ARMED} title="…">RC</option>
        {#each gun.rof.items as item, i}
            <option value={i} title="{item.name} · {item.rpm} rpm">#{i + 1}</option>
        {/each}
    </select>
    <button class="oc-btn oc-danger"
            on:click={() => gunStopFiring(gun.id)}
            disabled={busy}
            title="Stop — always enabled (emergency cutoff)">■ Stop</button>
</div>

<!-- EnginePanel: Start + Stop, no picker -->
<div class="op-cluster">
    <button class="oc-btn oc-primary" on:click={onStart}
            disabled={busy || $engineDirty || soundsHaveErrors}>▶ Start</button>
    <button class="oc-btn oc-danger" on:click={onStop}
            disabled={busy}>■ Stop</button>
</div>
```

**Required classes** (all global, all in `style.css`):

- `.op-cluster`        — flex wrapper, 28 px height, shared 1 px outer border, 4 px radius, no internal gap
- `.oc-btn`            — segment-shaped button (no border, no radius, 11 px font)
- `.oc-btn.oc-primary` — green text (`--success`); the start/fire action
- `.oc-btn.oc-danger`  — red text (`--error`); the stop action; red-tinted hover
- `.oc-picker`         — narrow inline `<select>`, mono font, custom CSS chevron via two linear-gradients (because `appearance: none` strips the native arrow)

**UX invariants** (mandatory):

- The **danger / stop** segment is ALWAYS the rightmost element AND is ALWAYS enabled (no `dirty`/`errors` gate). It's the safety switch.
- The **primary** segment carries the Rule 35 gate (`busy || $dirty || hasErrors`) — running on a stale draft is the bug-disguised-as-a-bug.
- The **picker** (when present) sits BETWEEN primary and danger — it's a modifier on the primary action, visually flanked by the action and the cutoff.
- Closed-state picker text stays short (≤ 5 chars). Long names live in `<option title="…">` tooltips so the cluster doesn't expand when the operator picks a verbose option.

Reference rule: **Rule 48**. Call sites:
[GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte)
fire cluster,
[EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte)
status-row.

---

## 18. Modular config sources + global Apply

Every persistent config source (`/hubfx.yaml`, `/enginefx.yaml`,
`/gunfx.yaml`, future `/lightfx.yaml`, `/gearcontrol.yaml`) plugs into
the global [`ConfigToolbar`](../app/go/studio/frontend/src/lib/layout/ConfigToolbar.svelte)
via a **`DirtySource`** descriptor:

```ts
import type { DirtySource } from './dirty-registry'

export const xxxConfigSource: DirtySource = {
    id:        'xxx',                  // stable, used in apply-order hints
    label:     'XxxFx',                // human label for the dirty pill
    isDirty:   xxxDirty,               // Readable<boolean>
    hasErrors: xxxErrors,              // Readable<boolean> (derived from draft validation)
    apply:     applyXxxConfig,         // () => Promise<void> — must throw on failure
    refresh:   loadXxxConfig,          // () => Promise<void>
}
```

Register ONCE in `App.svelte` `onMount`, in dependency order:

```ts
registerDirtySource(hubConfigSource)    // FIRST — effect translators resolve named inputs against /hubfx.yaml
registerDirtySource(engineConfigSource)
registerDirtySource(gunfxConfigSource)
```

**Rule 46 split between fingerprint-based and signal-based**:

- **Per-effect drafts** (engine, gunfx) use the **draft-vs-config
  fingerprint** pattern (§10) — the draft store is the operator's
  edit surface, the config store is the device truth, dirty is
  derived from JSON equality.
- **Hub IO state** (`/hubfx.yaml` — port aliases, role attachments,
  servo profiles, input protocol + channels) uses the **signal-based**
  pattern: `_hubDirty` is a plain writable that mutators raise via
  `markHubDirty()`. The fingerprint approach failed three times here
  (closed-over baselines racing with async Wails events; missed
  `get` import making rebaseline a silent no-op) so we use the
  cheaper, race-free model.

Mutator contract for the hub side:

```ts
export async function attachRole(p: PortRef, roleKind: number): Promise<void> {
    const snap = await AttachRole(p.guid, p.kind, p.index, roleKind)
    deviceModel.set(normalize(snap))
    markHubDirty()    // role attachment persists into /hubfx.yaml ports[]
}
```

If you add a new IO-tab mutator that writes `/hubfx.yaml`, call
`markHubDirty()` — explicit contract. Mutators that DON'T persist
(claim/unclaim — studio overlay only) leave the flag alone.

`hydrateFromHubYaml()` and `applyHubConfig()` both call
`clearHubDirty()` after their respective op so the flag returns to
false after a clean load/save.

Reference rule: **Rule 46**. References:
[dirty-registry.ts](../app/go/studio/frontend/src/lib/dirty-registry.ts),
[ConfigToolbar.svelte](../app/go/studio/frontend/src/lib/layout/ConfigToolbar.svelte),
[devicemodel.ts (markHubDirty)](../app/go/studio/frontend/src/lib/devicemodel.ts),
[gunfx.ts (gunfxConfigSource)](../app/go/studio/frontend/src/lib/gunfx.ts),
[effects.ts (engineConfigSource)](../app/go/studio/frontend/src/lib/effects.ts).

---

## 19. Servo I/O status widget

A **read-only live monitor** for any servo driven from an RC channel —
pairs the SIGNAL INPUT (RC µs) with the SERVO OUTPUT in one widget, like
a calibration view that never stops updating.  Distinct from §11
(ServoWidget = *editing* the profile): this one only *shows* live state.

**Component:** [ServoIoWidget.svelte](../app/go/studio/frontend/src/lib/components/ServoIoWidget.svelte)
— two stacked bars:
- **Signal input** — RC value over the 1000–2000 µs range + neutral tick
  + striped **NO SIGNAL** state (Rule 34).
- **Servo output** — a track over a fixed 500–2500 µs envelope showing
  the configured `[min,max]` travel as a band + centre tick, with **two
  vertical lines**:
  - **ACTUAL position → solid RED line** (`var(--error)`) — the live
    `position()` = motion-profile output INCLUDING recoil kicks.
  - **TARGET → dashed YELLOW line** (`var(--warning)`) — where the
    profile is slewing to.
  - a colour-key legend (`actual` / `target`) renders under the track so
    the two are unambiguous even when they overlap at rest (servo idle,
    pos == target).

```svelte
{@const sv = $servoStatus[servoStatusKey(axis.servoPort.guid, axis.servoPort.idx)]}
<ServoIoWidget
    hasInput={!!axis.input}  inputUs={liveAx?.us ?? null}  inputValid={liveAx?.valid ?? false}
    neutralUs={axis.neutralUs}
    hasServo={!!(axis.servoPort && axis.servoPort.kind)}
    minUs={prof.minUs} maxUs={prof.maxUs} centerUs={prof.centerUs} reversed={prof.reversed}
    servo={sv ?? null} />
```

Props: `hasInput`/`inputUs`/`inputValid`/`neutralUs` (signal bar);
`hasServo`/`minUs`/`maxUs`/`centerUs`/`reversed` (output envelope);
`servo` (`{posUs,targetUs,velUsPerS}` | null).  When `servo` is null it
falls back to a dim **cmd-only** line at the commanded position (no
telemetry yet).

**Live telemetry plumbing (generic / port-keyed, Rule 42 + 53):**
1. The firmware emits a batched `SERVO_MOTION_UPDATE` (0x4C) of EVERY
   active servo at the host-requested rate (`SERVO_SET_BROADCAST_HZ`
   0x47), gated on `hostVerboseActive` and naturally silent during
   uploads (the upload-exclusive loop skips `RoleService::update`).
2. Studio's [`servo_status.ts`](../app/go/studio/frontend/src/lib/servo_status.ts)
   store is keyed `${guid}|servo|${idx}` — the SAME composite key a
   consumer derives from its servo `PortRef`, so any panel reads its
   servo by ref.
3. **Subscribe-on-view:** the panel calls `setServoLiveView(true)` in
   `onMount` (+ `false` on destroy); the Go side re-applies on reconnect
   ([app_servo.go](../app/go/studio/app_servo.go)).
4. The Go event hop is `OnServoMotion` → emit `servo:motion` (hub-local
   GUID "" remapped to the hub GUID so keys match).

> ⚠️ **Gotcha (the bug that motivated this widget, 2026-05-31):** the
> client's `Events.add()` is a manual type switch over the observer slice
> type — a new `OnXxx` event silently no-ops unless you add a
> `case *[]func(XxxEvent):`.  `OnServoMotion` was missing it, so the
> stream decoded fine but reached zero observers and the widget showed
> only the cmd-only line.  When adding any live event, add the `case`.
> See [protocol/connection.go](../app/go/protocol/connection.go) (Rule 53/56)
> and `client/events.go`.

Reference rule: **Rule 42** (telemetry is role-layer, decoupled from the
effect).  References:
[ServoIoWidget.svelte](../app/go/studio/frontend/src/lib/components/ServoIoWidget.svelte),
[servo_status.ts](../app/go/studio/frontend/src/lib/servo_status.ts),
[app_servo.go](../app/go/studio/app_servo.go),
[GunFxPanel.svelte Turret section](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte),
[role_service.cpp `emitServoBroadcast`](../controllers/lib/sfx_board/server/role_service.cpp).

---

## When the catalog is wrong

If you're building a widget and this catalog says one thing but the
formal rule says another, the **formal rule wins**. Update the catalog
in the same commit as the panel work that triggered the change. The
catalog is a working handbook; rules are the spec.

If you build a NEW pattern that's reusable, you owe future-you a
catalog entry. Add it before the panel ships, even if the entry is
just "see X.svelte for the implementation, formalise next time."

---

## Reference panels (canonical implementations)

- **EnginePanel** — [src/lib/tabs/EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte) — most complete; channel cluster, validation, sound rows, button alignment, dirty-draft pattern.
- **GunFxPanel** — [src/lib/tabs/GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) — per-unit cards, add/remove lists, multi-input named-channel pickers, cross-board output port pickers.
- **PortRoleTab** — [src/lib/tabs/PortRoleTab.svelte](../app/go/studio/frontend/src/lib/tabs/PortRoleTab.svelte) — port-role assignment (no validation cluster; pure inventory table).
- **InputPanel** — [src/lib/tabs/InputPanel.svelte](../app/go/studio/frontend/src/lib/tabs/InputPanel.svelte) — where channel **names** are authored (the source of truth Rule 43 references).
