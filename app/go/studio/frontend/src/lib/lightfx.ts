// lightfx.ts — LightFx domain store (Lighting tab left column).
//
// REFACTOR (2026-05-24): programs are now Studio-side PRESETS.
//   - Bundled FACTORY presets (`assets/presets/lightfx/programs/*.yaml`)
//     ship in the .exe (mirror of `/media/presets/`, kept in sync via
//     `app/go/studio/sync-presets.ps1`).
//   - USER presets live in `%APPDATA%/ScaleFX/lightfx-presets/` and
//     shadow factory entries with the same name.
//   - The Lighting tab's "active list" is now a draft-edited copy of
//     library entries.  Edit modifies the draft in place; Save-As
//     clones it into the library under a new name; Apply syncs every
//     active draft to `/lightfx/programs/<name>.yaml` on the device
//     AND deletes any on-device program file not in the active list
//     (Go-side `SyncLightFxToDevice`).
//
// The cfg shape changes: `programs: string[]` (path list) is gone —
// the active list is `activePrograms: ActiveProgramT[]` carrying both
// the name and the current draft program data.  /lightfx.yaml's
// `programs:` block is rebuilt at sync time from the active names.

import { writable, derived, get, type Readable } from 'svelte/store'
import type { DirtySource } from './dirty-registry'
import type { PortRefT } from './landing'
import {
    GetLightFxConfig,
    GetLightFxProgramStructured,
    ListPresetLibrary, SavePresetAs as WailsSavePresetAs, DeletePreset as WailsDeletePreset,
    ListLightFxOrphans,
    SyncLightFxToDevice,
} from '../../wailsjs/go/main/App'

// ─── DTOs (mirror app_lightfx.go + app_lightfx_presets.go) ───────────

export interface ProgramSelectorRangeT {
    fromUs:  number
    toUs:    number
    program: string         // program name (= active entry's `name` field)
}

export interface ProgramSelectorT {
    enabled:      boolean
    input:        string     // named channel from /hubfx.yaml inputs[]
    hysteresisUs: number
    ranges:       ProgramSelectorRangeT[]
}

// ActiveProgramT — one slot in the operator's active list.  `name`
// becomes /lightfx/programs/<name>.yaml on the device at sync time;
// `program` is the (possibly edited) draft of the channels/events.
//
// (Provenance tracking — which template seeded this slot — was removed
// 2026-05-24 in favour of a flatter model: once you pick a template,
// it's just a program in your list.  To "start over" from a template,
// remove the row and add it again.)
export interface ActiveProgramT {
    name:    string
    program: ProgramT
}

export interface LightFxConfigT {
    schemaVersion:       number
    enabled:             boolean
    masterBrightnessPct: number
    activePrograms:      ActiveProgramT[]
    programSelector:     ProgramSelectorT
}

// PresetLibraryEntryT mirrors Go's PresetLibraryEntry — what
// ListPresetLibrary returns.  Source flag drives the UI badge + the
// "factory presets are immutable, Save-As only" gating.
export type PresetSourceT = 'factory' | 'user'

export interface PresetLibraryEntryT {
    name:     string
    source:   PresetSourceT
    category: string
    note:     string
    program:  ProgramT
}

// ─── Defaults ─────────────────────────────────────────────────────────

const defaultSelector: ProgramSelectorT = {
    enabled: false, input: '', hysteresisUs: 50, ranges: [],
}
const defaultLightFx: LightFxConfigT = {
    schemaVersion: 1, enabled: true, masterBrightnessPct: 100,
    activePrograms: [], programSelector: { ...defaultSelector },
}

// ─── Stores ───────────────────────────────────────────────────────────

export const lightfxConfig = writable<LightFxConfigT>(defaultLightFx)
export const lightfxDraft  = writable<LightFxConfigT>(structuredClone(defaultLightFx))
export const lightfxDirty  = derived(
    [lightfxConfig, lightfxDraft],
    ([$c, $d]) => JSON.stringify($c) !== JSON.stringify($d),
)

/** Merged factory + user preset catalog — refreshed via
 *  `refreshPresetLibrary()` on panel mount and after every
 *  Save-As / Delete.  Drives the "add preset" picker and the
 *  per-active-row library lookup (initial draft on add, "revert" hint
 *  if the slot was seeded from a library entry). */
export const presetLibrary = writable<PresetLibraryEntryT[]>([])

// ─── Helpers ─────────────────────────────────────────────────────────

function nameFromDevicePath(path: string): string {
    return path.replace(/^.*\//, '').replace(/\.yaml$/, '')
}

// ─── Loader / saver ───────────────────────────────────────────────────
//
// On connect, /lightfx.yaml gives us a list of program PATHS (legacy
// schema).  For each path, look the basename up in the (already-loaded)
// library and seed the active draft from the library entry — that's
// the "current state on the device matches a known preset" case.  If
// the library doesn't have the name, fall back to downloading the
// YAML from the device (via GetLightFxProgramStructured) so we don't
// lose data the operator put there out-of-band.

function normaliseLightFx(c: any): Pick<LightFxConfigT,
    'schemaVersion' | 'enabled' | 'masterBrightnessPct' | 'programSelector'> & { _paths: string[] } {
    return {
        schemaVersion:       c?.schemaVersion ?? 1,
        enabled:             c?.enabled ?? true,
        masterBrightnessPct: c?.masterBrightnessPct ?? 100,
        _paths:              Array.isArray(c?.programs) ? c.programs as string[] : [],
        programSelector: {
            enabled:      c?.programSelector?.enabled ?? false,
            input:        c?.programSelector?.input ?? '',
            hysteresisUs: c?.programSelector?.hysteresisUs ?? 50,
            ranges:       Array.isArray(c?.programSelector?.ranges) ? c.programSelector.ranges : [],
        },
    }
}

/** Refresh the merged (factory + user) library catalog from Go. */
export async function refreshPresetLibrary(): Promise<void> {
    const entries = await ListPresetLibrary() as PresetLibraryEntryT[]
    presetLibrary.set(entries ?? [])
}

export async function loadLightFxConfig(): Promise<void> {
    // Library first so the active-list seeding has something to read from.
    await refreshPresetLibrary().catch(() => {})
    const lib = get(presetLibrary)
    const byName = new Map(lib.map(e => [e.name, e] as const))

    const wasDirty = get(lightfxDirty)
    const raw = normaliseLightFx(await GetLightFxConfig() as any)

    const activePrograms: ActiveProgramT[] = []
    for (const p of raw._paths) {
        const name = nameFromDevicePath(p)
        const tmpl = byName.get(name)
        if (tmpl) {
            // Template by this name exists — seed the draft from it.
            // If the operator edited the on-device YAML out of band
            // (via file manager) those changes will be OVERWRITTEN by
            // the template on the next Apply.  Click Edit then Apply
            // to push the current template state if that's intentional;
            // otherwise re-download via the File Manager.
            activePrograms.push({
                name,
                program: structuredClone(tmpl.program),
            })
        } else {
            // Unknown name — fetch from device so the active row has
            // SOMETHING to show.  If the device doesn't have it either
            // (deleted out of band), we get a default-empty program.
            let prog: ProgramT = defaultProgram()
            try {
                prog = await GetLightFxProgramStructured(p) as ProgramT
            } catch { /* default-empty is fine */ }
            activePrograms.push({ name, program: prog })
        }
    }

    const cfg: LightFxConfigT = {
        schemaVersion:       raw.schemaVersion,
        enabled:             raw.enabled,
        masterBrightnessPct: raw.masterBrightnessPct,
        programSelector:     raw.programSelector,
        activePrograms,
    }
    lightfxConfig.set(cfg)
    if (!wasDirty) lightfxDraft.set(structuredClone(cfg))
}

export async function saveLightFxConfig(): Promise<void> {
    const cfg = get(lightfxDraft)
    const activeNames = cfg.activePrograms.map(a => a.name)

    // Orphan-delete confirm — surface destructive sync consequences
    // before they happen so a stray file uploaded via the File Manager
    // dialog doesn't vanish silently on Apply.  Empty list → no confirm.
    let orphans: string[] = []
    try { orphans = await ListLightFxOrphans(activeNames) as string[] } catch { /* best-effort */ }
    if (orphans.length > 0) {
        const list = orphans.length <= 5
            ? orphans.join(', ')
            : `${orphans.slice(0, 5).join(', ')} (+ ${orphans.length - 5} more)`
        const ok = confirm(
            `Apply will delete ${orphans.length} orphan program file${orphans.length === 1 ? '' : 's'} ` +
            `from /lightfx/programs/ on the device:\n\n  ${list}\n\n` +
            `These aren't in your active list.  Continue?`,
        )
        if (!ok) throw new Error('Apply cancelled — orphan deletion declined')
    }

    const goCfg = {
        schemaVersion:       cfg.schemaVersion,
        enabled:             cfg.enabled,
        masterBrightnessPct: cfg.masterBrightnessPct,
        // Programs[] gets rebuilt server-side from `active[].name`.
        programs:            [],
        programSelector:     cfg.programSelector,
    }
    const active = cfg.activePrograms.map(a => ({ name: a.name, program: a.program }))
    await SyncLightFxToDevice(goCfg as any, active as any)
    lightfxConfig.set(structuredClone(cfg))
}

// ─── Library mutators (Save-As / Delete) ─────────────────────────────

/** Clone the active slot's current draft into the USER templates
 *  directory under `newName`.  Also renames the active slot to the new
 *  name so subsequent Apply pushes /lightfx/programs/<newName>.yaml.
 *  Refreshes the templates catalog on success and returns the new
 *  entry.  Rejects (via the Go side) if `newName` collides with a
 *  factory template — operators must pick a fresh name. */
export async function savePresetAs(activeIdx: number, newName: string): Promise<PresetLibraryEntryT> {
    const cfg = get(lightfxDraft)
    const slot = cfg.activePrograms[activeIdx]
    if (!slot) throw new Error('No active slot at that index')
    const entry = await WailsSavePresetAs(newName, slot.program as any) as PresetLibraryEntryT
    await refreshPresetLibrary()
    // Adopt the new name so the active slot now matches the saved
    // template (and uploads to /lightfx/programs/<newName>.yaml on
    // the next Apply).  Also flow the rename into selector ranges.
    renameActive(activeIdx, newName)
    return entry
}

/** Delete a USER library preset.  Factory presets reject server-side
 *  (Save-As to a new name to override instead).  After deleting, the
 *  same name from the factory layer (if any) becomes visible again. */
export async function deletePreset(name: string): Promise<void> {
    await WailsDeletePreset(name)
    await refreshPresetLibrary()
}

// ─── Active-list mutators (draft only) ───────────────────────────────

/** Add a template to the active list — clones the template program
 *  into a new active slot keyed by the template name.  Idempotent on
 *  name: adding the same template twice does nothing.  To get two
 *  copies, Save-As the first one to a fresh name then add again. */
export function addPresetToActive(templateName: string): void {
    const tmpl = get(presetLibrary).find(e => e.name === templateName)
    if (!tmpl) return
    lightfxDraft.update(c => {
        if (c.activePrograms.some(a => a.name === templateName)) return c
        return {
            ...c,
            activePrograms: [...c.activePrograms, {
                name: templateName,
                program: structuredClone(tmpl.program),
            }],
        }
    })
}

/** Add a blank slot for "+ Blank" — the operator picks ports + builds
 *  events from scratch.  Uses a UNIQUE temporary name (`untitled-N`)
 *  so the operator can Save-As to a permanent name later. */
export function addBlankActive(): void {
    lightfxDraft.update(c => {
        let n = 1
        while (c.activePrograms.some(a => a.name === `untitled-${n}`)) n++
        return {
            ...c,
            activePrograms: [...c.activePrograms, {
                name: `untitled-${n}`,
                program: defaultProgram(),
            }],
        }
    })
}

/** Remove an active slot at `idx`.  Also drops any selector range
 *  referencing the removed name so /lightfx.yaml stays consistent. */
export function removeActive(idx: number): void {
    lightfxDraft.update(c => {
        const dropped = c.activePrograms[idx]?.name
        return {
            ...c,
            activePrograms: c.activePrograms.filter((_, i) => i !== idx),
            programSelector: {
                ...c.programSelector,
                ranges: c.programSelector.ranges.filter(r => r.program !== dropped),
            },
        }
    })
}

/** Replace the program data of an active slot — the in-panel editor
 *  calls this every time the operator tweaks channels / events. */
export function setActiveProgram(idx: number, program: ProgramT): void {
    lightfxDraft.update(c => {
        const a = [...c.activePrograms]
        if (!a[idx]) return c
        a[idx] = { ...a[idx], program }
        return { ...c, activePrograms: a }
    })
}

/** Rename an active slot in place — the new name flows to
 *  /lightfx/programs/<new>.yaml on Apply.  No library write. */
export function renameActive(idx: number, newName: string): void {
    lightfxDraft.update(c => {
        const a = [...c.activePrograms]
        if (!a[idx]) return c
        const oldName = a[idx].name
        a[idx] = { ...a[idx], name: newName }
        // Update selector range references too.
        return {
            ...c,
            activePrograms: a,
            programSelector: {
                ...c.programSelector,
                ranges: c.programSelector.ranges.map(r =>
                    r.program === oldName ? { ...r, program: newName } : r),
            },
        }
    })
}

// (revertActiveToLibrary removed 2026-05-24 — provenance tracking is
// gone; to "start over" from a template, remove the active row and
// re-add it via "+ Template …".)

export function setMasterBrightness(pct: number): void {
    const clamped = Math.max(0, Math.min(100, pct | 0))
    lightfxDraft.update(c => ({ ...c, masterBrightnessPct: clamped }))
}
export function setEnabled(on: boolean): void {
    lightfxDraft.update(c => ({ ...c, enabled: on }))
}

// Selector mutators — used by the ChannelBandCluster glue layer.

export function setSelectorInput(name: string): void {
    lightfxDraft.update(c => ({
        ...c,
        programSelector: { ...c.programSelector, input: name, enabled: name !== '' },
    }))
}
export function setSelectorHysteresis(us: number): void {
    lightfxDraft.update(c => ({
        ...c,
        programSelector: { ...c.programSelector, hysteresisUs: us },
    }))
}
export function setSelectorRange(idx: number, patch: Partial<ProgramSelectorRangeT>): void {
    lightfxDraft.update(c => ({
        ...c,
        programSelector: {
            ...c.programSelector,
            ranges: c.programSelector.ranges.map((r, i) => i === idx ? { ...r, ...patch } : r),
        },
    }))
}
export function addSelectorRange(range: ProgramSelectorRangeT): void {
    lightfxDraft.update(c => ({
        ...c,
        programSelector: {
            ...c.programSelector,
            ranges: [...c.programSelector.ranges, range],
        },
    }))
}
export function removeSelectorRange(idx: number): void {
    lightfxDraft.update(c => ({
        ...c,
        programSelector: {
            ...c.programSelector,
            ranges: c.programSelector.ranges.filter((_, i) => i !== idx),
        },
    }))
}

// ─── Validation ───────────────────────────────────────────────────────
//
// Selector is the main thing operators get wrong — a range that
// references a program NAME not in the active list silently does
// nothing at runtime.  Flag it early so it's caught at edit-time.

export function selectorErrors($d: LightFxConfigT): string[] {
    const out: string[] = []
    const names = new Set($d.activePrograms.map(a => a.name))
    for (const r of $d.programSelector.ranges) {
        if (!r.program) { out.push('A range has no program picked.'); continue }
        if (!names.has(r.program)) {
            out.push(`Range references unknown program "${r.program}" (not in active list).`)
        }
        if (r.fromUs >= r.toUs) {
            out.push(`Range ${r.fromUs}–${r.toUs} µs is inverted (from ≥ to).`)
        }
    }
    if ($d.programSelector.enabled && !$d.programSelector.input) {
        out.push('Selector is enabled but no input channel picked.')
    }
    return out
}

/** Per-active-program validation.  Inlined-editor surfaces these next
 *  to each track + folds them into the global Apply gate via
 *  `lightfxHasErrors` so the operator can't push a broken program.
 *
 *  Channel-name resolution against /hubfx.yaml port labels is NOT
 *  checked here (the device-model store would have to be threaded in);
 *  the picker already filters to valid LedAnimator labels.  Firmware
 *  warns + skips unresolved tracks at load. */
export function programErrors(p: ProgramT): string[] {
    const out: string[] = []
    for (let i = 0; i < p.tracks.length; i++) {
        const t = p.tracks[i]
        if (!t.channel || !t.channel.trim()) out.push(`Track #${i + 1}: no channel picked.`)
        if (t.events.length === 0) out.push(`Track #${i + 1}: no events — track does nothing.`)
        for (let j = 0; j < t.events.length; j++) {
            const e = t.events[j]
            if (e.kind === 'flash'  && e.cycleMs === 0) out.push(`Track #${i+1} event #${j+1}: flash needs cycle_ms > 0.`)
            if (e.kind === 'fading' && e.cycleMs === 0) out.push(`Track #${i+1} event #${j+1}: fading needs cycle_ms > 0.`)
            if (e.kind === 'beacon' && e.cycleMs === 0) out.push(`Track #${i+1} event #${j+1}: beacon needs cycle_ms > 0.`)
            if (e.minPct > e.maxPct) out.push(`Track #${i+1} event #${j+1}: min_pct (${e.minPct}) > max_pct (${e.maxPct}).`)
        }
        // Duplicate channel refs across siblings (same program only —
        // sibling programs CAN share channels, the firmware tolerates
        // overlap).
        for (let k = i + 1; k < p.tracks.length; k++) {
            const o = p.tracks[k]
            if (o.channel && o.channel === t.channel) {
                out.push(`Track #${i+1}: duplicate channel "${t.channel}" (also used by track #${k+1}).`)
                break
            }
        }
    }
    return out
}

export const lightfxHasErrors: Readable<boolean> = derived(lightfxDraft, ($d) => {
    if (selectorErrors($d).length > 0) return true
    // Active program names must be unique — duplicate would clobber a
    // sibling's /lightfx/programs/<name>.yaml on Apply.
    const names = new Set<string>()
    for (const a of $d.activePrograms) {
        if (names.has(a.name)) return true
        names.add(a.name)
        if (programErrors(a.program).length > 0) return true
    }
    return false
})

// ─── DirtySource ──────────────────────────────────────────────────────

export const lightfxConfigSource: DirtySource = {
    id:        'lightfx',
    label:     'LightFX',
    isDirty:   lightfxDirty,
    hasErrors: lightfxHasErrors,
    apply:     saveLightFxConfig,
    refresh:   loadLightFxConfig,
}

// ─── Structured program types (Phase 2 editor) ───────────────────────
//
// Mirrors app_lightfx.go's ProgramDTO + ProgramChannelDTO +
// ProgramEventDTO + ProgramLandingBindingDTO field-for-field
// (camelCase JSON tags on the Go side → these types).
//
// `ProgramEventT` is intentionally identical to the preset
// `LightEventT` below, so the LIGHT_PRESETS library populates a
// channel's events list with no field translation.  Type alias kept
// for clarity — the editor reads/writes `ProgramEventT[]`.

export type LightEventKindT =
    | 'on' | 'off' | 'flash' | 'fade_in' | 'fade_out' | 'fading' | 'beacon'

export interface ProgramEventT {
    kind:          LightEventKindT
    durationMs:    number
    cycleMs:       number
    brightnessPct: number
    minPct:        number
    maxPct:        number
    flashPct:      number
}

// TrackT — v2 program track.  References the LED channel by NAME (the
// /hubfx.yaml port label).  Editing surface = name dropdown + events
// list; no per-track port picker (port lives on the IO tab where the
// channel is labelled).  brightnessPct overrides the master scale for
// just this track in this program.
export interface TrackT {
    channel:       string        // matches /hubfx.yaml port.label of a LedAnimator port
    brightnessPct: number        // per-track scale (0..100)
    loop:          boolean       // sets LightEventFlags::Loop on event[0]
    events:        ProgramEventT[]
}

export interface ProgramLandingBindingT {
    id:    number
    state: 'on' | 'off'
}

export interface ProgramT {
    schemaVersion:   number
    tracks:          TrackT[]
    landingBindings: ProgramLandingBindingT[]
}

export function defaultEvent(patch: Partial<ProgramEventT> = {}): ProgramEventT {
    return {
        kind: 'on', durationMs: 0, cycleMs: 0,
        brightnessPct: 100, minPct: 0, maxPct: 100, flashPct: 50,
        ...patch,
    }
}
export function defaultTrack(channelName: string = ''): TrackT {
    return {
        channel: channelName,
        brightnessPct: 100,
        loop: false,
        events: [defaultEvent({ kind: 'on', brightnessPct: 100 })],
    }
}
export function defaultProgram(): ProgramT {
    return { schemaVersion: 2, tracks: [], landingBindings: [] }
}

// (The modal openProgram editor was removed 2026-05-24 — the active-
// list refactor inlines the editor inside LightFxPanel.  The
// normaliseProgram helper migrated into loadLightFxConfig's per-slot
// hydration.)

// ─── Canonical per-channel event presets (operator quality-of-life) ──
//
// Hardcoded patterns the operator can drop into a per-channel events
// list when authoring a program YAML.  The Phase-2 program editor
// uses these for the "preset" dropdown — pick one and the events
// list is replaced with a known-good FAA-compliant pattern.  Names
// match the aviation-lighting vocabulary so a reviewer can spot
// what's running at a glance.

// `LightEventT` is the same shape as `ProgramEventT` above (the
// structured editor reads/writes these directly into a channel's
// events list — no field translation).  Kept as a type alias so
// older `LightEventT` references compile unchanged.
export type LightEventT = ProgramEventT
const ev = defaultEvent

export interface LightPresetT {
    id:    string             // stable key for the dropdown
    label: string             // human label
    note:  string             // one-line description (tooltip + caption)
    group: 'Aircraft' | 'Vehicle' | 'Naval' | 'Effects' | 'Generic'
    loop:  boolean            // phase-locked repeating pattern (sets channel.loop on apply)
    events: LightEventT[]
}

/** Order to render preset groups in the editor's <optgroup> headers.
 *  Generic first (universal placeholders), then domain-specific. */
export const PRESET_GROUP_ORDER: LightPresetT['group'][] = [
    'Generic', 'Aircraft', 'Vehicle', 'Naval', 'Effects',
]

// Curated preset library — imported from the archived StudioFx
// `light-data.ts` and re-encoded for the new wire schema.  Grouped
// into Aircraft / Vehicle / Naval / Effects + a Generic group for
// off / steady that work for any vehicle type.  The dropdown
// renders these inside `<optgroup>` headers driven by the `group`
// field; see ProgramEditorDialog § preset picker.
export const LIGHT_PRESETS: LightPresetT[] = [
    // ─── Generic ────────────────────────────────────────────────────
    {
        id:    'steady_on',
        label: 'Steady on (100 %)',
        note:  'Constant on at full brightness — universal placeholder + steady-light base (FAA nav, naval anchor, vehicle DRL).',
        group: 'Generic',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 100, durationMs: 0 })],
    },
    {
        id:    'off',
        label: 'Off',
        note:  'Channel forced off.  Useful as a placeholder before authoring.',
        group: 'Generic',
        loop:  false,
        events: [ev({ kind: 'off', durationMs: 0 })],
    },

    // ─── Aircraft (FAA-compliant patterns) ──────────────────────────
    {
        id:    'aircraft_nav_position',
        label: 'Nav position (steady, dim)',
        note:  'Steady position/nav light at 100 % — red port, green starboard, white tail (FAA 14 CFR § 91.209).',
        group: 'Aircraft',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 100, durationMs: 0 })],
    },
    {
        id:    'aircraft_nav_dimmed',
        label: 'Nav position (dimmed, 40 %)',
        note:  'Position light at 40 % for ground ops / formation flying.',
        group: 'Aircraft',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 40, durationMs: 0 })],
    },
    {
        id:    'aircraft_anticol_beacon',
        label: 'Anti-collision beacon (red, ~46/min)',
        note:  'Red beacon, 1300 ms cycle, 12 % flash — Airbus-style mechanical-beacon mimic (FAA anti-collision baseline).',
        group: 'Aircraft',
        loop:  true,
        events: [ev({ kind: 'beacon', cycleMs: 1300, minPct: 0, maxPct: 100, flashPct: 12 })],
    },
    {
        id:    'aircraft_beacon_rotating',
        label: 'Rotating beacon (wide sweep, ~46/min)',
        note:  'Red rotating-beacon sweep, 1300 ms cycle, 30 % beam — broader sweep than the strict anti-col beacon.',
        group: 'Aircraft',
        loop:  true,
        events: [ev({ kind: 'beacon', cycleMs: 1300, minPct: 0, maxPct: 100, flashPct: 30 })],
    },
    {
        id:    'aircraft_strobe_single',
        label: 'Strobe — single flash (~46/min)',
        note:  'White single-flash strobe, 1300 ms period, 4 % duty (~52 ms pulse).  FAA-compliant baseline.',
        group: 'Aircraft',
        loop:  true,
        events: [ev({ kind: 'flash', cycleMs: 1300, brightnessPct: 100, flashPct: 4 })],
    },
    {
        id:    'aircraft_strobe_single_fast',
        label: 'Strobe — single flash (1 Hz)',
        note:  'White single-flash strobe — 60 ms on / 940 ms off, 1 Hz period.  Common helicopter setup.',
        group: 'Aircraft',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  60 }),
            ev({ kind: 'off', durationMs: 940 }),
        ],
    },
    {
        id:    'aircraft_strobe_double',
        label: 'Strobe — double flash (~46/min)',
        note:  'Double-flash anti-collision pattern: 50 ms on / 100 ms gap / 50 ms on / 1100 ms gap.',
        group: 'Aircraft',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  50 }),
            ev({ kind: 'off', durationMs: 100 }),
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  50 }),
            ev({ kind: 'off', durationMs: 1100 }),
        ],
    },
    {
        id:    'aircraft_strobe_double_1hz',
        label: 'Strobe — double flash (1 Hz)',
        note:  'Tighter double-flash: 60 ms / 80 ms gap / 60 ms / 800 ms gap, 1 Hz cycle.  Matches the helicopter_flight preset.',
        group: 'Aircraft',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  60 }),
            ev({ kind: 'off', durationMs:  80 }),
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  60 }),
            ev({ kind: 'off', durationMs: 800 }),
        ],
    },
    {
        id:    'aircraft_strobe_triple',
        label: 'Strobe — triple flash (~46/min)',
        note:  'Triple-flash pattern: 40 ms on / 60 ms gap × 3 / 1060 ms gap.  Marine + emergency-services pattern.',
        group: 'Aircraft',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 40 }),
            ev({ kind: 'off', durationMs: 60 }),
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 40 }),
            ev({ kind: 'off', durationMs: 60 }),
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 40 }),
            ev({ kind: 'off', durationMs: 1060 }),
        ],
    },
    {
        id:    'aircraft_landing_light',
        label: 'Landing light (fade-in + steady)',
        note:  'Soft 400 ms fade-in to full, then steady — used on landing-light deployment so the operator doesn\'t see a hard snap.',
        group: 'Aircraft',
        loop:  false,
        events: [
            ev({ kind: 'fade_in', brightnessPct: 100, durationMs: 400 }),
            ev({ kind: 'on',      brightnessPct: 100, durationMs:   0 }),
        ],
    },
    {
        id:    'aircraft_taxi_hover',
        label: 'Taxi / hover (dim 80 %)',
        note:  'Reduced-power taxi or hover light — 80 % steady.  Less glare than landing-light full.',
        group: 'Aircraft',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 80, durationMs: 0 })],
    },
    {
        id:    'aircraft_formation',
        label: 'Formation light (15 %)',
        note:  'Very dim steady — formation flying / night ops where bright lights would dazzle wingmen.',
        group: 'Aircraft',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 15, durationMs: 0 })],
    },
    {
        id:    'aircraft_cabin',
        label: 'Cabin / interior (35 %)',
        note:  'Soft cabin lighting — 35 % steady.',
        group: 'Aircraft',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 35, durationMs: 0 })],
    },

    // ─── Vehicle ────────────────────────────────────────────────────
    {
        id:    'vehicle_headlight_lo',
        label: 'Headlight — low beam (70 %)',
        note:  'Steady headlight at 70 % — typical low-beam intensity.',
        group: 'Vehicle',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 70, durationMs: 0 })],
    },
    {
        id:    'vehicle_headlight_hi',
        label: 'Headlight — high beam (100 %)',
        note:  'Steady headlight at full — high-beam.',
        group: 'Vehicle',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 100, durationMs: 0 })],
    },
    {
        id:    'vehicle_brake',
        label: 'Brake light (100 %)',
        note:  'Steady brake light — 100 %.  Often paired with tail light at lower brightness.',
        group: 'Vehicle',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 100, durationMs: 0 })],
    },
    {
        id:    'vehicle_tail',
        label: 'Tail light (40 %)',
        note:  'Steady tail light at 40 % — dim baseline; brake-light pattern modulates on top.',
        group: 'Vehicle',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 40, durationMs: 0 })],
    },
    {
        id:    'vehicle_turn_signal',
        label: 'Turn signal (~86/min)',
        note:  'Flash at 700 ms cycle, 50 % duty — SAE J590 turn-signal cadence (60–120/min).',
        group: 'Vehicle',
        loop:  true,
        events: [ev({ kind: 'flash', cycleMs: 700, brightnessPct: 100, flashPct: 50 })],
    },
    {
        id:    'vehicle_emergency_strobe',
        label: 'Emergency strobe (fast, ~300/min)',
        note:  'Fast strobe at 200 ms cycle, 50 % duty — LED light-bar style emergency strobe.',
        group: 'Vehicle',
        loop:  true,
        events: [ev({ kind: 'flash', cycleMs: 200, brightnessPct: 100, flashPct: 50 })],
    },
    {
        id:    'vehicle_wig_wag',
        label: 'Emergency wig-wag (~120/min)',
        note:  'Alternating 250 ms on / 250 ms off — pair on two channels with opposite phase for wig-wag headlights.',
        group: 'Vehicle',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 250 }),
            ev({ kind: 'off', durationMs: 250 }),
        ],
    },

    // ─── Naval ──────────────────────────────────────────────────────
    {
        id:    'naval_masthead',
        label: 'Masthead (white forward, 100 %)',
        note:  'Steady white masthead light — 225° forward arc, COLREGS Rule 21.',
        group: 'Naval',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 100, durationMs: 0 })],
    },
    {
        id:    'naval_sidelight',
        label: 'Sidelight (port red / starboard green, 80 %)',
        note:  'Steady sidelight — 112.5° arc each, port red / starboard green.',
        group: 'Naval',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 80, durationMs: 0 })],
    },
    {
        id:    'naval_anchor',
        label: 'Anchor (white all-round, 100 %)',
        note:  'Steady white all-round — vessel at anchor, COLREGS Rule 30.',
        group: 'Naval',
        loop:  false,
        events: [ev({ kind: 'on', brightnessPct: 100, durationMs: 0 })],
    },
    {
        id:    'naval_signal_flash',
        label: 'Signal flash (3-pulse)',
        note:  'Three 150 ms pulses with 150 ms gaps, then 1500 ms quiet — generic signal-flash pattern.',
        group: 'Naval',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 150 }),
            ev({ kind: 'off', durationMs: 150 }),
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 150 }),
            ev({ kind: 'off', durationMs: 150 }),
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 150 }),
            ev({ kind: 'off', durationMs: 1500 }),
        ],
    },

    // ─── Effects ────────────────────────────────────────────────────
    {
        id:    'fx_beacon_short',
        label: 'Effect — short beacon (1 Hz)',
        note:  '10 % flash duty over 1000 ms — brief brightness peak with a dim baseline.  Generic flashing beacon.',
        group: 'Effects',
        loop:  true,
        events: [ev({ kind: 'beacon', cycleMs: 1000, minPct: 5, maxPct: 100, flashPct: 10 })],
    },
    {
        id:    'fx_beacon_rotating',
        label: 'Effect — sinusoidal rotating beacon (1 Hz)',
        note:  'Sinusoidal fade between 5 % and 100 % over 1000 ms — mimics a mechanical rotating beacon.',
        group: 'Effects',
        loop:  true,
        events: [ev({ kind: 'fading', cycleMs: 1000, minPct: 5, maxPct: 100 })],
    },
    {
        id:    'fx_breathing',
        label: 'Effect — breathing (3 s)',
        note:  'Slow sinusoidal pulse from 5 % to 80 %, 3 s cycle — calming aesthetic effect.',
        group: 'Effects',
        loop:  true,
        events: [ev({ kind: 'fading', cycleMs: 3000, minPct: 5, maxPct: 80 })],
    },
    {
        id:    'fx_fade_breath',
        label: 'Effect — breathing fade (2 s)',
        note:  '1 s fade in / 1 s fade out — linear pulsing effect (vs. fx_breathing which is sinusoidal).',
        group: 'Effects',
        loop:  true,
        events: [
            ev({ kind: 'fade_in',  brightnessPct: 100, durationMs: 1000 }),
            ev({ kind: 'fade_out', brightnessPct: 100, durationMs: 1000 }),
        ],
    },
    {
        id:    'fx_pulse',
        label: 'Effect — fast pulse',
        note:  '100 ms on / 100 ms off — fast crisp pulse for indicator / alert use.',
        group: 'Effects',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs: 100 }),
            ev({ kind: 'off', durationMs: 100 }),
        ],
    },

    // ─── Legacy id aliases (preserve external references to the
    //     Phase-1 ids; Studio looks them up by id when applying to a
    //     channel). ────────────────────────────────────────────────
    {
        id:    'faa_strobe_single',
        label: '(alias) Anti-collision strobe — single flash (1 Hz)',
        note:  'Compatibility alias for the new aircraft_strobe_single_fast preset.',
        group: 'Aircraft',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  60 }),
            ev({ kind: 'off', durationMs: 940 }),
        ],
    },
    {
        id:    'faa_strobe_double',
        label: '(alias) Anti-collision strobe — double flash (1 Hz)',
        note:  'Compatibility alias for the new aircraft_strobe_double_1hz preset.',
        group: 'Aircraft',
        loop:  true,
        events: [
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  60 }),
            ev({ kind: 'off', durationMs:  80 }),
            ev({ kind: 'on',  brightnessPct: 100, durationMs:  60 }),
            ev({ kind: 'off', durationMs: 800 }),
        ],
    },
    {
        id:    'beacon_rotating',
        label: '(alias) Rotating beacon (sinusoidal 1 Hz)',
        note:  'Compatibility alias for the new fx_beacon_rotating preset.',
        group: 'Effects',
        loop:  true,
        events: [ev({ kind: 'fading', cycleMs: 1000, minPct: 5, maxPct: 100 })],
    },
    {
        id:    'beacon_short',
        label: '(alias) Beacon — brief flash (1 Hz)',
        note:  'Compatibility alias for the new fx_beacon_short preset.',
        group: 'Effects',
        loop:  true,
        events: [ev({ kind: 'beacon', cycleMs: 1000, minPct: 5, maxPct: 100, flashPct: 10 })],
    },
    {
        id:    'fade_breath',
        label: '(alias) Breathing fade (linear, 2 s)',
        note:  'Compatibility alias for the new fx_fade_breath preset.',
        group: 'Effects',
        loop:  true,
        events: [
            ev({ kind: 'fade_in',  brightnessPct: 100, durationMs: 1000 }),
            ev({ kind: 'fade_out', brightnessPct: 100, durationMs: 1000 }),
        ],
    },
]
