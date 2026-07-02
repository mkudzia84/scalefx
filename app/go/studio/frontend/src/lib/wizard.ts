// wizard.ts — Setup Wizard stepper state (config-wizard, see
// instructions/33-CONFIG-WIZARD.md).
//
// The wizard is a guided front-end over the SAME draft stores the normal
// panels use (Rule 46): it walks features → input source → channel map →
// per-effect ports/params → review, writing the existing drafts, and the
// final step runs the global applyAll().  This file owns only the stepper
// navigation + the "offer on an empty config" trigger; the per-step UI lives
// in ConfigWizard.svelte and (later) per-step components.

import { writable, derived, get } from 'svelte/store'
import { showConfigWizard, connectionInfo } from './stores'
import { diag } from './diag'
import { deviceModel } from './devicemodel'
import { collectChannelOptions } from './channels'
import { WIZARD_FEATURES } from './wizard-features'
import { engineDraft } from './effects'
import { gunfxDraft } from './gunfx'
import { lightfxDraft } from './lightfx'
import { landingDraft } from './landing'
import { gearDraft } from './gear'

export interface WizardStep {
    id: string
    title: string
    blurb: string
    feature?: string   // set on per-effect steps (the effect id)
}

const HEAD: WizardStep[] = [
    { id: 'features', title: 'Features',     blurb: 'Choose which effects you want to use.' },
    { id: 'input',    title: 'Input source', blurb: 'Set the RC protocol (PPM / SBUS / Jeti) and how many channels.' },
    { id: 'channels', title: 'Channel map',  blurb: 'Map each channel to a function — we check it fits the features you picked.' },
]
const REVIEW: WizardStep = { id: 'review', title: 'Review & apply', blurb: 'Confirm the summary and apply everything to the board.' }

/** The step list — DYNAMIC: one dedicated step per enabled effect, between the
 *  channel map and the review.  Recomputes as the operator toggles features. */
export const wizardSteps = derived(
    [engineDraft, gunfxDraft, lightfxDraft, landingDraft, gearDraft],
    (): WizardStep[] => {
        const fx = WIZARD_FEATURES.filter(f => f.isEnabled()).map(f => ({
            id: 'fx:' + f.id, title: f.label, blurb: f.blurb, feature: f.id,
        }))
        return [...HEAD, ...fx, REVIEW]
    },
)

/** Zero-based index of the active step. */
export const wizardStep = writable(0)

export function openWizard(): void {
    diag.info('WIZ', 'opening Setup Wizard')
    wizardStep.set(0)
    showConfigWizard.set(true)
}
export function closeWizard(): void {
    showConfigWizard.set(false)
}
export function nextStep(): void {
    wizardStep.update(i => Math.min(i + 1, get(wizardSteps).length - 1))
}
export function prevStep(): void {
    wizardStep.update(i => Math.max(i - 1, 0))
}
export function gotoStep(i: number): void {
    if (i >= 0 && i < get(wizardSteps).length) wizardStep.set(i)
}
export function isFirstStep(i: number): boolean { return i <= 0 }
export function isLastStep(i: number): boolean { return i >= get(wizardSteps).length - 1 }

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
                diag.info('WIZ', 'auto-offer: config empty — opening Setup Wizard')
                openWizard()
                diag.info('WIZ', 'auto-offer: wizard open returned')
            }
        }, 2000)
    })
}
