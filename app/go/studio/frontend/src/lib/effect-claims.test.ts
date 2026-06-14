import { describe, it, expect, beforeEach } from 'vitest'
import { get } from 'svelte/store'
import { effectClaims } from './effect-claims'
import { gunfxDraft, defaultGun } from './gunfx'
import { gearDraft, defaultGearChannel } from './gear'
import { lightfxDraft } from './lightfx'
import { landingDraft } from './landing'
import { PortKind, type Claim } from './devicemodel'

// effectClaims is now the SOLE port-ownership source (the hard-claim path —
// deviceModel.claims / ApplyDefaults — was retired in the 2026-06 cleanup).  It
// derives claims purely from the effect drafts, so these tests pin that synthesis
// (and guard against the hard-claim path sneaking back in).

beforeEach(() => {
    gunfxDraft.update(c => ({ ...c, enabled: false, guns: [] }))
    gearDraft.update(c => ({ ...c, enabled: false, gears: [] }))
    lightfxDraft.update(c => ({ ...c, enabled: false, channels: [] }))
    landingDraft.update(c => ({ ...c, lights: [] }))
})

const has = (claims: Claim[], domain: string, kindByte: number, idx: number) =>
    claims.some(c => c.domain === domain && c.port.kind === kindByte && c.port.index === idx)

describe('effectClaims (soft-only synthesis)', () => {
    it('is empty when no effect is configured', () => {
        expect(get(effectClaims)).toEqual([])
    })

    it('synthesizes a claim for an enabled gun muzzle port', () => {
        const gun = defaultGun(0)
        gun.muzzleFlash.port = { board: '', guid: '', kind: 'pwm', idx: 5 }
        gunfxDraft.update(c => ({ ...c, enabled: true, guns: [gun] }))
        expect(has(get(effectClaims), 'gunfx', PortKind.Pwm, 5)).toBe(true)
    })

    it('a DISABLED effect contributes no claims (ports return to the pool)', () => {
        const gun = defaultGun(0)
        gun.muzzleFlash.port = { board: '', guid: '', kind: 'pwm', idx: 5 }
        gunfxDraft.update(c => ({ ...c, enabled: false, guns: [gun] }))
        expect(get(effectClaims).filter(c => c.domain === 'gunfx')).toEqual([])
    })

    it('an unset port (empty kind) does not claim', () => {
        const gun = defaultGun(0) // muzzle defaults to an empty ref (kind: '')
        gunfxDraft.update(c => ({ ...c, enabled: true, guns: [gun] }))
        // The default muzzle ref is unset, so no muzzle claim is synthesized.
        expect(get(effectClaims).some(c => c.slot.includes('muzzle'))).toBe(false)
    })

    it('synthesizes a gear motor claim with the right port kind', () => {
        const g = defaultGearChannel(0)
        g.motor = { board: '', guid: '', kind: 'hbridge', idx: 0 }
        gearDraft.update(c => ({ ...c, enabled: true, gears: [g] }))
        expect(has(get(effectClaims), 'gear', PortKind.HBridge, 0)).toBe(true)
    })

    it('recomputes reactively when an effect is toggled off', () => {
        const gun = defaultGun(0)
        gun.muzzleFlash.port = { board: '', guid: '', kind: 'pwm', idx: 2 }
        gunfxDraft.update(c => ({ ...c, enabled: true, guns: [gun] }))
        expect(has(get(effectClaims), 'gunfx', PortKind.Pwm, 2)).toBe(true)
        gunfxDraft.update(c => ({ ...c, enabled: false }))
        expect(has(get(effectClaims), 'gunfx', PortKind.Pwm, 2)).toBe(false)
    })
})
