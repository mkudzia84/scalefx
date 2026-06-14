// wizard.ts — Setup Wizard stepper state (config-wizard, see
// instructions/33-CONFIG-WIZARD.md).
//
// The wizard is a guided front-end over the SAME draft stores the normal
// panels use (Rule 46): it walks features → input source → channel map →
// per-effect ports/params → review, writing the existing drafts, and the
// final step runs the global applyAll().  This file owns only the stepper
// navigation + the "offer on an empty config" trigger; the per-step UI lives
// in ConfigWizard.svelte and (later) per-step components.

import { writable, get } from 'svelte/store'
import { showConfigWizard, connectionInfo } from './stores'
import { deviceModel } from './devicemodel'
import { collectChannelOptions } from './channels'

export interface WizardStep {
    id: string
    title: string
    blurb: string
}

// Phase-1 static step list.  The per-effect steps become dynamic (one per
// enabled effect) once step 1 drives the set.
export const WIZARD_STEPS: WizardStep[] = [
    { id: 'features', title: 'Features',       blurb: 'Choose which effects you want to use and their sub-features.' },
    { id: 'input',    title: 'Input source',   blurb: 'Set the RC protocol (PPM / SBUS / Jeti) and how many channels.' },
    { id: 'channels', title: 'Channel map',    blurb: 'Map each channel to a function — we check it fits the features you picked.' },
    { id: 'effects',  title: 'Effects',        blurb: 'Assign ports (roles attach automatically) and set the key params with sensible defaults.' },
    { id: 'review',   title: 'Review & apply', blurb: 'Confirm the summary and apply everything to the board.' },
]

/** Zero-based index of the active step. */
export const wizardStep = writable(0)

export function openWizard(): void {
    wizardStep.set(0)
    showConfigWizard.set(true)
}
export function closeWizard(): void {
    showConfigWizard.set(false)
}
export function nextStep(): void {
    wizardStep.update(i => Math.min(i + 1, WIZARD_STEPS.length - 1))
}
export function prevStep(): void {
    wizardStep.update(i => Math.max(i - 1, 0))
}
export function gotoStep(i: number): void {
    if (i >= 0 && i < WIZARD_STEPS.length) wizardStep.set(i)
}
export function isFirstStep(i: number): boolean { return i <= 0 }
export function isLastStep(i: number): boolean { return i >= WIZARD_STEPS.length - 1 }

// ─── Auto-offer on an empty config ───────────────────────────────────
//
// On connect to a HubFX whose config looks "fresh" (no named RC inputs yet),
// offer the wizard once per app session.  Dismissible — opening the modal is
// just a suggestion, never forced.

/** A board with no named RC input channels reads as un-set-up (the typical
 *  intimidating-first-config case).  Heuristic for now; refined later to also
 *  weigh whether any effect is enabled. */
export function isConfigEmpty(dm = get(deviceModel)): boolean {
    return collectChannelOptions(dm).length === 0
}

let offeredThisSession = false

/** Subscribe to the connection so a fresh HubFX auto-offers the wizard once.
 *  Call once from App.svelte onMount. */
export function installWizardAutoOffer(): void {
    connectionInfo.subscribe(ci => {
        if (!ci.connected || ci.controllerType !== 'hubfx') return
        if (offeredThisSession) return
        // Defer so the on-connect config loads (App.svelte) have a chance to
        // populate the device model before we judge "empty".
        setTimeout(() => {
            if (offeredThisSession) return
            if (!get(connectionInfo).connected) return
            if (isConfigEmpty()) {
                offeredThisSession = true
                openWizard()
            }
        }, 2000)
    })
}
