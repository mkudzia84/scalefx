// wizard-features.ts — the effect registry the Setup Wizard drives.
//
// One WizardFeature per top-level effect.  Each knows how to read/seed its
// existing draft store (so the wizard reuses the SAME config the panels edit,
// Rule 46), which RC channel function(s) it expects (Rule 43 named inputs —
// canonical ids from the Go channel-function catalog), and which tab to
// deep-link to for fine-tuning.  No new config surface — just a guided view.

import { get } from 'svelte/store'
import type { DeviceModelSnapshotT } from './devicemodel'
import type { TabKind } from './devicemodel'
import { engineDraft } from './effects'
import { gunfxDraft, setEnabled as setGunEnabled, addGun } from './gunfx'
import { lightfxDraft, setEnabled as setLightEnabled } from './lightfx'
import { landingDraft, addLandingLight } from './landing'
import { gearDraft, setGearEnabled } from './gear'

/** An RC channel function an effect wants mapped (canonical id from the Go
 *  catalog in devicemodel/input.go). */
export interface ExpectedInput { fn: string; label: string; required: boolean }

export interface WizardFeature {
    id: string
    label: string
    icon: string
    blurb: string
    /** Domain ids that, when advertised by the hub, make this feature available
     *  (capability gating — matches deviceModel.domains). */
    domainIds: string[]
    tabKind: TabKind
    /** RC channels this feature binds (for the fitness check + the effect step). */
    inputs: ExpectedInput[]
    isEnabled(): boolean
    setEnabled(on: boolean): void
    /** Read/write the primary RC input's channel-function id on the draft. */
    getInput(): string
    setInput(fn: string): void
}

export const WIZARD_FEATURES: WizardFeature[] = [
    {
        id: 'engine', label: 'Engine FX', icon: '🛩', tabKind: 'engine',
        blurb: 'Turbine / radial / diesel start-up + running sound, RC on/off.',
        domainIds: ['engine'],
        inputs: [{ fn: 'engine_onoff', label: 'Engine on/off', required: true }],
        isEnabled: () => !!get(engineDraft).enabled,
        setEnabled: (on) => engineDraft.update(c => ({ ...c, enabled: on })),
        getInput: () => get(engineDraft).toggle?.input ?? '',
        setInput: (fn) => engineDraft.update(c => ({ ...c, toggle: { ...c.toggle, input: fn } })),
    },
    {
        id: 'gun', label: 'Gun FX', icon: '🔫', tabKind: 'gun',
        blurb: 'Rate-of-fire firing, muzzle flash, recoil, smoke, turret.',
        domainIds: ['gun'],
        inputs: [
            { fn: 'gun_fire',      label: 'Gun fire (trigger)', required: true },
            { fn: 'gun_fire_mode', label: 'Fire mode (ROF)',    required: false },
            { fn: 'gun_smoke',     label: 'Smoke on/off',       required: false },
        ],
        isEnabled: () => !!get(gunfxDraft).enabled,
        setEnabled: (on) => {
            setGunEnabled(on)
            if (on && get(gunfxDraft).guns.length === 0) addGun()
        },
        getInput: () => get(gunfxDraft).guns[0]?.trigger?.input ?? '',
        setInput: (fn) => gunfxDraft.update(c => {
            const guns = c.guns.length ? c.guns : c.guns
            if (guns[0]) guns[0] = { ...guns[0], trigger: { ...guns[0].trigger, input: fn } }
            return { ...c, guns: [...guns] }
        }),
    },
    {
        id: 'lighting', label: 'Lighting', icon: '💡', tabKind: 'lighting',
        blurb: 'LED light programs (strobes, beacons, nav) + an RC program selector.',
        domainIds: ['lighting', 'landing-lights'],
        inputs: [{ fn: 'light_program', label: 'Light program select', required: false }],
        isEnabled: () => !!get(lightfxDraft).enabled,
        setEnabled: (on) => setLightEnabled(on),
        getInput: () => get(lightfxDraft).programSelector?.input ?? '',
        setInput: (fn) => lightfxDraft.update(c => ({
            ...c, programSelector: { ...c.programSelector, input: fn, enabled: fn !== '' },
        })),
    },
    {
        id: 'landing', label: 'Landing lights', icon: '🔦', tabKind: 'lighting',
        blurb: 'Servo-deployed LED searchlights (retractable), RC deploy.',
        domainIds: ['lighting', 'landing-lights'],
        inputs: [{ fn: 'landing_lights', label: 'Landing lights on/off', required: false }],
        isEnabled: () => get(landingDraft).lights.length > 0,
        setEnabled: (on) => {
            if (on) { if (get(landingDraft).lights.length === 0) addLandingLight() }
            else landingDraft.update(c => ({ ...c, lights: [] }))
        },
        getInput: () => get(landingDraft).lights[0]?.activation?.input ?? '',
        setInput: (fn) => landingDraft.update(c => {
            if (c.lights[0]) {
                const lights = [...c.lights]
                lights[0] = { ...lights[0], activation: { ...lights[0].activation, mode: fn ? 'input_channel' : lights[0].activation.mode, input: fn } }
                return { ...c, lights }
            }
            return c
        }),
    },
    {
        id: 'gear', label: 'Undercarriage', icon: '🛬', tabKind: 'gear',
        blurb: 'Retractable landing gear — motors, doors, RC up/down.',
        domainIds: ['gearcontrol', 'landing-gear'],
        inputs: [{ fn: 'landing_gear', label: 'Gear up/down', required: true }],
        isEnabled: () => !!get(gearDraft).enabled,
        setEnabled: (on) => setGearEnabled(on),
        getInput: () => get(gearDraft).input?.name ?? '',
        setInput: (fn) => gearDraft.update(c => ({ ...c, input: { ...c.input, name: fn } })),
    },
]

/** Features available on the connected board (capability-gated by the hub's
 *  advertised domains; pre-connect shows all so the operator can review). */
export function availableFeatures(dm: DeviceModelSnapshotT): WizardFeature[] {
    const ids = new Set((dm.domains ?? []).map(d => d.id))
    if (ids.size === 0) return WIZARD_FEATURES   // pre-connect / no domains yet
    return WIZARD_FEATURES.filter(f => f.domainIds.some(d => ids.has(d)))
}

export function featureById(id: string): WizardFeature | undefined {
    return WIZARD_FEATURES.find(f => f.id === id)
}
