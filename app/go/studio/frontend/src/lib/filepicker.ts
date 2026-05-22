// ScaleFX Studio — file-picker glue.
//
// Reuses the existing FileManagerDialog as both a browser and a single-
// file picker.  `pickFile()` opens the dialog in `pick` mode and resolves
// the Promise with the chosen device path (e.g. "/sounds/x.wav"), or
// `null` if the user cancels.  The dialog's Select button calls
// resolveFilePick(path); closing the dialog calls resolveFilePick(null).

import { writable } from 'svelte/store'
import { showFileManager } from './stores'

export type FileManagerMode = 'browse' | 'pick'
export const fileManagerMode = writable<FileManagerMode>('browse')

let resolver: ((p: string | null) => void) | null = null

/** pickFile opens the file manager in single-file pick mode. */
export function pickFile(): Promise<string | null> {
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
    showFileManager.set(false)
}
