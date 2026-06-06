import { describe, it, expect } from 'vitest'
import { portRefToKey, modelPortKey, parsePortKey } from './port_keys'
import type { Port } from '../devicemodel'

describe('portRefToKey', () => {
    it('keys a HUB-LOCAL ref (empty guid) — the regression that blanked dropdowns', () => {
        // The old `!r.guid` guard returned '' here → the <select> showed blank
        // for every loaded hub port. Empty guid is the canonical hub form.
        expect(portRefToKey({ board: '', guid: '', kind: 'servo', idx: 0 })).toBe('|servo|0')
    })

    it('keys an expander ref (real guid)', () => {
        expect(portRefToKey({ board: '', guid: '3C4D', kind: 'pwm', idx: 2 })).toBe('3C4D|pwm|2')
    })

    it('returns "" for an unset / kind-less ref', () => {
        expect(portRefToKey(null)).toBe('')
        expect(portRefToKey(undefined)).toBe('')
        expect(portRefToKey({ board: '', guid: '', kind: '', idx: 0 })).toBe('')
    })
})

describe('modelPortKey matches portRefToKey (option ⇄ value)', () => {
    function mkPort(guid: string, kindName: string, idx: number): Port {
        return { ref: { guid, kind: 1, index: idx }, kindName } as unknown as Port
    }
    it('a hub port option key equals the picked-ref value key', () => {
        const p = mkPort('', 'servo', 0)
        const refFromPick = parsePortKey(modelPortKey(p), 'servo')
        // The round-trip a panel does: option key → ref → value key.
        expect(portRefToKey(refFromPick)).toBe(modelPortKey(p))
        expect(modelPortKey(p)).toBe('|servo|0')
    })
    it('an expander port round-trips too', () => {
        const p = mkPort('3C4D', 'pwm', 3)
        const ref = parsePortKey(modelPortKey(p), 'pwm')
        expect(portRefToKey(ref)).toBe(modelPortKey(p))
    })
})

describe('parsePortKey', () => {
    it('parses a hub key (empty guid prefix)', () => {
        expect(parsePortKey('|servo|0', 'servo')).toEqual({ board: '', guid: '', kind: 'servo', idx: 0 })
    })
    it('parses an expander key', () => {
        expect(parsePortKey('3C4D|pwm|2', 'pwm')).toEqual({ board: '', guid: '3C4D', kind: 'pwm', idx: 2 })
    })
    it('empty key → unset ref with the fallback kind', () => {
        expect(parsePortKey('', 'servo')).toEqual({ board: '', guid: '', kind: 'servo', idx: 0 })
    })
})
