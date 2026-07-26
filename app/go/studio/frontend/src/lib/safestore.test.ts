// Guards the safestore invariant that motivated the module (2026-07-26
// app-wide freeze): one throwing subscriber must never stop other
// subscribers from being notified, and must never poison the shared
// notify queue for LATER set() calls on OTHER stores.
//
// NOTE: `svelte/store` is vite-aliased to ./safestore — importing the
// public name here proves the alias wiring, not just the module.
import { get, writable, derived } from 'svelte/store'
import { setStoreErrorReporter } from './safestore'

describe('safestore (aliased svelte/store)', () => {
    let reported: string[]

    beforeEach(() => {
        reported = []
        setStoreErrorReporter((e, phase) => {
            reported.push(`${phase}: ${e instanceof Error ? e.message : String(e)}`)
        })
    })

    it('keeps notifying healthy subscribers after one throws', () => {
        const a = writable(0)
        const seen: number[] = []
        a.subscribe(() => { /* healthy #1 */ })
        a.subscribe((v) => { if (v > 0) throw new Error('bad subscriber') })
        a.subscribe((v) => seen.push(v))

        a.set(1)
        a.set(2)

        expect(seen).toEqual([0, 1, 2])
        expect(reported.some((r) => r.includes('bad subscriber'))).toBe(true)
    })

    it('does not poison OTHER stores after a subscriber throw (the freeze bug)', () => {
        const poison = writable(0)
        poison.subscribe((v) => { if (v > 0) throw new Error('poison') })
        poison.set(1)   // upstream svelte: leaves the shared queue non-empty forever

        const other = writable('idle')
        const seen: string[] = []
        other.subscribe((v) => seen.push(v))
        other.set('clicked')   // upstream svelte: silently never delivered

        expect(seen).toEqual(['idle', 'clicked'])
    })

    it('derived keeps tracking after its compute throws once', () => {
        const src = writable<{ items: number[] | null }>({ items: [] })
        const count = derived(src, ($s) => $s.items!.length)
        const seen: number[] = []
        count.subscribe((v) => seen.push(v))

        src.set({ items: null })   // compute throws (reported), value unchanged
        src.set({ items: [1, 2, 3] })

        expect(seen).toEqual([0, 3])
        expect(reported.some((r) => r.startsWith('derived'))).toBe(true)
        expect(get(count)).toBe(3)
    })

    it('a throwing initial run does not abort subscribe', () => {
        const a = writable(5)
        expect(() => a.subscribe(() => { throw new Error('mount throw') })).not.toThrow()
        expect(get(a)).toBe(5)
    })
})
