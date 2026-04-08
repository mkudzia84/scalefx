// ScaleFX Studio — Theme & View Settings
// Supports user-selectable theme (auto/dark/light/gray) and font size.
// Persisted in localStorage.

import { writable, derived } from 'svelte/store'

// ─── Types ───

export type ThemeChoice = 'auto' | 'dark' | 'light' | 'gray'
export type ResolvedTheme = 'dark' | 'light' | 'gray'

// ─── Persistence keys ───

const STORAGE_KEY_THEME = 'scalefx-theme'
const STORAGE_KEY_FONT  = 'scalefx-font-size'

// ─── Defaults ───

const DEFAULT_THEME: ThemeChoice = 'auto'
const DEFAULT_FONT_SIZE = 13
const MIN_FONT_SIZE = 11
const MAX_FONT_SIZE = 18

// ─── Load persisted values ───

function loadThemeChoice(): ThemeChoice {
    const stored = localStorage.getItem(STORAGE_KEY_THEME)
    if (stored === 'auto' || stored === 'dark' || stored === 'light' || stored === 'gray') {
        return stored
    }
    return DEFAULT_THEME
}

function loadFontSize(): number {
    const stored = localStorage.getItem(STORAGE_KEY_FONT)
    if (stored) {
        const num = parseInt(stored, 10)
        if (!isNaN(num) && num >= MIN_FONT_SIZE && num <= MAX_FONT_SIZE) return num
    }
    return DEFAULT_FONT_SIZE
}

// ─── OS theme detection ───

function getSystemTheme(): ResolvedTheme {
    if (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches) {
        return 'dark'
    }
    return 'light'
}

const systemTheme = writable<ResolvedTheme>(getSystemTheme())

if (window.matchMedia) {
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
        systemTheme.set(e.matches ? 'dark' : 'light')
    })
}

// ─── Stores ───

/** User's theme preference (persisted) */
export const themeChoice = writable<ThemeChoice>(loadThemeChoice())

/** Resolved theme applied to the document (derived from choice + OS) */
export const theme = derived(
    [themeChoice, systemTheme],
    ([$choice, $system]) => {
        if ($choice === 'auto') return $system
        return $choice as ResolvedTheme
    }
)

/** UI font size in px (persisted) */
export const fontSize = writable<number>(loadFontSize())

// ─── Persist on change ───

themeChoice.subscribe(v => localStorage.setItem(STORAGE_KEY_THEME, v))
fontSize.subscribe(v => localStorage.setItem(STORAGE_KEY_FONT, String(v)))

// ─── Export constants for dialog ───

export { MIN_FONT_SIZE, MAX_FONT_SIZE, DEFAULT_FONT_SIZE, DEFAULT_THEME }
