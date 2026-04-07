// ScaleFX Studio — OS Theme Detection
// Detects system dark/light mode and provides a reactive store.

import { writable } from 'svelte/store'

export type Theme = 'dark' | 'light'

function getSystemTheme(): Theme {
    if (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches) {
        return 'dark'
    }
    return 'light'
}

export const theme = writable<Theme>(getSystemTheme())

// Listen for OS theme changes
if (window.matchMedia) {
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
        theme.set(e.matches ? 'dark' : 'light')
    })
}
