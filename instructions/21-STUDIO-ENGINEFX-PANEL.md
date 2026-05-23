# 21 — Studio EngineFX Panel: Reference Design for Effect Tabs

> **Read this before building a new Studio effect tab.** EngineFX is the
> canonical implementation of the Studio design language for *operational*
> effect panels — panels that bind firmware config, surface live state, and
> let an operator test the effect on real hardware. The patterns documented
> here apply verbatim to GunFX (trigger gate), LightFX (mode-switch input),
> any future effect that wants to play in the same UX.
>
> File of record: [app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte).
> Cross-references: [Rule 34](../.github/copilot-instructions.md) (design system),
> [Rule 35](../.github/copilot-instructions.md) (validation gates Apply + operational),
> [Rule 36](../.github/copilot-instructions.md) (channel-setup cluster).

---

## 0. What problem is this panel solving?

An effect panel is the bridge between **a YAML file on the device** and **a
human dialing knobs**. The operator needs to:

1. **See** the current firmware config (loaded from `/<effect>fx.yaml` on flash).
2. **Edit** it without committing — they should be able to twiddle, change their mind, and back out.
3. **Apply** it — write the YAML back, reload it, and have the effect run the new config immediately.
4. **Test** it on real hardware — `▶ Start`, `Trigger`, `Preview` — but ONLY against the currently-loaded firmware config (not a dirty draft).
5. **Tune the RC channel** that gates the effect — pick a channel, set a threshold, set hysteresis, and see what their stick movement actually does.

The panel layout follows that exact sequence, top to bottom — every region
of the card answers one of the questions above. Don't shuffle them.

---

## 1. Panel anatomy (top → bottom)

```
┌─ Card ─────────────────────────────────────────────────────────────┐
│ ▣ EngineFX                            [☑ Enabled / ☐ Disabled]     │   ← card-header (Rule 34)
├────────────────────────────────────────────────────────────────────┤
│ STATE [running]  [resolve errors above ▸]  [Apply] ‖ [▶ Start][■]  │   ← status-row (Rule 35)
│                                                                    │
│ ┌─ Channel-setup cluster (Rule 36) ──────────────────────────────┐ │
│ │ DRIVER CHANNEL  [CH3 · engine_toggle              ▾ ]          │ │
│ │ FIRES WHEN CHANNEL ≥ [1500] µs  ±  [25] µs hysteresis          │ │   ← settings ABOVE the bar
│ │ ▓▓▓▓▓▓▓▓████████│░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │ │   ← bar with red line + amber band
│ │ 1532 µs LIVE · 1500 µs THRESHOLD · ±25 µs HYSTERESIS · 1000…  │ │   ← color-coded legend
│ └────────────────────────────────────────────────────────────────┘ │
│                                                                    │
│ TYPE     [Turbine                      ▾ ]                         │
│ OUTPUT   [I²S (TAS5825M)               ▾ ]                         │
│                                                                    │
│ ── SOUNDS ────────────────────────────[missing files chip]──────── │   ← section-head (red on err)
│ starting   [/sounds/start.wav      ] […] [Clear]                   │   ← row pattern (Rule 34)
│ looping *  [/sounds/run.wav        ] […] [        ]                │   ← btn-spacer for alignment
│ stopping   [/sounds/stop.wav       ] […] [Clear]                   │
│   ⚠ file not found on SD: /sounds/run.wav                          │   ← row-err inline
│                                                                    │
│ ── TRANSITIONS ───────────────────────────────────────────────────  │
│ Starting offset [200] ms      Stopping offset [200] ms             │
│ Start fade-in   [400] ms      Stop fade-out   [600] ms             │
└────────────────────────────────────────────────────────────────────┘
```

The card-header and status-row are **always visible** when the effect is
enabled. Disabling the effect collapses the body — the toggle is the only
control that stays exposed (so the operator can re-enable without
configuring anything).

---

## 2. The channel-setup cluster (the "way forward")

This is the most important pattern in the panel. **Every effect that binds
an RC channel and gates an action on a microsecond threshold MUST use this
cluster verbatim** (Rule 36). It exists because earlier designs scattered
the threshold halfway down the form, and operators couldn't dial it
correctly without scrolling to see the live bar.

### 2.1 The four rows, in order

```html
<div class="chan-cluster">
    <!-- 1. Channel selector -->
    <div class="form-row">
        <span class="field-label">Driver channel</span>
        <select class="field-input wide" …>
            <option value="">— manual only —</option>
            <option value="engine_toggle">CH3 · engine_toggle</option>
            …
        </select>
    </div>

    <!-- 2. Trigger settings — verb-led, inline, above the bar -->
    <div class="form-row trigger-row">
        <span class="field-label">Fires when channel ≥</span>
        <input class="field-input narrow" type="number"
               min="800" max="2200" step="10" value={thresholdUs}
               on:change={(e) => { thresholdUs = numValue(e); mark() }} />
        <span class="unit">µs</span>
        <span class="trigger-pm">±</span>
        <input class="field-input narrow" type="number"
               min="0" max="500" step="5" value={hysteresisUs}
               on:change={(e) => { hysteresisUs = numValue(e); mark() }} />
        <span class="unit">µs hysteresis</span>
    </div>

    <!-- 3. Live bar with markers -->
    <div class="bar tall" class:nosignal={!liveUs?.valid}>
        {#if liveUs?.valid}
            <div class="bar-fill" style="width: {usToPct(liveUs.us)}%"></div>
        {/if}
        <div class="hyst-band"
             style="left: {usToPct(thresholdUs - hysteresisUs)}%;
                    width: {Math.max(0.4, usToPct(thresholdUs + hysteresisUs)
                                         - usToPct(thresholdUs - hysteresisUs))}%"
             title="hysteresis band — ±{hysteresisUs}µs"></div>
        <div class="threshold-mark"
             style="left: {usToPct(thresholdUs)}%"
             title="threshold {thresholdUs}µs"></div>
        {#if !liveUs?.valid}
            <span class="bar-nosignal">
                {channel ? 'NO SIGNAL' : 'no channel bound — manual only'}
            </span>
        {/if}
    </div>

    <!-- 4. Legend — color-coded to the markers -->
    <div class="bar-legend">
        <span class="leg-live">{liveUs?.valid ? liveUs.us : '—'} µs <span class="leg-label">live</span></span>
        <span class="leg-sep">·</span>
        <span class="leg-mark">{thresholdUs} µs <span class="leg-label">threshold</span></span>
        <span class="leg-sep">·</span>
        <span class="leg-band">±{hysteresisUs} µs <span class="leg-label">hysteresis</span></span>
        <span class="leg-sep">·</span>
        <span class="leg-range">1000–2000 µs</span>
    </div>
</div>
```

### 2.2 The bar overlays — design rationale

The bar is 18 px tall (`.bar.tall`) instead of the standard 14 px so the
markers are readable. It has **three z-ordered overlays** on top of the
existing `.bar-fill`:

| Overlay | Element | Style | Why |
|---|---|---|---|
| **Hysteresis band** | `.hyst-band` | translucent `var(--warning)` rectangle from `usToPct(thr-hyst)` to `usToPct(thr+hyst)`, dashed amber side-borders, `z-index: 1`, pointer-events none | Shows the deadband around the threshold. Visible even when there's no live signal — operator can dial the trigger before powering the RC link. |
| **Threshold marker** | `.threshold-mark` | 2 px solid `var(--error)` vertical line at `usToPct(thr)`, soft red glow, `z-index: 2`, pointer-events none, `transform: translateX(-1px)` to center the 2 px stroke on the math | The trigger point. Red because it's the "action happens here" boundary. Glow makes it pop on the dim track. |
| **Live fill** | `.bar-fill` (existing) | accent → success gradient, 0 → `usToPct(liveUs)` | The current stick position. Reads as "fuel rising" against the threshold line. |

**Color semantics are part of the rule** — don't recolor the markers
locally. Green = live, red = threshold, amber = hysteresis. The legend
beneath the bar uses the same colors on the values so the operator can
visually trace what's what.

### 2.3 Live update wiring

Every input pushes through `mark()` (`engineDraft.set(cfg)` re-publishing
the draft). This is what makes the red line and amber band **slide in
real time** as the operator types — the whole point of putting the
settings above the bar. Don't debounce the marker positions; debounce the
firmware push, never the visual feedback.

### 2.4 Constants & units

- **Threshold input:** `min="800" max="2200" step="10"` µs. 800–2200 is the
  PPM/SBUS-realistic range with a tiny margin; step 10 matches RC servo
  granularity.
- **Hysteresis input:** `min="0" max="500" step="5"` µs. Hysteresis above
  500 µs is so wide it would prevent legitimate triggers.
- **Bar scale:** `usToPct(us)` from
  [devicemodel.ts:321](../app/go/studio/frontend/src/lib/devicemodel.ts) —
  `1000µs → 0%, 2000µs → 100%`. **All Studio bars use this helper** so
  panels are visually consistent.
- **Verb-led labels:** `"Fires when channel ≥"`, not `"Threshold (µs)"`. Read
  like a sentence so the meaning is obvious. Other verbs: `"Activates above"`,
  `"Holds below"`, `"Triggers above"`.

### 2.5 Reusing the cluster

When wiring a new channel-gated panel, **copy the markup + the
`.chan-cluster` / `.threshold-mark` / `.hyst-band` / `.bar-legend` style
block verbatim**. Don't re-roll your own markers. If you need a different
trigger semantic (e.g. "trigger on edge in either direction"), keep the
visual language (red line + amber band) and only change the verb-led label
and the firmware-side semantics. Future operators move between panels and
expect the same colors to mean the same thing.

---

## 3. The status-row (Rule 35 §7)

```
┌─────────────────────────────────────────────────────────────────────┐
│ STATE [running] [RC engaged]   [unapplied] [Apply] ‖ [▶ Start][■]  │
└─────────────────────────────────────────────────────────────────────┘
   ↑                          ↑                  ↑       ↑
   left cluster               commit-config      |       operate-firmware
                              cluster        divider     cluster
                                            (1×20 var(--border))
```

Three semantic clusters separated by visual hierarchy:

- **Left cluster — state.** `STATE` label + `.state-pill` (color-coded per
  state: green=running, amber=starting/stopping, dim=stopped) + optional
  `.engaged-pill` chip when RC has crossed the threshold.
- **Middle cluster — commit config.** `.dirty-flag` (text — `in sync` /
  `unapplied changes` / `resolve errors above`, colored dim/warning/error)
  + `[Apply]` button. Apply is `disabled={busy || !dirty || hasErrors}`.
- **Right cluster — operate firmware.** `[▶ Start]` + `[■ Stop]`. Start is
  gated on the SAME `busy || dirty || hasErrors` predicate as Apply,
  because pressing Start on a dirty draft would test the *old* firmware
  config and look like a bug. Stop is unconditional — always want a kill
  switch.

The `.ctrl-sep` (1 px wide, 20 px tall, `var(--border)`) between Apply and
Start is **the whole reason this row works** — without it, the four
buttons read as one homogeneous group and the operator can't tell that
Apply is the prerequisite for Start.

**No duplicate Apply at the bottom of the form.** Earlier iterations had
Apply both in this row AND at the bottom; the second one is clutter.
Config-only panels (PortRoleTab) put Apply in the card-header's
`.header-actions` instead.

---

## 4. The sound rows (Rule 34 row-button order)

```
[starting   ] [/sounds/start.wav      ] [...] [Clear  ]
[looping *  ] [/sounds/run.wav        ] [...] [       ]   ← visibility:hidden btn-spacer
[stopping   ] [/sounds/stop.wav       ] [...] [Clear  ]
              ⚠ file not found on SD: /sounds/run.wav    ← row-err inline
```

Anatomy of one row:

```html
<div class="sound-row" class:invalid={!!err}>
    <div class="form-row">
        <span class="field-label" style="width: 72px">{name}{optional ? '' : ' *'}</span>
        <input class="field-input wide" type="text"
               placeholder={optional ? '/sounds/…  (optional)' : '/sounds/…  (required)'}
               value={path} on:input={…} />
        <button class="small btn-slot" on:click={browse}>…</button>
        {#if optional}
            <button class="small btn-slot" on:click={clear} disabled={!path}>Clear</button>
        {:else}
            <span class="btn-slot btn-spacer" aria-hidden="true"></span>
        {/if}
    </div>
    {#if err}<div class="row-err">⚠ {err}</div>{/if}
</div>
```

Three rules baked in here:

1. **Browse `…` is the leftmost action button, Clear is the rightmost.**
   `Clear` over `None` — name the button after what it does, not what
   state it produces.
2. **Reserve the Clear slot on required rows** with a `visibility: hidden`
   `.btn-spacer` of the same width — without it the `…` column shifts
   right on the required `looping` row and the form looks ragged.
3. **Errors surface twice** — `.sound-row.invalid` paints a red border +
   light-red background on the row, and a `.row-err` line under the row
   spells out the reason (`⚠ file not found on SD: …`). The
   `.section-head` above also turns red and grows a chip (`missing files`)
   so a scroller catches the error from the section title alone.

### 4.1 File picker is parametrized (Rule 34)

```ts
async function browsePath(field: 'starting' | 'running' | 'stopping') {
    const p = await pickFile({ targets: 'sd' })   // ← always pass the narrowest target
    if (p != null) { cfg.sounds[field] = p; mark(); scheduleValidate() }
}
```

Sounds live on SD, configs live on flash. Pass the narrowest backend the
field actually addresses; the dialog hides the disallowed tab entirely
(not just disables it) so the operator can't accidentally pick a file
from the wrong filesystem. `targets: 'both'` stays available for the
standalone File Manager dialog.

### 4.2 Optional vs required field validation

```ts
async function validateSounds() {
    const next = { starting: '', running: '', stopping: '' }
    if (!cfg.sounds.running) next.running = 'required — pick a looping sound'
    const probe = [cfg.sounds.starting, cfg.sounds.running, cfg.sounds.stopping]
                    .filter(p => !!p)        // skip empty paths (optional + empty = valid)
    if (probe.length > 0) {
        const exists = await checkFiles(probe)
        for (const k of ['starting','running','stopping'] as const) {
            const p = cfg.sounds[k]
            if (!p) continue                  // optional + empty = valid, no error
            if (!exists[p]) next[k] = `file not found on SD: ${p}`
        }
    }
    soundErrors = next
}
```

- **Required + empty → error** (`'required — pick a looping sound'`).
- **Optional + empty → valid** (no check, no row decoration).
- **Any path that's set → must exist on the device** (batch-probed via the
  `CheckFiles` Go binding — single round-trip for all paths).
- **Clear button on optional rows clears + re-validates immediately** — no
  debounce, no "did it stick?" ambiguity.
- **Same validator runs on read** — a malformed `/enginefx.yaml` pulled
  from the device shows the same red rows the operator would see while
  editing. They never have to guess what the device rejected.

The firmware-side mirror is important: **EngineFX `forceStart()` accepts
no starting/stopping path** (jumps directly to Running on the loop
channel) — the optional field on the UI and the optional handling in the
firmware are the same decision. If you mark a field optional in Studio,
make sure the firmware actually treats it as optional.

---

## 5. State management (the dirty-draft pattern)

Two stores, one validation lattice:

```ts
// effects.ts (simplified)
export const engineConfig = writable<EngineConfigT>(emptyConfig())  // "what firmware loaded"
export const engineDraft  = writable<EngineConfigT>(emptyConfig())  // "what the operator typed"
export const engineDirty  = derived(                                 // "do they differ?"
    [engineConfig, engineDraft],
    ([cfg, draft]) => !deepEqual(cfg, draft)
)
```

- **`engineConfig`** is set ONLY by `loadEngineConfig()` (auto on connect,
  manual on Refresh) and by `applyEngineConfig()` (after a successful
  Apply, we re-fetch). It mirrors the device.
- **`engineDraft`** is what the form binds to (via `engineDraft.subscribe(c
  => cfg = c)`). Every mutation calls `mark()` which calls
  `engineDraft.set(cfg)` to re-publish.
- **`engineDirty`** is a derived store. The status-row dirty-flag reads
  `$engineDirty`; Apply and Start gate on it.

### 5.1 The classic bug we fixed

The `loadEngineConfig()` path used to set `engineConfig` *before*
checking dirtiness:

```ts
// WRONG
async function loadEngineConfig() {
    const n = await GetEngineConfig()
    engineConfig.set(n)                   // ← engineDirty becomes true here
    if (!get(engineDirty)) engineDraft.set(structuredClone(n))   // ← never fires
}
```

The fix is to **snapshot wasDirty before mutating**:

```ts
// RIGHT
async function loadEngineConfig() {
    const n = await GetEngineConfig()
    const wasDirty = get(engineDirty)     // snapshot BEFORE the set
    engineConfig.set(n)
    if (!wasDirty) engineDraft.set(structuredClone(n))   // seeds the draft on fresh load
}
```

If you build a new panel with the same draft/config split, remember:
**snapshot the dirty flag before publishing the new config**, or your
form will never auto-hydrate.

### 5.2 Auto-hydrate on connect (Rule 26)

```ts
// EnginePanel.svelte
onMount(() => {
    loadEngineConfig()
    refreshEngineStatus()
    return unsub
})
```

Plus the model-level [config-loader.ts](../app/go/studio/frontend/src/lib/config/config-loader.ts)
hook: `autoLoadOnConnect(driver, ['hubfx'])` runs `driver.parseYaml` →
`driver.applyState` once per `(port × controllerType)`, so the form is
populated the moment the connect handshake completes. No "click Load"
button.

---

## 6. Cross-tab consistency checklist

When you build the next operational effect tab, walk this list:

- [ ] **Card + enable toggle** in the header (Rule 34).
- [ ] **Status-row** at top with `[State pill] [dirty-flag] [Apply] [divider] [▶ Op]…` (Rule 35 §7).
- [ ] **Channel-setup cluster** (Rule 36) for any RC-gated action — settings ABOVE the bar with red threshold line + amber hysteresis band.
- [ ] **All inputs/selects** carry `.field-input` (+ `.narrow` / `.wide`) — no bespoke control styling (Rule 34).
- [ ] **Row buttons** in canonical order: browse `…` leftmost, `Clear` (never `None`) rightmost, `.btn-spacer` on required rows (Rule 34).
- [ ] **File picker** parametrized — `pickFile({ targets: 'sd' | 'flash' })`, never bare `pickFile()` (Rule 34).
- [ ] **Validation runs continuously** on input (~350 ms debounce), on load, after sub-dialogs — never on Apply-click only (Rule 35).
- [ ] **Errors surface twice** — at the field/row AND at the group header (Rule 35).
- [ ] **Apply and operational buttons gated** on `busy || dirty || hasErrors` (Rule 35 §6).
- [ ] **One Apply per panel** — same row as operational buttons, no duplicate at the bottom (Rule 35 §7).
- [ ] **engineDraft / engineConfig / engineDirty** triad — snapshot `wasDirty` before publishing on load (§5.1).
- [ ] **Required vs optional field semantics** match the firmware — if the form marks a field optional, the firmware must accept its absence (§4.2).
- [ ] **CSS vars for all colours** (`var(--error)`, `var(--warning)`, `var(--success)`, `var(--accent)`) — never hex (Rule 34).
- [ ] **All bars** use the shared `usToPct(us)` helper (§2.4).

When in doubt, open [EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte)
side-by-side with your new panel and diff the patterns. Anything that
looks bespoke probably shouldn't be.

---

## 7. What's intentionally NOT in this panel

A few things the design explicitly omits, with reasons (so a later
maintainer doesn't "fix" them):

- **No "Reload from device" button** — the dirty-flag + Apply already
  surface the divergence, and Refresh on the connect handshake re-pulls
  automatically. A manual reload button just opens the question "does it
  blow away my draft?" which has no good answer.
- **No "Reset to defaults" button** — defaults live in
  [enginefx_config.h](../controllers/hubfx/esp32s3/src/effects/enginefx/enginefx_config.h);
  the operator can write `{}` to the YAML and the firmware will fill in
  defaults on the next reload. A UI "reset" button would require Studio
  to mirror the firmware defaults, which is a second source of truth.
- **No multi-engine support** — EngineFX is a single instance on the
  HubFX, so the panel doesn't accordion multiple engines. If/when a
  multi-engine board appears, the cluster becomes a card-per-engine list,
  not a tabbed sub-view inside the same card.
- **No Stop confirm dialog** — Stop is a kill switch; making it a
  two-click action defeats the safety property. Start has no confirm
  either; Apply's validation gating is the safety net.
- **No graph of the channel history** — the live bar shows the current
  value; a rolling graph would add visual noise without changing what
  the operator does. If we ever add one, it goes BELOW the legend, not
  inside the bar.

---

## 8. Reference paths

- Panel source: [app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte)
- Stores + draft logic: [app/go/studio/frontend/src/lib/effects.ts](../app/go/studio/frontend/src/lib/effects.ts)
- Bar scale helper: [app/go/studio/frontend/src/lib/devicemodel.ts](../app/go/studio/frontend/src/lib/devicemodel.ts) `usToPct(us)`
- File picker: [app/go/studio/frontend/src/lib/filepicker.ts](../app/go/studio/frontend/src/lib/filepicker.ts) — `pickFile({ targets })`
- File dialog: [app/go/studio/frontend/src/lib/dialogs/FileManagerDialog.svelte](../app/go/studio/frontend/src/lib/dialogs/FileManagerDialog.svelte) — honours `fileManagerTargets`
- Backend bindings: [app/go/studio/app_engine.go](../app/go/studio/app_engine.go) — `GetEngineConfig` / `SetEngineConfig` / `EngineStart` / `EngineStop` / `EngineStatus` / `CheckFiles`
- Firmware service: [controllers/hubfx/esp32s3/src/effects/enginefx/enginefx_service.ipp](../controllers/hubfx/esp32s3/src/effects/enginefx/enginefx_service.ipp) — `forceStart()` with optional starting-sound handling
- Design system CSS: [app/go/studio/frontend/src/style.css](../app/go/studio/frontend/src/style.css)
- Rules: [Rule 34](../.github/copilot-instructions.md) (design system), [Rule 35](../.github/copilot-instructions.md) (validation gates), [Rule 36](../.github/copilot-instructions.md) (channel cluster)
- Device-model rules: [20-STUDIO-DEVICE-MODEL.md](20-STUDIO-DEVICE-MODEL.md)
