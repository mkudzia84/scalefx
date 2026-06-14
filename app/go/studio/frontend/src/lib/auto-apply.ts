// auto-apply — optional "persist on settle" controller for the global
// dirty system.  When ON, a config edit that leaves a draft dirty arms a
// short VISIBLE countdown; if the operator presses neither Apply nor Hold
// first, the dirty sources are persisted automatically via applyAll().
//
// This persists to LIVE hardware (a hub apply re-attaches roles), so the
// design is deliberately conservative:
//   - VISIBLE countdown + Hold + the normal Apply button (never a silent
//     fire) — the operator always sees it coming and can veto.
//   - Held by validation errors (Rule 35) and suppressed while the Setup
//     Wizard owns its own Apply.
//   - Reuses applyAll(), which only touches DIRTY sources — so an effect
//     tweak never cycles the hub role roster.
//   - Per-edit debounce: each edit re-arms the timer, so it fires only
//     after editing settles, not mid-keystroke.

import { writable, get, type Readable } from 'svelte/store'
import { anyDirty, anyErrors, applyAll } from './dirty-registry'
import { showConfigWizard } from './stores'
import { engineDraft } from './effects'
import { gunfxDraft } from './gunfx'
import { lightfxDraft } from './lightfx'
import { landingDraft } from './landing'
import { gearDraft } from './gear'
import { deviceModel } from './devicemodel'

const DELAY_MS = 5000
const TICK_MS = 250
const LS_KEY = 'scalefx.autoApply.enabled'

function loadEnabled(): boolean {
    try { return localStorage.getItem(LS_KEY) !== '0' } catch { return true } // default ON
}

/** Operator toggle (persisted).  OFF = classic manual-Apply behaviour. */
export const autoApplyEnabled = writable<boolean>(loadEnabled())
autoApplyEnabled.subscribe(v => { try { localStorage.setItem(LS_KEY, v ? '1' : '0') } catch { /* private mode */ } })

/** Seconds left on the visible countdown; 0 when idle. */
export const autoApplySecondsLeft = writable<number>(0)

/** True while an apply (manual OR auto) is in flight — shared so the
 *  countdown and the manual Apply button never double-fire. */
export const applyInFlight = writable<boolean>(false)

/** Last auto-apply error (surfaced in the toolbar banner). */
export const autoApplyError = writable<string>('')

let deadline = 0
let ticker: ReturnType<typeof setInterval> | null = null

function stopTicker(): void {
    if (ticker != null) { clearInterval(ticker); ticker = null }
    deadline = 0
    autoApplySecondsLeft.set(0)
}

/** Pre-conditions for an auto countdown to run at all. */
function gated(): boolean {
    return get(autoApplyEnabled) && !get(showConfigWizard) && !get(applyInFlight)
}

/** (Re)arm the countdown.  Called on every edit (debounce) and on the
 *  dirty rising edge.  No-op when nothing is dirty or a gate is closed. */
function arm(): void {
    if (!gated() || !get(anyDirty)) { return }
    deadline = Date.now() + DELAY_MS
    autoApplySecondsLeft.set(Math.ceil(DELAY_MS / 1000))
    if (ticker == null) ticker = setInterval(onTick, TICK_MS)
}

async function onTick(): Promise<void> {
    if (!get(anyDirty) || !gated()) { stopTicker(); return }
    const ms = deadline - Date.now()
    if (ms > 0) { autoApplySecondsLeft.set(Math.ceil(ms / 1000)); return }
    if (get(anyErrors)) { stopTicker(); return } // held by errors — re-arms on the next edit
    stopTicker()
    await runApply()
}

/** Persist all dirty sources through the shared in-flight guard.  Used by
 *  the auto-fire AND the toolbar's Apply button so they can't race. */
export async function runApply(): Promise<void> {
    if (get(applyInFlight)) return
    applyInFlight.set(true)
    autoApplyError.set('')
    try {
        await applyAll()
    } catch (e) {
        autoApplyError.set(String(e))
        throw e
    } finally {
        applyInFlight.set(false)
    }
}

/** "Hold" — cancel the current countdown.  It re-arms on the next edit. */
export function holdAutoApply(): void { stopTicker() }

let installed = false

/** Wire the controller to draft activity + dirty/enabled state.  Call once
 *  (toolbar onMount); returns the teardown. */
export function installAutoApply(): () => void {
    if (installed) return () => { /* already wired */ }
    installed = true

    // Per-edit tick: any draft mutation re-arms the timer.  Each .subscribe
    // fires once synchronously on attach — skip those priming calls.
    const drafts: Readable<unknown>[] = [engineDraft, gunfxDraft, lightfxDraft, landingDraft, gearDraft, deviceModel]
    let primed = false
    const unsubs = drafts.map(d => d.subscribe(() => { if (primed) arm() }))
    primed = true

    // Dirty rising edge arms (covers any source not in the draft list);
    // everything going clean (e.g. after a manual Apply) cancels at once.
    unsubs.push(anyDirty.subscribe(v => (v ? arm() : stopTicker())))
    // Toggling the feature off stops a running countdown immediately.
    unsubs.push(autoApplyEnabled.subscribe(v => (v ? arm() : stopTicker())))

    return () => { unsubs.forEach(u => u()); stopTicker(); installed = false }
}
