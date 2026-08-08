// gunfx.ts — Studio store + Wails wrapper for the GunFX panel.
//
// Same shape as effects.ts's engineDraft / engineConfig / engineDirty
// triad: `gunfxConfig` mirrors what firmware loaded, `gunfxDraft` is what
// the operator is editing, `gunfxDirty` is the derived diff flag.  The
// dirty-aware loader pattern (snapshot `wasDirty` before mutating) is
// the same one the Engine panel learned the hard way.
//
// All Wails bindings are stubbed in app_gunfx.go (Phase 1).  Phase 2+3
// firmware actually parses /gunfx.yaml and routes the wire commands;
// Phase 4 just wires up the UI.

import { writable, derived, get } from 'svelte/store'
import type { DirtySource } from './dirty-registry'
import { suggestNextBand } from './range-suggest'
import {
    LoadGunFxConfig, SaveGunFxConfig, GunFxStatus,
    GunFire, GunFireWithRof,
    GunStartFiring, GunStartFiringWithRof,
    GunStopFiring, GunSmokeArm,
    GunManualSet, GunManualRelease, GunVerboseSubscribe,
} from '../../wailsjs/go/main/App'
import { EventsOn, EventsOff } from '../../wailsjs/runtime/runtime'

// ─── DTO mirrors (must match app_gunfx.go DTO names) ──────────────────

export interface PortRefT { board: string; guid: string; kind: string; idx: number }

export interface RofItemT {
    name: string
    bandLoUs: number
    bandHiUs: number
    rpm: number
    soundPath: string
    /** Stereo routing mask: bit 0 = left, bit 1 = right.
     *  0x03 = both (default), 0x01 = left only, 0x02 = right only. */
    outputMask: number
}

// Rule 43 — channel-input references are NAMES from the IO tab's
// `inputs:` block; firmware resolves to PortRef + channel at apply.
export interface RofConfigT {
    input: string
    items: RofItemT[]
}

export interface TriggerConfigT {
    input: string
    thresholdUs: number
    hysteresisUs: number
}

export interface MuzzleFlashT {
    port: PortRefT
    durationMs: number
    brightness: number
}

// Recoil — turret BEHAVIOUR, no dedicated servo.  2026-06: a role-level
// impulse applied to BOTH yaw + pitch on each shot — each axis gets a random
// ±offset up to `jerkUs` added to its servo OUTPUT for `holdMs`, then the
// ServoActuatorRole de-jerks it.  The kick rides ON TOP of the aim (the motion
// profile keeps tracking RC underneath), so it works moving or stationary —
// no snap-back, no RC suppression.  `holdMs` is an advanced dwell knob.
export interface RecoilConfigT {
    enabled: boolean
    jerkUs: number   // max random |offset| µs added to output
    holdMs: number   // how long the offset rides before the role de-jerks (advanced)
}

// Rule 44 — mirrors ServoMotionProfileDTO (app_gunfx.go); the same field
// shape is pushed live via SetPortProfile, so one ServoProfileEditor binds
// to both the inline-with-feature profile (persisted in /gunfx.yaml) and
// the live-tune wire commands.
export interface ServoMotionProfileT {
    minUs: number
    maxUs: number
    centerUs: number
    maxSpeedUsPerSec: number
    maxAccelUsPerSec2: number
    maxJerkUsPerSec3: number
}

// Rule 43 named-channel that gates the heater (Phase 4 polish
// 2026-05-26). Empty `input` ⇒ no gating (heater is always allowed when
// smoke is armed). Threshold/hysteresis are µs around the activation
// stick — same shape as `TriggerConfigT`.
export interface HeaterActivationT {
    input: string
    thresholdUs: number
    hysteresisUs: number
}

export type FanMode    = 'continuous' | 'pulse'
export type HeaterMode = 'continuous' | 'cycle'

// Heater fields (Phase 4 polish 2026-05-26).  cycle mode toggles the
// heater on/off at gun-layer intervals (cycleOnMs / cycleOffMs) — used
// for power conservation or to limit cartridge temperature without a
// thermistor.
export interface HeaterT {
    port: PortRefT
    elementMv: number                  ///< default 6 V smoke cartridge
    mode: HeaterMode                   ///< continuous | cycle
    cycleOnMs: number                  ///< CYCLE: on-phase duration (ms)
    cycleOffMs: number                 ///< CYCLE: off-phase duration (ms)
    activation: HeaterActivationT      ///< Rule 43 named-channel gate
}

export interface FanT {
    port: PortRefT
    elementMv: number                  ///< default 6 V smoke-fan motor
    mode: FanMode
    /** FN_PULSE sinusoid envelope duration in ms.  One shot = one
     *  envelope: 50% base → 100% peak (at half-duration) → 50% base.
     *  Default 100 ms matches the 600 RPM default firing rate so
     *  the sinusoid completes once per shot at default cadence. */
    pulseDurationMs: number
}

export interface SmokeConfigT { heater: HeaterT; fan: FanT }

export interface GunAxisT {
    enabled: boolean
    servoPort: PortRefT
    input: string                  // Rule 43 — named-channel reference
    neutralUs: number
    // Servo motion profile lives on the port-role row in /hubfx.yaml
    // (Rule 42 storage); the GunFx panel reads it via
    // `$deviceModel.ports[i].profile` and writes via `SetPortProfile`
    // (Rule 44 editing surface).  Not in /gunfx.yaml.
}

export interface GunT {
    id: number
    name: string
    trigger: TriggerConfigT
    rof: RofConfigT
    muzzleFlash: MuzzleFlashT
    recoil: RecoilConfigT
    smoke: SmokeConfigT
    yaw: GunAxisT
    pitch: GunAxisT
}

export interface GunFxConfigT {
    schemaVersion: number
    enabled: boolean
    guns: GunT[]
}

// Verbose-status event payload (firmware emits this at ~10 Hz per
// subscribed gun; we re-emit as `gun:verbose:<id>` from app_gunfx.go).
export interface GunVerboseStatusT {
    id: number
    mode: number             // 0 rc, 1 manual
    firing: boolean
    smokeArmed: boolean
    smokeFanRunning: boolean
    heaterDutyPct: number
    heaterTempCx10: number
    yawCurrentUs: number
    yawTargetUs: number
    pitchCurrentUs: number
    pitchTargetUs: number
    rofIndex: number
    rofSelectorUs: number
    triggerUs: number
    shotsThisSession: number
    /** Rule 43 activation gate state (Phase 4 polish 2026-05-26).
     *  true ⇒ activation channel above threshold OR no channel bound.
     *  false ⇒ channel bound and reading below threshold — heater forced
     *  off even when smokeArmed.  Studio uses this to render a
     *  "gated off" pill in the gun-smoke card header. */
    heaterActive: boolean
}

export interface GunStatusT { id: number; firing: boolean; smokeArmed: boolean }

// ─── Defaults ─────────────────────────────────────────────────────────

const emptyPort: PortRefT = { board: '', guid: '', kind: '', idx: 0 }

const defaultAxis = (): GunAxisT => ({
    enabled: false,
    servoPort: { ...emptyPort, kind: 'servo' },
    input: '',          // Rule 43 — operator picks a named channel
    neutralUs: 1500,
})

export function defaultGun(id: number): GunT {
    return {
        id, name: '',
        trigger: { input: '', thresholdUs: 1500, hysteresisUs: 25 },
        // Seed one default ROF item so a freshly-added gun is fireable
        // immediately (operator can rename / re-band / pick a sound).
        // Empty band ([0, 0]) means "armed regardless of selector
        // channel" — a sensible default when no selector is bound.
        rof: { input: '', items: [
            { name: 'rof1', bandLoUs: 0, bandHiUs: 0, rpm: 600, soundPath: '', outputMask: 0x03 },
        ] },
        // Empty ports default to {guid:"", kind:"", idx:0} so the
        // firmware skips driving them.  The operator picks a port in
        // the panel; until then NOTHING gets flashed / heated / etc.
        muzzleFlash: { port: { ...emptyPort }, durationMs: 30, brightness: 100 },
        recoil: { enabled: true, jerkUs: 200, holdMs: 80 },
        smoke: {
            heater: {
                port: { ...emptyPort },
                elementMv: 6000,                        // typical 6 V smoke cartridge
                mode: 'continuous',
                cycleOnMs: 5000,                        // cycle: 5 s on
                cycleOffMs: 3000,                       // cycle: 3 s off
                activation: { input: '', thresholdUs: 1500, hysteresisUs: 25 },
            },
            fan: {
                port: { ...emptyPort },
                elementMv: 6000,                        // typical 6 V fan motor
                mode: 'pulse',
                pulseDurationMs: 100,                   // sinusoid period = 600 RPM firing period
            },
        },
        yaw: defaultAxis(),
        pitch: defaultAxis(),
    }
}

export const emptyGunFxConfig = (): GunFxConfigT => ({ schemaVersion: 1, enabled: false, guns: [] })

// ─── Stores ───────────────────────────────────────────────────────────

/** What firmware loaded (last successful LoadGunFxConfig). */
export const gunfxConfig = writable<GunFxConfigT>(emptyGunFxConfig())
/** What the operator is editing. */
export const gunfxDraft  = writable<GunFxConfigT>(emptyGunFxConfig())

/** True when draft diverges from config. */
export const gunfxDirty = derived(
    [gunfxConfig, gunfxDraft],
    ([cfg, draft]) => JSON.stringify(cfg) !== JSON.stringify(draft),
)

// ─── Validation ───────────────────────────────────────────────────────

/** Returns the indices of ROF items whose [bandLoUs, bandHiUs] window
 *  overlaps with at least one sibling item.  Bands must NOT overlap
 *  (Rule 38) — the operator picks an item by driving the selector
 *  channel into a band; overlapping bands would arm two items at once.
 *  Studio surfaces both panel-level red borders AND a global Apply
 *  block (via `gunfxHasErrors` below). */
export function detectBandOverlaps(items: RofItemT[]): number[] {
    const out: number[] = []
    for (let i = 0; i < items.length; i++) {
        const a = items[i]
        for (let j = i + 1; j < items.length; j++) {
            const b = items[j]
            const al = a.bandLoUs || 1000, ah = a.bandHiUs || 2000
            const bl = b.bandLoUs || 1000, bh = b.bandHiUs || 2000
            if (al <= bh && bl <= ah) {
                if (!out.includes(i)) out.push(i)
                if (!out.includes(j)) out.push(j)
            }
        }
    }
    return out
}

/** True for any gun in the draft that has zero ROF items — an
 *  unfireable gun.  Only relevant when the effect is enabled
 *  (disabled effect = nothing fires, empty ROF is harmless). */
export function gunHasNoRof(g: GunT): boolean {
    return g.rof.items.length === 0
}

/** Aggregate validation state — true if the effect is enabled AND
 *  any gun has either (a) zero ROF items (unfireable) or (b) bands
 *  that overlap with another item in the same gun.  Feeds the
 *  dirty-registry so global Apply (Rule 35 + 45) is shaded out
 *  until the operator resolves the issue.  Pure derivation — no
 *  async, no side effects. */
export const gunfxHasErrors = derived(gunfxDraft, ($draft) => {
    if (!$draft || !$draft.enabled) return false
    return $draft.guns.some(g =>
        gunHasNoRof(g) ||
        (g.rof.items.length > 1 && detectBandOverlaps(g.rof.items).length > 0))
})

/** Pre-built `DirtySource` for the dirty-registry — register from
 *  App.svelte startup so the global Apply order is deterministic.
 *  Rule 46 (modular config sources): each domain module owns its full
 *  source descriptor; the panel is a pure view. */
export const gunfxConfigSource: DirtySource = {
    id:        'gunfx',
    label:     'GunFX',
    isDirty:   gunfxDirty,
    hasErrors: gunfxHasErrors,
    apply:     saveGunFxConfig,
    refresh:   loadGunFxConfig,
}

/** Live verbose-status mirror, keyed by gun id.  Populated by the
 *  `gun:verbose` Wails event subscription installed below. */
export const gunfxVerbose = writable<Record<number, GunVerboseStatusT>>({})

/** Light status (firing + smoke) per gun, populated by polling
 *  GunFxStatus on demand.  Studio Phase 4 calls refreshGunFxStatus()
 *  on Apply or on a status-tab open. */
export const gunfxStatus = writable<GunStatusT[]>([])

// ─── Loaders / savers ─────────────────────────────────────────────────

/** Download /gunfx.yaml, populate `gunfxConfig`, and (if the draft
 *  wasn't already dirty) seed `gunfxDraft` with the loaded values. */
export async function loadGunFxConfig(): Promise<void> {
    const raw = (await LoadGunFxConfig()) as GunFxConfigT
    const n = normaliseGunFxConfig(raw)
    const wasDirty = get(gunfxDirty)   // SNAPSHOT FIRST (engine-panel lesson)
    gunfxConfig.set(n)
    if (!wasDirty) gunfxDraft.set(structuredClone(n))
}

/** Hydrate fields that fresh /gunfx.yaml files (or YAMLs hand-edited
 *  without the activation block) omit so the UI doesn't read undefined
 *  / zero where it expects sensible defaults:
 *    - heater.activation block: missing ⇒ stick-midpoint defaults so
 *      the threshold slider opens at 1500 µs, not 0.
 *    - heater.elementMv / fan.elementMv: 0 ⇒ 6000 (typical smoke
 *      cartridge / fan motor).
 *    - heater.mode / fan.mode: empty ⇒ canonical default.
 *  Legacy mode strings are NOT remapped here — back-compat dropped
 *  2026-05-26.  A pre-cleanup YAML with `always_on` / `closed_loop` /
 *  `off` / `puff_per_shot` / `puff_on_fire_active` will hit the
 *  firmware parser's "unknown mode" warn path and default to the
 *  canonical value; the operator re-saves through Studio to update
 *  the file on-disk.
 */
function normaliseGunFxConfig(c: GunFxConfigT | null): GunFxConfigT {
    if (!c) return emptyGunFxConfig()
    const out: GunFxConfigT = structuredClone(c)
    out.schemaVersion = out.schemaVersion ?? 1
    out.enabled       = !!out.enabled
    out.guns          = (out.guns ?? []).map(g => {
        const h = g.smoke?.heater
        const f = g.smoke?.fan
        if (h) {
            if (!h.activation || typeof h.activation.thresholdUs !== 'number') {
                h.activation = { input: '', thresholdUs: 1500, hysteresisUs: 25 }
            } else {
                if (!h.activation.thresholdUs)  h.activation.thresholdUs  = 1500
                if (!h.activation.hysteresisUs) h.activation.hysteresisUs = 25
                if (typeof h.activation.input !== 'string') h.activation.input = ''
            }
            if (!h.elementMv)   h.elementMv   = 6000
            if (!h.mode)        h.mode        = 'continuous'
            // Remap any legacy 'bang_bang' YAML (briefly shipped 2026-05-26)
            // to the canonical 'continuous' — without a temp sensor wired
            // the role degraded bang_bang to continuous anyway.
            if ((h.mode as any) === 'bang_bang') h.mode = 'continuous'
            if (!h.cycleOnMs)   h.cycleOnMs   = 5000
            if (!h.cycleOffMs)  h.cycleOffMs  = 3000
        }
        if (f) {
            if (!f.elementMv) f.elementMv = 6000
            if (!f.mode)      f.mode      = 'pulse'
            // Remap legacy 'puff_per_fire' YAMLs to the canonical 'pulse'.
            if ((f.mode as any) === 'puff_per_fire') f.mode = 'pulse'
            if (!f.pulseDurationMs) f.pulseDurationMs = 100
        }
        return g
    })
    return out
}

/** Save the current draft to /gunfx.yaml + ask the hub to reload it. */
export async function saveGunFxConfig(): Promise<void> {
    const cfg = get(gunfxDraft)
    await SaveGunFxConfig(cfg)
    gunfxConfig.set(structuredClone(cfg))   // optimistic — server reload is fire-and-confirm
}

export async function refreshGunFxStatus(): Promise<void> {
    const st = (await GunFxStatus()) as GunStatusT[] | null
    gunfxStatus.set(st ?? [])
}

// ─── Runtime control wrappers ─────────────────────────────────────────

export async function gunFire(id: number): Promise<void> { return GunFire(id) }
/** Single shot with explicit ROF index — 0xFF (=255) means "use the
 *  firmware's currently-armed ROF" (same as `gunFire`).  Used by the
 *  Studio Fire button when the operator picks a program from the
 *  dropdown so manual tests always have a concrete program even when
 *  the RC selector stick is between bands (2026-05-24). */
export async function gunFireWithRof(id: number, rofIndex: number): Promise<void> {
    return GunFireWithRof(id, rofIndex)
}
export async function gunStartFiring(id: number, rpm: number): Promise<void> { return GunStartFiring(id, rpm) }
/** Auto-fire burst with explicit ROF index — counterpart to gunFireWithRof. */
export async function gunStartFiringWithRof(id: number, rpm: number, rofIndex: number): Promise<void> {
    return GunStartFiringWithRof(id, rpm, rofIndex)
}
export async function gunStopFiring(id: number): Promise<void> { return GunStopFiring(id) }
export async function gunSmokeArm(id: number, armed: boolean): Promise<void> { return GunSmokeArm(id, armed ? 1 : 0) }

// Manual override — bitmask flags + per-field values.  Match
// gunfx.ManualFlag* constants (kept in sync with gunfx_protocol.h).
export const ManualFlag = {
    Yaw:      0x01,
    Pitch:    0x02,
    Rof:      0x04,
    Fire:     0x08,
    Smoke:    0x10,
    FanBurst: 0x20,
} as const

export interface GunManualStateT {
    flags: number
    yawUs: number
    pitchUs: number
    rofIndex: number
    fireHold: number
    smokeArm: number
    smokeFanBurst: number
}

export async function gunManualSet(id: number, state: GunManualStateT): Promise<void> {
    return GunManualSet(id, state as any)
}
export async function gunManualRelease(id: number): Promise<void> { return GunManualRelease(id) }
export async function gunVerboseSubscribe(id: number, enable: boolean): Promise<void> {
    return GunVerboseSubscribe(id, enable ? 1 : 0)
}

// ─── Verbose-status event plumbing ────────────────────────────────────

let verboseRegistered = false
let cancelVerbose: (() => void) | null = null

/** Install the Wails event listener for `gun:verbose`.  Idempotent —
 *  the GunFxPanel mounts it once; multiple mounts share one listener. */
export function installVerboseListener(): void {
    if (verboseRegistered) return
    verboseRegistered = true
    cancelVerbose = EventsOn('gun:verbose', (ev: GunVerboseStatusT) => {
        gunfxVerbose.update(m => ({ ...m, [ev.id]: ev }))
    })
}

/** Tear it down — call from a top-level onDestroy if needed. */
export function uninstallVerboseListener(): void {
    if (cancelVerbose) cancelVerbose()
    EventsOff('gun:verbose')
    verboseRegistered = false
    cancelVerbose = null
}

// ─── Mutators (draft only) ────────────────────────────────────────────
// These all operate on the DRAFT store; saving pushes it to firmware.

export function addGun(): void {
    gunfxDraft.update(c => {
        const used = new Set(c.guns.map(g => g.id))
        let id = 0
        while (used.has(id)) id++
        return { ...c, guns: [...c.guns, defaultGun(id)] }
    })
}

export function removeGun(id: number): void {
    gunfxDraft.update(c => ({ ...c, guns: c.guns.filter(g => g.id !== id) }))
}

export function setEnabled(on: boolean): void {
    gunfxDraft.update(c => {
        if (!on) return { ...c, enabled: false }
        // Auto-seed on enable:
        //   • zero guns → drop in a default gun #0
        //   • any gun with zero ROF items → seed one ROF (a gun
        //     without a ROF item is unfireable and flagged as an
        //     error by gunfxHasErrors — the operator shouldn't have
        //     to manually fix that on the click that enables the
        //     effect).
        const guns = c.guns.length === 0
            ? [defaultGun(0)]
            : c.guns.map(g => g.rof.items.length === 0
                ? { ...g, rof: { ...g.rof, items: [
                    { name: 'rof1', bandLoUs: 0, bandHiUs: 0, rpm: 600, soundPath: '', outputMask: 0x03 },
                  ] } }
                : g)
        return { ...c, enabled: true, guns }
    })
}

export function updateGun(id: number, mutate: (g: GunT) => GunT): void {
    gunfxDraft.update(c => ({
        ...c,
        guns: c.guns.map(g => g.id === id ? mutate(g) : g),
    }))
}

/** Suggest a [loUs, hiUs] band for the NEXT ROF item that does not
 *  overlap any existing bands.  Algorithm:
 *    1. If there are no items, return the full RC range (1000–2000 µs).
 *    2. Build a sorted list of explicit (non-unbounded) bands; merge
 *       any unbounded item ([0,0] / [0,n] / [n,0]) into a full-cover
 *       sentinel so the gap search treats it as "everything".
 *    3. Scan for the largest empty gap in [1000, 2000] between sorted
 *       band edges; if its width ≥ 100 µs, hand back that gap.
 *    4. If no usable gap exists (bar is fully covered or only
 *       sub-100µs slivers remain), slice the largest existing band in
 *       half and return its upper half — caller's mutator handles the
 *       trim of the original.  Falls back to a small slice at the top
 *       (1800–2000) if everything else is degenerate.
 *  Operator-visible: a fresh item never collides with a sibling, and
 *  every new item lands SOMEWHERE on the bar so the overlay shows it
 *  immediately (the [0,0] default used to make every new item span
 *  the entire bar and stack invisibly under siblings). */
export function suggestNextRofBand(items: RofItemT[]): { lo: number; hi: number } {
    // Thin wrapper around the shared suggester so callers that only
    // need the band (not the trim-sibling info) stay compact.
    // `addRofItem` uses the full SuggestResult so it can apply the
    // guard-aware bisect trim.  Unbounded items (lo=0 OR hi=0) are
    // dropped from the input — they effectively cover everything and
    // would always force a bisect; better to ignore them and let the
    // suggester see only explicit bands.
    const explicit = items
        .filter(i => i.bandLoUs > 0 && i.bandHiUs > 0 && i.bandHiUs > i.bandLoUs)
        .map(i => ({ lo: i.bandLoUs, hi: i.bandHiUs }))
    return suggestNextBand(explicit).band
}

export function addRofItem(gunId: number): void {
    updateGun(gunId, g => {
        // Drop unbounded items (zero-edge wildcards) for the suggest
        // call, but remember the original index so the trim instruction
        // maps back to the right ROF entry when bisecting.
        const indexed = g.rof.items.map((it, idx) => ({ it, idx }))
            .filter(({ it }) => it.bandLoUs > 0 && it.bandHiUs > 0 && it.bandHiUs > it.bandLoUs)
        const explicit = indexed.map(({ it }) => ({ lo: it.bandLoUs, hi: it.bandHiUs }))
        const suggest  = suggestNextBand(explicit)
        const trimRealIdx = suggest.trimSiblingIdx >= 0 ? indexed[suggest.trimSiblingIdx].idx : -1

        const items = g.rof.items.map((it, i) =>
            i === trimRealIdx ? { ...it, bandHiUs: suggest.trimSiblingNewHi } : it)

        return {
            ...g,
            rof: {
                ...g.rof,
                items: [...items, {
                    name: `rof${g.rof.items.length + 1}`,
                    bandLoUs: suggest.band.lo, bandHiUs: suggest.band.hi,
                    rpm: 600, soundPath: '', outputMask: 0x03,
                }],
            },
        }
    })
}

export function removeRofItem(gunId: number, index: number): void {
    updateGun(gunId, g => ({
        ...g,
        rof: { ...g.rof, items: g.rof.items.filter((_, i) => i !== index) },
    }))
}
