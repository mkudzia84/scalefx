// ScaleFX Studio — file-picker glue.
//
// Reuses the existing FileManagerDialog as both a browser and a single-
// file picker.  `pickFile()` opens the dialog in `pick` mode and resolves
// the Promise with the chosen device path (e.g. "/sounds/x.wav"), or
// `null` if the user cancels.  The dialog's Select button calls
// resolveFilePick(path); closing the dialog calls resolveFilePick(null).
//
// The picker can be constrained to a single backend via the `targets`
// option — sound-file pickers in the EngineFX panel pass `'sd'` so the
// Flash tab is hidden and the dialog opens straight onto SD.

import { writable } from 'svelte/store'
import { showFileManager } from './stores'

export type FileManagerMode = 'browse' | 'pick'
export const fileManagerMode = writable<FileManagerMode>('browse')

/** Which storage backends the picker is allowed to browse. 'both' is the
 *  default and lets the operator switch between Flash and SD. */
export type PickerTargets = 'flash' | 'sd' | 'both'
export const fileManagerTargets = writable<PickerTargets>('both')

let resolver: ((p: string | null) => void) | null = null

export interface PickFileOptions {
    /** Restrict the picker to one backend. Default 'both'. */
    targets?: PickerTargets
}

/** pickFile opens the file manager in single-file pick mode. */
export function pickFile(opts: PickFileOptions = {}): Promise<string | null> {
    fileManagerTargets.set(opts.targets ?? 'both')
    fileManagerMode.set('pick')
    showFileManager.set(true)
    return new Promise(resolve => { resolver = resolve })
}

/** Called from FileManagerDialog when the user picks a file or cancels. */
export function resolveFilePick(path: string | null) {
    if (resolver) {
        const r = resolver
        resolver = null
        r(path)
    }
    fileManagerMode.set('browse')
    fileManagerTargets.set('both')
    showFileManager.set(false)
}
