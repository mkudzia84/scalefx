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
2. [Status row](#2-status-row) — state pill + dirty flag + Apply + operate buttons (Rule 35)
3. [Field row](#3-field-row) — label + input + unit + action buttons (Rule 34)
4. [Button cluster](#4-button-cluster) — browse left, clear right, alignment slots (Rule 34)
5. [Validation surfacing](#5-validation-surfacing) — error/warn at field + section + Apply (Rule 35)
6. [Channel-setup cluster](#6-channel-setup-cluster) — channel + threshold + hyst + live bar overlays (Rule 36)
7. [Named-channel picker](#7-named-channel-picker) — pick from `/hubfx.yaml inputs[]` by name (Rule 43)
8. [Cross-board port picker](#8-cross-board-port-picker) — output ports with rail-voltage labels (Rule 34, Rule 37)
9. [File picker](#9-file-picker) — `pickFile({ targets })` parametrised by backend (Rule 34)
10. [Dirty-flag indicator + draft store pattern](#10-dirty-flag-indicator) — `engineConfig` / `engineDraft` / `engineDirty`
11. [Servo motion profile editor](#11-servo-motion-profile-editor) — lives on the port-role row, not the effect (Rule 42)
12. [Element scaling editor](#12-element-scaling-editor) — heater / DC-motor element voltage + scaling, on the port-role row (Rule 42)
13. [Verbose-status event subscriber](#13-verbose-status-event-subscriber) — live ~10 Hz mirror (Phase 4 staple for manual-mode panels)
14. [Add/remove list (ROF items, gun units, …)](#14-addremove-list) — operator-authored arrays in a draft store

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

## 6. Channel-setup cluster

The canonical 4-row cluster for any RC-channel-gated action (engine
throttle, gun trigger, future LightFx mode-switch …):

```
[ Channel selector (named, via Rule 43) ]
[ Threshold + hysteresis (verb-led, units inline) ]
[ Live bar with threshold marker + hysteresis band ]
[ Color-coded legend ]
```

Markup + the `.chan-cluster` / `.threshold-mark` / `.hyst-band` /
`.bar-legend` CSS lives in
[EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte).
**Copy verbatim** — colour semantics (green = live, red = threshold,
amber = hysteresis) are part of the rule.

Reference rule: **Rule 36**. Full walkthrough:
[21-STUDIO-ENGINEFX-PANEL.md § 2](21-STUDIO-ENGINEFX-PANEL.md).

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

**For OUTPUT ports** (muzzle flash LED, recoil servo, smoke heater) —
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
    const rail = formatPortRail(p.voltageMv)
    return `${p.boardName ?? 'Hub'} · ${p.hardwareName}${rail ? ` (${rail})` : ''}`
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

## 11. Servo motion profile editor

**Rule 42 storage + Rule 44 editing surface:** the profile DATA lives
in `/hubfx.yaml`'s `ports[]` block (canonical, per-port, because it's
a property of the physical servo — min/max are mechanical end-stops,
speed/accel match the servo's spec sheet).  The EDITOR is embedded
**inline in the feature panel** (GunFx Turret section, future EngineFx
servo binding, …) so operators tune the servo where they set the
feature.  The IO tab's `PortRoleConfig.svelte` does **not** show this
editor — that would create a duplicate authoring surface for the same
data.

**Component:** [ServoProfileEditor.svelte](../app/go/studio/frontend/src/lib/components/ServoProfileEditor.svelte)
— a reusable, sectioned editor (Limits / Direction / Motion Profile)
with auto-fit grid layout (`repeat(auto-fill, minmax(110px, 1fr))`) so
fields breathe at any panel width, plus a plain-English behaviour
summary at the bottom.

```svelte
<ServoProfileEditor
    profile={profileForPort(axis.servoPort)}    <!-- look up from $deviceModel.ports[i].profile -->
    label="{which} motion profile"
    on:change={(e) => schedulePushProfile(axis.servoPort, e.detail)} />
```

```ts
// Frontend helpers (one per panel):
function profileForPort(port: PortRefT): ProfileT {
    // walks $deviceModel.ports for a matching ref, returns the
    // attached profile or a fresh default
}
function schedulePushProfile(port: PortRefT, prof: ProfileT) {
    // ~350 ms debounce → SetPortProfile(guid, kind, idx, prof)
    // which updates Studio's overlay + live-pushes via ServoSetProfile
    // + marks /hubfx.yaml dirty for the next SaveHubConfig
}
```

Props:
- `profile` — `ServoMotionProfileT` ({minUs, maxUs, centerUs, reversed,
  maxSpeedUsPerSec, maxAccelUsPerSec2, maxJerkUsPerSec3}); pass by value,
  not `bind:` (parent updates via the `change` event)
- `disabled` — disables every input + button
- `pushStatus` — surfaces a chip on the footer; omit to hide
- `label` — optional section header; omit to skip

The component is pure UI: it validates locally, fires `change` on
every edit, and exposes a `Defaults` button.  The **parent owns the
debounce + push timer** and the persisted draft.

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
[ServoProfileEditor.svelte](../app/go/studio/frontend/src/lib/components/ServoProfileEditor.svelte),
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
