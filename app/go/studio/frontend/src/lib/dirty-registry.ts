// dirty-registry — centralised dirty/validation tracking for Studio.
//
// Every tab that authors persistent state registers a `DirtySource`
// on mount and unregisters on destroy.  The global toolbar (rendered
// in App.svelte) reads aggregate state from the derived stores and
// runs `applyAll()` when the operator presses the unified Apply.
//
// Why a registry instead of hard-coding sources in App.svelte:
//   - Tabs come and go (collapsing panels, future feature toggles);
//     a registry decouples App.svelte from knowing which sources exist.
//   - Apply order matters (hubfx must commit BEFORE enginefx/gunfx
//     because the effect translators resolve named channels against
//     /hubfx.yaml's `inputs:` block) — registration order = apply
//     order.  Each panel registers in its natural lifecycle, the
//     registry preserves order.
//
// Validation:
//   - Each source exposes a `hasErrors` Readable.  The global Apply
//     is disabled whenever ANY source reports errors (Rule 35 + 45).
//   - Validation should happen on every data change inside the source;
//     this registry just reads the resulting boolean.

import { writable, derived, get, type Readable } from 'svelte/store'
import { onMount } from 'svelte'

export interface DirtySource {
    /** Stable identifier — shown in the global dirty-flag list and
     *  used for ordering hints (e.g. `hubconfig` always applies first). */
    id: string
    /** Human label for the dirty pill (e.g. "EngineFX", "GunFX"). */
    label: string
    /** True when the in-memory draft diverges from the device state. */
    isDirty: Readable<boolean>
    /** True when local validation has at least one error severity. */
    hasErrors: Readable<boolean>
    /** Persist the draft to the device.  Must throw on failure so the
     *  registry can stop the apply chain. */
    apply: () => Promise<void>
    /** Optional: re-fetch from the device.  Wired to the global Refresh
     *  button when present.  Multiple sources contribute. */
    refresh?: () => Promise<void>
}

// ─── Registry ────────────────────────────────────────────────────────

const sources = writable<DirtySource[]>([])

/** Register a dirty source.  Returns the unregister function — call it
 *  from the component's `onDestroy`. */
export function registerDirtySource(src: DirtySource): () => void {
    sources.update(list => {
        // Replace any existing entry with the same id (a re-mounted
        // panel shouldn't double-register).  This also lets a panel
        // tweak its source mid-life by re-registering with new stores.
        const filtered = list.filter(s => s.id !== src.id)
        return [...filtered, src]
    })
    return () => {
        sources.update(list => list.filter(s => s.id !== src.id))
    }
}

// ─── Aggregate state ─────────────────────────────────────────────────

/** True when ANY registered source is dirty. */
export const anyDirty: Readable<boolean> = derived(sources, (list, set) => {
    // Re-subscribe to each source's isDirty store; recompute on any change.
    const unsubs = list.map(s => s.isDirty.subscribe(() => {
        set(list.some(t => get(t.isDirty)))
    }))
    set(list.some(s => get(s.isDirty)))
    return () => unsubs.forEach(u => u())
}, false)

/** True when ANY registered source has validation errors.  Used to
 *  gate the global Apply per Rule 35 — Apply is disabled if anyone
 *  has unresolved errors. */
export const anyErrors: Readable<boolean> = derived(sources, (list, set) => {
    const unsubs = list.map(s => s.hasErrors.subscribe(() => {
        set(list.some(t => get(t.hasErrors)))
    }))
    set(list.some(s => get(s.hasErrors)))
    return () => unsubs.forEach(u => u())
}, false)

/** Human labels of currently-dirty sources — feeds the global dirty
 *  pill ("dirty: hubconfig, enginefx").  Empty array when in sync. */
export const dirtyLabels: Readable<string[]> = derived(sources, (list, set) => {
    const unsubs = list.map(s => s.isDirty.subscribe(() => {
        set(list.filter(t => get(t.isDirty)).map(t => t.label))
    }))
    set(list.filter(s => get(s.isDirty)).map(s => s.label))
    return () => unsubs.forEach(u => u())
}, [] as string[])

/** Labels of sources with errors — feeds the validation warning. */
export const errorLabels: Readable<string[]> = derived(sources, (list, set) => {
    const unsubs = list.map(s => s.hasErrors.subscribe(() => {
        set(list.filter(t => get(t.hasErrors)).map(t => t.label))
    }))
    set(list.filter(s => get(s.hasErrors)).map(s => s.label))
    return () => unsubs.forEach(u => u())
}, [] as string[])

// ─── Bulk operations ─────────────────────────────────────────────────

/** Apply every dirty source in REGISTRATION ORDER.  Stops at the
 *  first failure — the caller surfaces the error and the operator
 *  fixes it before pressing Apply again.  Idempotent on clean sources
 *  (skipped silently). */
export async function applyAll(): Promise<void> {
    const list = get(sources)
    for (const src of list) {
        if (!get(src.isDirty)) continue
        if (get(src.hasErrors)) {
            throw new Error(`${src.label}: validation errors must be resolved first`)
        }
        await src.apply()
    }
}

/** Re-fetch every registered source.  Errors collected and re-thrown
 *  at the end (refresh failure shouldn't strand other sources). */
export async function refreshAll(): Promise<void> {
    const list = get(sources)
    const errors: string[] = []
    for (const src of list) {
        if (!src.refresh) continue
        try { await src.refresh() } catch (e) {
            errors.push(`${src.label}: ${String(e)}`)
        }
    }
    if (errors.length) throw new Error(errors.join('; '))
}

// ─── Component-lifecycle convenience ─────────────────────────────────

/** `useConfigSource(src)` — call from a Svelte component's `<script>`
 *  initialisation to register the source on mount and unregister on
 *  destroy.  One-liner replacement for the explicit `onMount(() =>
 *  registerDirtySource(...))` boilerplate.
 *
 *  Most domains pre-register at App.svelte startup so apply order is
 *  deterministic (independent of which tab the operator opens first).
 *  This helper is for ad-hoc / per-tab sources that don't need
 *  ordering — e.g. a future "experimental" panel.
 */
export function useConfigSource(src: DirtySource): void {
    onMount(() => registerDirtySource(src))
}
