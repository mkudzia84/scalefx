import { describe, it, expect } from 'vitest'
import { boardLabel, claimsForPort, portRefKey, liveChannelKey, usToPct } from './devicemodel'
import type { Claim, PortRef } from './devicemodel'

describe('boardLabel (GUID redesign — hub is the empty guid)', () => {
    it('empty guid renders as "Hub"', () => {
        expect(boardLabel('')).toBe('Hub')
    })
    it('a real expander guid renders as itself', () => {
        expect(boardLabel('3C4D')).toBe('3C4D')
    })
})

describe('portRefKey / liveChannelKey are stable for hub-local ("")', () => {
    it('a hub-local PortRef keys with an empty guid prefix', () => {
        expect(portRefKey({ guid: '', kind: 1, index: 0 })).toBe('|1|0')
    })
    it('liveChannelKey matches between a hub port and its async frame', () => {
        // Both the device model and the input:values async use "" now, so the
        // live-bar key must match (the regression that left bars un-armed).
        const modelKey = liveChannelKey({ guid: '', kind: 4, index: 0 }, 2)
        const frameKey = liveChannelKey({ guid: '', kind: 4, index: 0 }, 2)
        expect(modelKey).toBe(frameKey)
        expect(modelKey).toBe('|0|2')
    })
})

describe('claimsForPort (hub-local match after the redesign)', () => {
    const claims: Claim[] = [
        { domain: 'gun', slot: 'yaw', port: { guid: '', kind: 1, index: 0 } },
        { domain: 'landing', slot: 'servo1', port: { guid: '3C4D', kind: 1, index: 0 } },
    ]
    it('a hub-local port ("") matches its hub-local claim', () => {
        const got = claimsForPort(claims, { guid: '', kind: 1, index: 0 } as PortRef)
        expect(got).toHaveLength(1)
        expect(got[0].domain).toBe('gun')
    })
    it('an expander port matches only its own claim (no hub cross-talk)', () => {
        const got = claimsForPort(claims, { guid: '3C4D', kind: 1, index: 0 } as PortRef)
        expect(got).toHaveLength(1)
        expect(got[0].domain).toBe('landing')
    })
    it('an unclaimed port returns no claims', () => {
        expect(claimsForPort(claims, { guid: '', kind: 1, index: 5 } as PortRef)).toHaveLength(0)
    })
})

describe('usToPct (RC bar mapping)', () => {
    it('maps the 1000–2000 µs window to 0–100% and clamps', () => {
        expect(usToPct(1000)).toBe(0)
        expect(usToPct(1500)).toBe(50)
        expect(usToPct(2000)).toBe(100)
        expect(usToPct(800)).toBe(0)
        expect(usToPct(2200)).toBe(100)
    })
})
