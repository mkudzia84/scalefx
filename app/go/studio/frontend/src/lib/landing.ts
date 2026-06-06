// landing.ts — Landing-Light domain store (Lighting tab right column).
//
// Mirrors the engine + gunfx triad pattern (Rule 46):
//   landingConfig  — last loaded/applied state (truth from device)
//   landingDraft   — what the operator typed
//   landingDirty   — derived: draft differs from config
//   landingConfigSource — DirtySource exposed to the global ConfigToolbar
//
// Plus the runtime live-state stream — Wails event `landing:phase`
// (emitted by app_landing.go::installLandingStream) lands in
// `landingPhases` so the per-group pill (Retracted / Deploying /
// Deployed / Retracting) updates without polling.

import { writable, derived, get, type Readable } from 'svelte/store'
import type { DirtySource } from './dirty-registry'
import {
    GetLandingConfig, SetLandingConfig,
    LandingActivate, LandingDeactivate, LandingStatus,
} from '../../wailsjs/go/main/App'
import { EventsOn, EventsOff } from '../../wailsjs/runtime/runtime'

// ─── DTOs (mirror app_landing.go) ─────────────────────────────────────

export interface PortRefT {
    board: string         // alias from /hubfx.yaml (or "hub" / "")
    guid:  string         // optional override
    kind:  string         // "servo" | "pwm"
    idx:   number
}

export interface LandingServoT { port: PortRefT }
export interface LandingLedT   { port: PortRefT; brightnessPct: number }

/** Activation source — what triggers deploy/retract.  Three modes:
 *   - "manual"        : only Studio's Activate / Deactivate buttons
 *                        (or the wire LL_SET_STATE) drive the group.
 *   - "input_channel" : an RC named-channel + threshold (deploy when
 *                        held above; retract when held below).
 *   - "program"       : tied to a LightFx program — deploy when the
 *                        named program is the active selection.
 *  Mode is YAML-driven; firmware-side wiring for the program/channel
 *  modes lands incrementally (today the GUI persists the choice;
 *  Studio's local activate/deactivate path always works). */
export interface LandingActivationT {
    mode: 'manual' | 'input_channel' | 'program'
    input:        string
    thresholdUs:  number
    hysteresisUs: number
    program:      string
    whenProgram:  'active' | 'inactive'
}

export interface LandingLightT {
    id:         number
    name:       string
    owner:      'lightfx' | 'gunfx' | 'gearcontrol' | 'landing-light'
    servos:     LandingServoT[]      // multi-servo (2026-05-24)
    openUs:     number
    closeUs:    number
    leds:       LandingLedT[]
    fadeInMs:   number               // LED soft-start after servo deploys (0 = hard on)
    activation: LandingActivationT
}

export interface LandingConfigT {
    schemaVersion: number
    lights: LandingLightT[]
}

export interface LandingPhaseEventT {
    id:    number
    phase: number         // 0..4 — Unconfigured / Retracted / Deploying / Deployed / Retracting
    name:  string         // canonical name from landing.PhaseName()
}
export interface LandingStatusT {
    id:        number
    phase:     number
    phaseName: string
    owner:     number
}

// ─── Defaults ─────────────────────────────────────────────────────────

const defaultActivation: LandingActivationT = {
    mode: 'manual', input: '', thresholdUs: 1500, hysteresisUs: 50,
    program: '', whenProgram: 'active',
}
const defaultLanding: LandingConfigT = { schemaVersion: 1, lights: [] }

export function defaultLandingLight(id: number): LandingLightT {
    return {
        // open/close are extreme sentinels — the servo role clamps them to its
        // own calibrated min/max, so "deploy" always lands on the real endpoint
        // (open ≥ close ⇒ deploy-at-max; toggled in the panel's Deploy direction).
        id, name: `landing${id}`, owner: 'landing-light',
        servos: [], openUs: 2500, closeUs: 500,
        leds: [],
        fadeInMs: 400,
        activation: { ...defaultActivation },
    }
}

// ─── Stores ───────────────────────────────────────────────────────────

export const landingConfig = writable<LandingConfigT>(defaultLanding)
export const landingDraft  = writable<LandingConfigT>(structuredClone(defaultLanding))
export const landingDirty  = derived(
    [landingConfig, landingDraft],
    ([$c, $d]) => JSON.stringify($c) !== JSON.stringify($d),
)

/** Per-light id → last phase seen, populated by the `landing:phase`
 *  event stream + seeded on mount via `refreshLandingStatus()`. */
export const landingPhases = writable<Record<number, LandingPhaseEventT>>({})

// ─── Validation ───────────────────────────────────────────────────────
//
// A group has errors when:
//   - it has zero LEDs (an empty LED list is the firmware's skip
//     criterion at apply time — silently dropped, looks like a bug
//     from the operator's perspective).
//   - id duplicated across siblings (firmware uses id for owner +
//     wire addressing; duplicates clobber the prior entry on apply).
//   - activation.mode === 'input_channel' AND input is empty
//   - activation.mode === 'program'      AND program is empty
//
// Aggregated into `landingHasErrors` for the DirtySource — gates the
// global Apply per Rule 35 / 45.

export function landingItemErrors(items: LandingLightT[], idx: number): string[] {
    const it = items[idx]
    const out: string[] = []
    if (!it) return out
    if (it.leds.length === 0) {
        out.push('No LEDs assigned — the firmware silently drops groups without LEDs.')
    }
    for (let i = 0; i < items.length; i++) {
        if (i !== idx && items[i].id === it.id) {
            out.push(`Duplicate id ${it.id} — every group needs a unique id.`)
            break
        }
    }
    if (it.activation.mode === 'input_channel' && !it.activation.input) {
        out.push('Activation = "input channel" but no channel picked.')
    }
    if (it.activation.mode === 'program' && !it.activation.program) {
        out.push('Activation = "program" but no program picked.')
    }
    return out
}
export const landingHasErrors: Readable<boolean> = derived(landingDraft, ($draft) => {
    return $draft.lights.some((_, i) => landingItemErrors($draft.lights, i).length > 0)
})

// ─── Loader / saver ───────────────────────────────────────────────────

export async function loadLandingConfig(): Promise<void> {
    const wasDirty = get(landingDirty)
    const cfg = normaliseLanding(await GetLandingConfig() as any)
    landingConfig.set(cfg)
    if (!wasDirty) landingDraft.set(structuredClone(cfg))
}
export async function saveLandingConfig(): Promise<void> {
    const cfg = get(landingDraft)
    await SetLandingConfig(cfg as any)
    landingConfig.set(structuredClone(cfg))
}
export async function refreshLandingStatus(): Promise<void> {
    const st = await LandingStatus() as LandingStatusT[]
    if (!st) return
    landingPhases.update(m => {
        const next = { ...m }
        for (const s of st) {
            next[s.id] = { id: s.id, phase: s.phase, name: s.phaseName }
        }
        return next
    })
}

/** Backend may omit fields on a fresh device (no /landing.yaml yet) —
 *  fill in the gaps so the form-binding code can assume the shape. */
function normaliseLanding(c: any): LandingConfigT {
    const cfg: LandingConfigT = {
        schemaVersion: c?.schemaVersion ?? 1,
        lights: Array.isArray(c?.lights) ? c.lights : [],
    }
    cfg.lights = cfg.lights.map(l => ({
        id:        l?.id ?? 0,
        name:      l?.name ?? '',
        owner:     l?.owner ?? 'landing-light',
        servos:    Array.isArray(l?.servos) ? l.servos : [],
        openUs:    l?.openUs ?? 1900,
        closeUs:   l?.closeUs ?? 1100,
        leds:      Array.isArray(l?.leds) ? l.leds : [],
        activation: { ...defaultActivation, ...(l?.activation ?? {}) },
    }))
    return cfg
}

// ─── Mutators (draft only — saving pushes to firmware) ────────────────

export function addLandingLight(): void {
    landingDraft.update(c => {
        const used = new Set(c.lights.map(l => l.id))
        let id = 0
        while (used.has(id) && id < 255) id++
        return { ...c, lights: [...c.lights, defaultLandingLight(id)] }
    })
}
export function removeLandingLight(id: number): void {
    landingDraft.update(c => ({ ...c, lights: c.lights.filter(l => l.id !== id) }))
}
export function updateLandingLight(id: number, mutate: (l: LandingLightT) => LandingLightT): void {
    landingDraft.update(c => ({
        ...c,
        lights: c.lights.map(l => l.id === id ? mutate(l) : l),
    }))
}

// ─── Runtime control wrappers ─────────────────────────────────────────

export async function landingActivate(id: number): Promise<void>   { return LandingActivate(id) }
export async function landingDeactivate(id: number): Promise<void> { return LandingDeactivate(id) }

// ─── Phase-event listener (idempotent) ────────────────────────────────

let phaseRegistered = false
let cancelPhase: (() => void) | null = null
export function installLandingPhaseListener(): void {
    if (phaseRegistered) return
    phaseRegistered = true
    cancelPhase = EventsOn('landing:phase', (ev: LandingPhaseEventT) => {
        landingPhases.update(m => ({ ...m, [ev.id]: ev }))
    })
}
export function uninstallLandingPhaseListener(): void {
    if (cancelPhase) cancelPhase()
    EventsOff('landing:phase')
    phaseRegistered = false
    cancelPhase = null
}

// ─── DirtySource (Rule 46) ────────────────────────────────────────────

export const landingConfigSource: DirtySource = {
    id:        'landing',
    label:     'Retractable Lights',
    isDirty:   landingDirty,
    hasErrors: landingHasErrors,
    apply:     saveLandingConfig,
    refresh:   loadLandingConfig,
}
