// Hardened drop-in replacement for `svelte/store` (aliased in vite.config.ts).
//
// WHY THIS EXISTS (2026-07-26 "UI freezes after connect" root cause):
// Svelte 3's store module keeps ONE module-level `subscriber_queue` shared by
// every store in the app.  Inside `set()`:
//
//     const run_queue = !subscriber_queue.length;
//     ...push subscribers...
//     if (run_queue) {
//         for (...) subscriber_queue[i][0](value);   // a subscriber THROWS here
//         subscriber_queue.length = 0;               // ...and this never runs
//     }
//
// After one subscriber callback throws mid-drain, the queue is left non-empty
// FOREVER.  Every later `set()` on ANY store then sees a non-empty queue,
// assumes an outer `set()` is still draining it, and silently never notifies
// anyone.  Event handlers keep firing and stores keep accepting values, but
// no component ever re-renders again — an app-wide, error-less, permanent
// freeze that the FE.FLUSH scheduler watchdog (diag.ts) cannot see, because
// no component is ever marked dirty.
//
// This module mirrors svelte 3.59.2's store semantics exactly, except:
//   1. the queue drain try/catches EVERY subscriber callback and clears the
//      queue in a `finally` — one bad subscriber can no longer mute the app;
//   2. every caught throw is reported (rate-limited) through a pluggable
//      sink — diag.ts wires it to the FE.STORE log tag so the culprit
//      subscriber names itself, stack and all, in scalefx-studio.log;
//   3. a store's `start` notifier and the initial `run(value)` on subscribe
//      are guarded the same way (a throw there would abort component mount).
//
// Consumers keep importing from 'svelte/store' — vite resolves that name to
// this file; TypeScript keeps type-checking against the real svelte types.

export type Subscriber<T> = (value: T) => void
export type Unsubscriber = () => void
export type Updater<T> = (value: T) => T
export type StartStopNotifier<T> = (set: Subscriber<T>) => Unsubscriber | void

export interface Readable<T> {
    subscribe(run: Subscriber<T>, invalidate?: (value?: T) => void): Unsubscriber
}
export interface Writable<T> extends Readable<T> {
    set(value: T): void
    update(updater: Updater<T>): void
}

const noop = () => {}

function safe_not_equal(a: unknown, b: unknown): boolean {
    // eslint-disable-next-line no-self-compare
    return a != a
        ? b == b
        : a !== b || (a && typeof a === 'object') || typeof a === 'function'
}

// ─── Error sink ───────────────────────────────────────────────────────
//
// console.error by default (which diag.ts also wraps → FE.CONSOLE); the
// diag bridge upgrades it to a direct FE.STORE report at install time.

type StoreErrorReporter = (e: unknown, phase: string) => void

let reportError: StoreErrorReporter = (e, phase) => {
    console.error(`[safestore] subscriber threw during ${phase}:`, e)
}

export function setStoreErrorReporter(fn: StoreErrorReporter): void {
    reportError = fn
}

// One report per unique error message per window — a throwing subscriber on
// a 10 Hz telemetry store must not flood the wire/log.
const recentReports = new Map<string, number>()
const REPORT_WINDOW_MS = 5000

function report(e: unknown, phase: string): void {
    try {
        // Key on message + top stack frame — two DIFFERENT subscribers
        // throwing the same message (e.g. ".some of null" in two deriveds)
        // must both surface, not dedup into one report.
        const frame = e instanceof Error && e.stack
            ? (e.stack.split('\n').find((l) => l.includes('    at ')) ?? '')
            : ''
        const key = `${phase}:${e instanceof Error ? e.message : String(e)}:${frame.trim()}`
        const now = Date.now()
        const last = recentReports.get(key) ?? 0
        if (now - last < REPORT_WINDOW_MS) return
        recentReports.set(key, now)
        if (recentReports.size > 64) recentReports.clear()
        reportError(e, phase)
    } catch { /* the guard itself must never throw */ }
}

// ─── Store implementation (svelte 3.59.2 semantics, hardened) ─────────

type SubscribeInvalidateTuple<T> = [Subscriber<T>, (value?: T) => void]

const subscriber_queue: Array<SubscribeInvalidateTuple<unknown> | unknown> = []

export function writable<T>(value?: T, start: StartStopNotifier<T> = noop): Writable<T> {
    let stop: Unsubscriber | null = null
    const subscribers = new Set<SubscribeInvalidateTuple<T>>()

    function set(new_value: T): void {
        if (safe_not_equal(value, new_value)) {
            value = new_value
            if (stop) { // store is ready
                const run_queue = !subscriber_queue.length
                for (const subscriber of subscribers) {
                    try { subscriber[1]() } catch (e) { report(e, 'invalidate') }
                    subscriber_queue.push(subscriber, value)
                }
                if (run_queue) {
                    try {
                        for (let i = 0; i < subscriber_queue.length; i += 2) {
                            const sub = subscriber_queue[i] as SubscribeInvalidateTuple<unknown>
                            try {
                                sub[0](subscriber_queue[i + 1])
                            } catch (e) {
                                report(e, 'notify')   // keep draining — never poison the queue
                            }
                        }
                    } finally {
                        subscriber_queue.length = 0
                    }
                }
            }
        }
    }

    function update(fn: Updater<T>): void {
        set(fn(value as T))
    }

    function subscribe(run: Subscriber<T>, invalidate: (value?: T) => void = noop): Unsubscriber {
        const subscriber: SubscribeInvalidateTuple<T> = [run, invalidate]
        subscribers.add(subscriber)
        if (subscribers.size === 1) {
            try {
                stop = start(set) || noop
            } catch (e) {
                report(e, 'start')
                stop = noop
            }
        }
        try {
            run(value as T)
        } catch (e) {
            report(e, 'first-run')
        }
        return () => {
            subscribers.delete(subscriber)
            if (subscribers.size === 0 && stop) {
                stop()
                stop = null
            }
        }
    }

    return { set, update, subscribe }
}

export function readable<T>(value?: T, start?: StartStopNotifier<T>): Readable<T> {
    return {
        subscribe: writable(value, start).subscribe,
    }
}

const is_function = (thing: unknown): thing is (...args: unknown[]) => unknown =>
    typeof thing === 'function'

/* eslint-disable @typescript-eslint/no-explicit-any */
export function derived(stores: any, fn: any, initial_value?: any): Readable<any> {
    const single = !Array.isArray(stores)
    const stores_array: Array<Readable<any>> = single ? [stores] : stores
    const auto = fn.length < 2
    return readable(initial_value, (set) => {
        let started = false
        const values: any[] = []
        let pending = 0
        let cleanup: Unsubscriber = noop
        const sync = () => {
            if (pending) return
            cleanup()
            // fn is user code — a throw here surfaces through the guarded
            // notify/start paths of the wrapping writable, but guard the
            // direct call too so `cleanup` stays consistent.
            let result: any
            try {
                result = fn(single ? values[0] : values, set)
            } catch (e) {
                report(e, 'derived')
                return
            }
            if (auto) {
                set(result)
            } else {
                cleanup = is_function(result) ? (result as Unsubscriber) : noop
            }
        }
        const unsubscribers = stores_array.map((store, i) =>
            store.subscribe(
                (value: any) => {
                    values[i] = value
                    pending &= ~(1 << i)
                    if (started) sync()
                },
                () => {
                    pending |= (1 << i)
                },
            ),
        )
        started = true
        sync()
        return function stop() {
            for (const u of unsubscribers) u()
            cleanup()
            // Callbacks queued before unsubscribe can still fire — same
            // guard svelte 3.59.2 carries.
            started = false
        }
    })
}
/* eslint-enable @typescript-eslint/no-explicit-any */

export function readonly<T>(store: Readable<T>): Readable<T> {
    return {
        subscribe: store.subscribe.bind(store),
    }
}

export function get<T>(store: Readable<T>): T {
    let value: T | undefined
    store.subscribe((v) => (value = v))()
    return value as T
}
