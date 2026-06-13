// sound_validation.ts — the ONE validator for effect sound-path fields.
//
// Engine (start/running/stop) and Gear (deploy/retract) both ran the same
// debounced check: a path must be absolute (start with '/'), a REQUIRED field
// must be set, and every set path must EXIST on the device's SD.  Hoisted
// 2026-06-13 so the rule lives once; panels keep only their tiny Svelte
// debounce wrapper (stateful, per-panel) and call this.

import { checkFiles } from './effects'

export interface SoundEntry {
    key:       string    // the panel's field key ('running', 'deploy', …)
    path:      string    // current path ('' = unset)
    required?: boolean   // unset → error (default optional)
}

/** Validate a set of sound-path fields.  Returns a `{ key → errorText }` map;
 *  '' / absent key = that field is valid.  Does ONE batched SD existence probe
 *  for all absolute paths. */
export async function validateSoundFiles(
    entries: readonly SoundEntry[],
): Promise<Record<string, string>> {
    const errs: Record<string, string> = {}
    for (const e of entries) {
        if (e.required && !e.path) errs[e.key] = 'required — pick a sound'
        else if (e.path && !e.path.startsWith('/')) errs[e.key] = 'path must be absolute (start with /)'
    }
    const probe = entries
        .filter(e => e.path && e.path.startsWith('/'))
        .map(e => e.path)
    if (probe.length > 0) {
        const exists = await checkFiles(probe)
        for (const e of entries) {
            if (errs[e.key]) continue
            if (e.path && e.path.startsWith('/') && !exists[e.path]) {
                errs[e.key] = `file not found on SD: ${e.path}`
            }
        }
    }
    return errs
}
