// ScaleFX Studio — Shared Light Data
// Event type definitions, presets, and preset groups for LED channel/sequence editing.
// Used by LightFxTab (8ch standalone) and LightSetup (6ch HubFX embedded).

// ─── Interfaces ───

export interface ParamDef {
    label: string
    key: string
    min: number
    max: number
    step: number
    defaultVal: number
    unit?: string
}

export interface EventTypeDef {
    name: string
    value: string
    params: ParamDef[]
}

export interface LightPreset {
    name: string
    group: string
    events: { type: string; params: Record<string, number> }[]
}

// ─── Event Type Definitions ───

export const eventTypes: EventTypeDef[] = [
    { name: 'ON', value: 'on', params: [
        { label: 'Duration', key: 'duration', min: 0, max: 60000, step: 10, defaultVal: 0, unit: 'ms' },
        { label: 'Brightness', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
        { label: 'Power Save', key: 'powerSave', min: 0, max: 1, step: 1, defaultVal: 0, unit: '' },
        { label: 'PWM Duty', key: 'pwmDuty', min: 1, max: 100, step: 1, defaultVal: 78, unit: '%' },
    ]},
    { name: 'OFF', value: 'off', params: [
        { label: 'Duration', key: 'duration', min: 0, max: 60000, step: 10, defaultVal: 0, unit: 'ms' },
    ]},
    { name: 'FLASH', value: 'flash', params: [
        { label: 'Interval', key: 'interval', min: 10, max: 10000, step: 10, defaultVal: 100, unit: 'ms' },
        { label: 'Duration', key: 'duration', min: 0, max: 60000, step: 10, defaultVal: 0, unit: 'ms' },
        { label: 'Brightness', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
        { label: 'Duty', key: 'duty', min: 1, max: 100, step: 1, defaultVal: 50, unit: '%' },
    ]},
    { name: 'FADE IN', value: 'fadein', params: [
        { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 10, defaultVal: 1000, unit: 'ms' },
        { label: 'Target', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
    ]},
    { name: 'FADE OUT', value: 'fadeout', params: [
        { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 10, defaultVal: 1000, unit: 'ms' },
        { label: 'Start', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
    ]},
    { name: 'FADING', value: 'fading', params: [
        { label: 'Cycle', key: 'cycle', min: 50, max: 10000, step: 50, defaultVal: 2000, unit: 'ms' },
        { label: 'Duration', key: 'duration', min: 0, max: 60000, step: 100, defaultVal: 0, unit: 'ms' },
        { label: 'Min', key: 'min', min: 0, max: 100, step: 1, defaultVal: 0, unit: '%' },
        { label: 'Max', key: 'max', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
    ]},
    { name: 'BEACON', value: 'beacon', params: [
        { label: 'Cycle', key: 'cycle', min: 100, max: 10000, step: 50, defaultVal: 1200, unit: 'ms' },
        { label: 'Duration', key: 'duration', min: 0, max: 60000, step: 100, defaultVal: 0, unit: 'ms' },
        { label: 'Flash %', key: 'flashPct', min: 1, max: 50, step: 1, defaultVal: 15, unit: '%' },
        { label: 'Min', key: 'min', min: 0, max: 100, step: 1, defaultVal: 0, unit: '%' },
        { label: 'Peak', key: 'max', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
    ]},
]

// ─── Preset Groups ───

export const presetGroups = ['', 'Aircraft', 'Vehicle', 'Naval', 'Effects'] as const

// ─── Presets ───
// Timing reference: LedFlashing full cycle = interval × 2, on-time = cycle × duty / 100
// FAA 14 CFR §25.1401: anti-collision 40-100 flashes/min (600-1500ms cycle)
// SAE J590: turn signals 60-120 flashes/min (500-1000ms cycle, typical ~700ms)

export const presets: LightPreset[] = [
    { name: 'Custom', group: '', events: [] },

    // ── Aircraft ─────────────────────────────────────────────────
    // Navigation (steady position lights — same for planes & helicopters)
    { name: 'Nav Position', group: 'Aircraft', events: [
        // Steady red (port), green (starboard), or white (tail) — always on
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Nav Dimmed', group: 'Aircraft', events: [
        // Reduced brightness for night / NVG operations
        { type: 'on', params: { duration: 0, brightness: 40, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Nav Flashing', group: 'Aircraft', events: [
        // Military — position lights in flash mode (~60/min)
        { type: 'flash', params: { interval: 500, duration: 0, brightness: 100, duty: 50 } },
    ]},
    // Anti-collision beacon (red rotating, ~46/min — FAA 40-100/min)
    { name: 'Anti-Col Beacon', group: 'Aircraft', events: [
        { type: 'beacon', params: { cycle: 1300, duration: 0, flashPct: 12, min: 0, max: 100 } },
    ]},
    // Rotating beacon — wider beam sweep (30% of cycle illuminated, ~46/min)
    // Simulates the visible light sweep of a rotating beacon on fuselage/belly
    { name: 'Beacon Rotating', group: 'Aircraft', events: [
        { type: 'beacon', params: { cycle: 1300, duration: 0, flashPct: 30, min: 0, max: 100 } },
    ]},
    // White strobe — single flash ~46/min (650×2=1300ms, 52ms on)
    { name: 'Strobe Single', group: 'Aircraft', events: [
        { type: 'flash', params: { interval: 650, duration: 0, brightness: 100, duty: 4 } },
    ]},
    // White strobe — double flash ~46/min (two quick pulses per 1300ms cycle)
    { name: 'Strobe Double', group: 'Aircraft', events: [
        { type: 'on', params:  { duration: 50, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 100 } },
        { type: 'on', params:  { duration: 50, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 1100 } },
    ]},
    // White strobe — triple flash ~46/min (three quick pulses per 1300ms cycle)
    { name: 'Strobe Triple', group: 'Aircraft', events: [
        { type: 'on', params:  { duration: 40, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 60 } },
        { type: 'on', params:  { duration: 40, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 60 } },
        { type: 'on', params:  { duration: 40, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 1060 } },
    ]},
    // Steady utility lights
    { name: 'Landing Light', group: 'Aircraft', events: [
        { type: 'fadein', params: { duration: 400, brightness: 100 } },
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Taxi / Hover', group: 'Aircraft', events: [
        { type: 'on', params: { duration: 0, brightness: 80, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Formation', group: 'Aircraft', events: [
        // Military — very dim for low-vis / IR
        { type: 'on', params: { duration: 0, brightness: 15, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Logo / Inspection', group: 'Aircraft', events: [
        // Tail logo or wing/engine inspection
        { type: 'on', params: { duration: 0, brightness: 60, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Cabin', group: 'Aircraft', events: [
        { type: 'on', params: { duration: 0, brightness: 35, powerSave: 0, pwmDuty: 78 } },
    ]},
    // ── Sequenced set (assign each to a separate channel for phased flash) ──
    // All three share a 1500ms cycle (~40/min). Offsets prevent simultaneous flash.
    { name: 'Seq: Beacon (1/3)', group: 'Aircraft', events: [
        // Phase 1 — rotating beacon at start of cycle (0ms offset)
        { type: 'beacon', params: { cycle: 1500, duration: 0, flashPct: 15, min: 0, max: 100 } },
    ]},
    { name: 'Seq: Strobe (2/3)', group: 'Aircraft', events: [
        // Phase 2 — strobe flash at 500ms offset into 1500ms cycle
        { type: 'off', params: { duration: 500 } },
        { type: 'on', params:  { duration: 50, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 950 } },
    ]},
    { name: 'Seq: Nav (3/3)', group: 'Aircraft', events: [
        // Phase 3 — nav position blink at 1000ms offset into 1500ms cycle
        { type: 'off', params: { duration: 1000 } },
        { type: 'on', params:  { duration: 80, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 420 } },
    ]},

    // ── Vehicle ──────────────────────────────────────────────────
    { name: 'Headlight Lo', group: 'Vehicle', events: [
        { type: 'on', params: { duration: 0, brightness: 70, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Headlight Hi', group: 'Vehicle', events: [
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Tail Light', group: 'Vehicle', events: [
        { type: 'on', params: { duration: 0, brightness: 30, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Brake Light', group: 'Vehicle', events: [
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Turn Signal', group: 'Vehicle', events: [
        // ~86/min per SAE J590 (60-120/min)
        { type: 'flash', params: { interval: 350, duration: 0, brightness: 100, duty: 50 } },
    ]},
    { name: 'DRL', group: 'Vehicle', events: [
        { type: 'on', params: { duration: 0, brightness: 55, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Fog Light', group: 'Vehicle', events: [
        { type: 'on', params: { duration: 0, brightness: 80, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Reverse', group: 'Vehicle', events: [
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Emergency Strobe', group: 'Vehicle', events: [
        // Fast alternating (~150/min typical LED lightbar)
        { type: 'flash', params: { interval: 100, duration: 0, brightness: 100, duty: 50 } },
    ]},
    { name: 'Emergency Wig-Wag', group: 'Vehicle', events: [
        // Alternating headlights (~120/min)
        { type: 'on', params:  { duration: 250, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 250 } },
    ]},

    // ── Naval ────────────────────────────────────────────────────
    { name: 'Masthead', group: 'Naval', events: [
        // White 225° forward arc
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Sidelight', group: 'Naval', events: [
        // Port red or starboard green (112.5° arc)
        { type: 'on', params: { duration: 0, brightness: 80, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Stern', group: 'Naval', events: [
        // White 135° aft arc
        { type: 'on', params: { duration: 0, brightness: 70, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Anchor', group: 'Naval', events: [
        // White all-round (at anchor)
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 1, pwmDuty: 78 } },
    ]},
    { name: 'Deck Work', group: 'Naval', events: [
        { type: 'on', params: { duration: 0, brightness: 100, powerSave: 0, pwmDuty: 78 } },
    ]},
    { name: 'Signal Flash', group: 'Naval', events: [
        // Three-pulse signal pattern
        { type: 'on', params:  { duration: 150, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 150 } },
        { type: 'on', params:  { duration: 150, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 150 } },
        { type: 'on', params:  { duration: 150, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 1500 } },
    ]},

    // ── Effects ──────────────────────────────────────────────────
    { name: 'Rotating Light', group: 'Effects', events: [
        // General-purpose rotating beacon — smooth cosine sweep
        { type: 'beacon', params: { cycle: 2000, duration: 0, flashPct: 25, min: 0, max: 100 } },
    ]},
    { name: 'Breathing', group: 'Effects', events: [
        { type: 'fading', params: { cycle: 3000, duration: 0, min: 5, max: 80 } },
    ]},
    { name: 'Pulse', group: 'Effects', events: [
        { type: 'on', params:  { duration: 100, brightness: 100, powerSave: 0, pwmDuty: 78 } },
        { type: 'off', params: { duration: 100 } },
    ]},
]
