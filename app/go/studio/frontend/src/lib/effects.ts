// ScaleFX Studio — effects (EngineFX, GunFX) client layer.
//
// Mirrors the Go EngineConfig DTO and the runtime control + state
// subscription.  Engine config lives in /enginefx.yaml on flash — the
// backend does the YAML round-trip; this file just exchanges typed
// structs.  Runtime state flows in via the `engine:state` Wails event.

import { writable, derived, get } from 'svelte/store'
import {
    GetEngineConfig, SetEngineConfig, EngineStart, EngineStop, EngineStatus,
    CheckFiles,
} from '../../wailsjs/go/main/App'
import { EventsOn } from '../../wailsjs/runtime/runtime'

// ─── EngineFX ─────────────────────────────────────────────────────────

export interface EngineToggle {
    input: string         // channel-function id (e.g. "engine_onoff")
    thresholdUs: number
    hysteresisUs: number
    failsafe: string      // "hold" | "force_low" | "force_high"
}
export interface EngineTransitions {
    startingOffsetMs: number
    stoppingOffsetMs: number
    startFadeInMs: number
    stopFadeOutMs: number
}
export interface EngineSounds {
    starting: string
    running: string
    stopping: string
    transitions: EngineTransitions
}
export interface EngineConfigT {
    schemaVersion: number
    enabled: boolean
    type: string          // "turbine" | "radial" | "diesel" (informational)
    output: string        // "left" | "right" | "both"
    toggle: EngineToggle
    sounds: EngineSounds
}

const defaultEngine: EngineConfigT = {
    schemaVersion: 1,
    enabled: false,
    type: 'turbine',
    output: 'both',
    toggle: { input: '', thresholdUs: 1500, hysteresisUs: 50, failsafe: 'force_low' },
    sounds: {
        starting: '', running: '', stopping: '',
        transitions: { startingOffsetMs: 0, stoppingOffsetMs: 0, startFadeInMs: 0, stopFadeOutMs: 0 },
    },
}

// engineConfig    — last loaded/applied state (truth from device).
// engineDraft     — working copy the EnginePanel edits.  Lives in a store
//                   so it survives tab switches (component unmount/remount
//                   no longer resets the form).
// engineDirty     — derived: draft differs from the loaded state.
export const engineConfig = writable<EngineConfigT>(defaultEngine)
export const engineDraft  = writable<EngineConfigT>(structuredClone(defaultEngine))
export const engineDirty  = derived(
    [engineConfig, engineDraft],
    ([$c, $d]) => JSON.stringify($c) !== JSON.stringify($d),
)

export interface EngineStatusT {
    state: number         // 0 Stopped · 1 Starting · 2 Running · 3 Stopping
    stateName: string
    engaged: boolean      // RC toggle currently held over threshold
}
export const engineStatus = writable<EngineStatusT>({ state: 0, stateName: 'stopped', engaged: false })

// Engine types catalogued for the UI — only turbine is fleshed out today.
export const ENGINE_TYPES = [
    { id: 'turbine', label: 'Turbine' },
    { id: 'radial',  label: 'Radial (planned)' },
    { id: 'diesel',  label: 'Diesel (planned)' },
] as const

export const FAILSAFE_MODES = [
    { id: 'hold',         label: 'Hold last value' },
    { id: 'force_low',    label: 'Force OFF (below threshold)' },
    { id: 'force_high',   label: 'Force ON (above threshold)' },
] as const

export const OUTPUT_MODES = [
    { id: 'both',  label: 'Stereo (both channels)' },
    { id: 'left',  label: 'Left only' },
    { id: 'right', label: 'Right only' },
] as const

export async function loadEngineConfig(): Promise<void> {
    try {
        const cfg = await GetEngineConfig() as EngineConfigT
        if (cfg) {
            const n = normaliseEngine(cfg)
            // Snapshot the dirty flag BEFORE updating engineConfig — once we
            // overwrite the loaded baseline the derived dirty store flips
            // true, and we'd refuse to seed the draft (which is exactly the
            // bug that left forms empty after a successful load).
            const wasDirty = get(engineDirty)
            engineConfig.set(n)
            if (!wasDirty) engineDraft.set(structuredClone(n))
        }
    } catch { /* device offline / no file — keep defaults */ }
}

/** Apply pushes the draft to the firmware (writes /enginefx.yaml + reload). */
export async function applyEngineConfig(): Promise<void> {
    const cfg = normaliseEngine(get(engineDraft))
    await SetEngineConfig(cfg as any)
    engineConfig.set(cfg)
    engineDraft.set(structuredClone(cfg))
}

function normaliseEngine(cfg: Partial<EngineConfigT>): EngineConfigT {
    const c = { ...defaultEngine, ...cfg } as EngineConfigT
    c.toggle = { ...defaultEngine.toggle, ...(cfg.toggle ?? {}) }
    c.sounds = {
        ...defaultEngine.sounds,
        ...(cfg.sounds ?? {}),
        transitions: { ...defaultEngine.sounds.transitions, ...(cfg.sounds?.transitions ?? {}) },
    }
    if (!c.schemaVersion) c.schemaVersion = 1
    return c
}

export async function engineStart(): Promise<void> { await EngineStart() }
export async function engineStop(): Promise<void> { await EngineStop() }

export async function refreshEngineStatus(): Promise<void> {
    try {
        const s = await EngineStatus() as EngineStatusT
        if (s) engineStatus.set(s)
    } catch { /* not connected */ }
}

// ─── File existence (sound-file validation on SD) ─────────────────────

export interface FileCheckT { path: string; exists: boolean; size: number; err?: string }

/** checkFiles probes a list of paths on the device's audio storage and
 *  returns a map of path → exists.  Used by the Engine panel to mark
 *  missing sound files red. */
export async function checkFiles(paths: string[]): Promise<Record<string, boolean>> {
    const out: Record<string, boolean> = {}
    const nonEmpty = paths.filter(p => !!p)
    if (nonEmpty.length === 0) return out
    try {
        const results = (await CheckFiles(nonEmpty)) as FileCheckT[]
        for (const r of results) out[r.path] = !!r.exists
    } catch {
        for (const p of nonEmpty) out[p] = false
    }
    return out
}

// ─── GunFX ─────────────────────────────────────────────────────────────
// Moved to `lib/gunfx.ts` in Phase 4a of the GunFX rollout
// (instructions/22).  The placeholder `gunFxDraft` here was a
// boolean-only stub; the per-gun config + Wails wrappers + verbose-status
// event plumbing all live in gunfx.ts now.

let stateBridged = false
export function installEngineStateBridge() {
    if (stateBridged) return
    stateBridged = true
    EventsOn('engine:state', (s: Partial<EngineStatusT>) => {
        if (!s) return
        engineStatus.update(prev => ({ ...prev, state: s.state ?? prev.state, stateName: s.stateName ?? prev.stateName }))
    })
}
