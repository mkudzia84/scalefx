// Shared debounced live-push helper (Rule 24).
//
// Board tabs must validate edits locally, debounce pushes ~350ms after the
// last change, dedupe against the last successfully sent command, and surface
// a status badge (pending / sent / invalid). This module implements the
// plumbing once; tabs supply only the build()/send glue.
//
// Reference pattern: see ServoWidget.svelte (single-key inline variant) and
// GearControlTab.svelte (multi-key consumer of createLivePusher).

import { writable, type Writable } from 'svelte/store'

export type PushStatus = '' | 'pending' | 'sent' | 'invalid'
export const PUSH_DEBOUNCE_MS = 350

export interface LivePusher {
    /** Reactive status-per-key map. Subscribe in templates via $status[key]. */
    status: Writable<Record<string, PushStatus>>

    /** Debounced live-push. `build()` returns a command string to send,
     *  `''` for "valid but unchanged from last push" (leaves status as-is),
     *  or `null` for "invalid — show badge and skip the push". */
    schedule(key: string, build: () => string | null): void

    /** True while a debounced push is pending for this key. Call from code
     *  that should yield to in-flight edits (e.g. don't overwrite user edits
     *  with a broadcast update until their push lands). */
    isPending(key: string): boolean

    /** Re-arm dedup so the next edit always pushes. Used when the underlying
     *  resource changes identity (e.g. a door servo channel reassigned). */
    resetBaseline(key: string): void

    /** Clear all pending timers. Call from onDestroy(). */
    destroy(): void
}

/** Create a per-component live-pusher bound to a specific command sender.
 *  Typical use:
 *
 *      const live = createLivePusher(SendCommand)
 *      const pushStatus = live.status
 *      onDestroy(() => live.destroy())
 */
export function createLivePusher(
    send: (cmd: string) => void,
    debounceMs: number = PUSH_DEBOUNCE_MS,
): LivePusher {
    const timers: Record<string, ReturnType<typeof setTimeout> | null> = {}
    const lastPushed: Record<string, string> = {}
    const status = writable<Record<string, PushStatus>>({})

    function setStatus(key: string, s: PushStatus): void {
        status.update(st => {
            if (st[key] === s) return st
            return { ...st, [key]: s }
        })
    }

    return {
        status,

        schedule(key, build) {
            const cmd = build()
            if (cmd === null) { setStatus(key, 'invalid'); return }
            if (cmd === '' || cmd === lastPushed[key]) return
            setStatus(key, 'pending')
            const t = timers[key]
            if (t) clearTimeout(t)
            timers[key] = setTimeout(() => {
                timers[key] = null
                const fresh = build()
                if (fresh === null) { setStatus(key, 'invalid'); return }
                if (fresh === '' || fresh === lastPushed[key]) { setStatus(key, 'sent'); return }
                send(fresh)
                lastPushed[key] = fresh
                setStatus(key, 'sent')
            }, debounceMs)
        },

        isPending(key) {
            return timers[key] != null
        },

        resetBaseline(key) {
            const t = timers[key]
            if (t) { clearTimeout(t); timers[key] = null }
            delete lastPushed[key]
            setStatus(key, '')
        },

        destroy() {
            for (const k of Object.keys(timers)) {
                const t = timers[k]
                if (t) clearTimeout(t)
                timers[k] = null
            }
        },
    }
}

/** Badge-text helper matching the CSS states in GearControlTab / ServoWidget. */
export function pushBadgeText(s: PushStatus | undefined): string {
    switch (s) {
        case 'pending': return '● syncing…'
        case 'sent':    return '✓ synced'
        case 'invalid': return '⚠ invalid'
        default:        return ''
    }
}
