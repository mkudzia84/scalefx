import { describe, it, expect } from 'vitest'
import { seedChannelsForProgram, defaultTrack, defaultChannel, type ProgramT } from './lightfx'
import type { Port } from './devicemodel'

// seedChannelsForProgram is the "+ Template… on a fresh board" auto-wiring:
// track channel-names missing from the instance pool become new channels
// mapped onto the caller's unclaimed led-animator ports (in pool order).
// These tests pin: seeding + mapping + naming, existing-channel precedence,
// pool exhaustion (port-less channels, Rule 39), and the no-op cases.

function fakePort(idx: number, guid = ''): Port {
    // seedChannelsForProgram touches only ref.guid / kindName / ref.index —
    // the rest of the Port shape is irrelevant to it.
    return {
        ref: { guid, kind: 2, index: idx },
        kindName: 'pwm',
        direction: 'output',
    } as unknown as Port
}

function program(...channelNames: string[]): ProgramT {
    return {
        schemaVersion: 2,
        tracks: channelNames.map(n => defaultTrack(n)),
        landingBindings: [],
    }
}

describe('seedChannelsForProgram (template auto-mapping)', () => {
    it('seeds + maps + names every track channel on a fresh board', () => {
        const out = seedChannelsForProgram(
            [], program('Red beacon', 'White strobe', 'Position lights'),
            [fakePort(3), fakePort(1), fakePort(2)])
        expect(out.map(c => c.name)).toEqual(['Red beacon', 'White strobe', 'Position lights'])
        // Mapped in POOL order, not index order.
        expect(out.map(c => c.port?.idx)).toEqual([3, 1, 2])
        expect(out.every(c => c.port?.kind === 'pwm')).toBe(true)
    })

    it('leaves existing channels untouched and skips their names', () => {
        const existing = { ...defaultChannel('Red beacon'),
            port: { board: '', guid: '', kind: 'pwm', idx: 7 } }
        const out = seedChannelsForProgram(
            [existing], program('Red beacon', 'White tail'), [fakePort(0), fakePort(1)])
        expect(out).toHaveLength(2)
        expect(out[0]).toEqual(existing)               // untouched, not duplicated
        expect(out[1].name).toBe('White tail')
        expect(out[1].port?.idx).toBe(0)
    })

    it('never double-assigns a port already used by an existing channel', () => {
        const existing = { ...defaultChannel('Nav'),
            port: { board: '', guid: '', kind: 'pwm', idx: 0 } }
        // Stale pool that still contains port 0 — the defensive filter drops it.
        const out = seedChannelsForProgram(
            [existing], program('Beacon'), [fakePort(0), fakePort(1)])
        expect(out[1].port?.idx).toBe(1)
    })

    it('creates port-less channels when the pool runs dry (Rule 39 yellow)', () => {
        const out = seedChannelsForProgram(
            [], program('A', 'B', 'C'), [fakePort(0)])
        expect(out.map(c => c.port?.idx ?? null)).toEqual([0, null, null])
    })

    it('dedupes repeated track names and ignores blank ones', () => {
        const out = seedChannelsForProgram(
            [], program('Strobe', 'Strobe', '', '  '), [fakePort(0), fakePort(1)])
        expect(out).toHaveLength(1)
        expect(out[0].name).toBe('Strobe')
        expect(out[0].port?.idx).toBe(0)
    })

    it('is a no-op when every track channel already exists', () => {
        const chans = [defaultChannel('X')]
        const out = seedChannelsForProgram(chans, program('X'), [fakePort(0)])
        expect(out).toEqual(chans)
    })
})
