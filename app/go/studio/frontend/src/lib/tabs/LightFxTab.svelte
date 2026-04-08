<!-- ScaleFX Studio — LightFX Tab -->
<!-- Two-column: Left=Programs & Channels, Right=Landing Lights & Servos -->
<!-- Top bar: slave mode toggle, input channel bar view, mode-dependent servo routing. -->
<script lang="ts">
    import { SendCommand } from '../../../wailsjs/go/main/App'
    import { connectionInfo } from '../stores'
    import ServoWidget from '../components/ServoWidget.svelte'

    export let boardLabel: string = 'LightFX'

    // ─── Slave mode ───
    let slaveMode = false

    $: isHubFX = $connectionInfo.controllerType === 'hubfx'
    $: hubFxConnected = isHubFX && $connectionInfo.connected
    $: controlsDisabled = slaveMode && !hubFxConnected

    // ─── Channel count ───
    const CHANNEL_COUNT = 8
    const SERVO_COUNT = 3

    // ─── Input bar ───
    // Non-slave: Servo 1 = PWM input (single source)
    // Slave: HubFX sends input channel 1-24
    let inputValue = 0                // 0-100 (live reading)
    let inputChannel = 0              // slave: selected input CH index (0-based)

    const slaveInputChannels = Array.from({ length: 24 }, (_, i) => `CH ${i + 1}`)

    // ─── Event Type Definitions ───
    interface ParamDef {
        label: string; key: string; min: number; max: number; step: number; defaultVal: number; unit?: string
    }
    interface EventTypeDef { name: string; value: string; params: ParamDef[] }

    const eventTypes: EventTypeDef[] = [
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

    // ─── Presets ───
    // Timing reference: LedFlashing full cycle = interval × 2, on-time = cycle × duty / 100
    // FAA 14 CFR §25.1401: anti-collision 40-100 flashes/min (600-1500ms cycle)
    // SAE J590: turn signals 60-120 flashes/min (500-1000ms cycle, typical ~700ms)
    interface LightPreset {
        name: string
        group: string
        events: { type: string; params: Record<string, number> }[]
    }

    const presetGroups = ['', 'Aircraft', 'Vehicle', 'Naval', 'Effects'] as const

    const presets: LightPreset[] = [
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

    // ─── Program Definitions ───
    interface SeqEvent {
        id: number; type: string; typeName: string; params: Record<string, number>
    }

    type ChannelMode = 'events' | 'landing'

    interface ChannelState {
        enabled: boolean
        mode: ChannelMode
        preset: string
        events: SeqEvent[]
        brightness: number
        landingSlot: number     // which landing slot to bind (0,1,2)
    }

    interface Program {
        name: string
        bandMin_us: number      // PWM range low  (1000-2000 µs)
        bandMax_us: number      // PWM range high (1000-2000 µs)
        channels: ChannelState[]
        playing: boolean
    }

    const PWM_MIN = 1000
    const PWM_MAX = 2000

    // Auto-distribute PWM range evenly for N programs
    function autoBands(count: number): { min: number, max: number }[] {
        const span = PWM_MAX - PWM_MIN
        const step = Math.floor(span / count)
        return Array.from({ length: count }, (_, i) => ({
            min: PWM_MIN + i * step,
            max: i < count - 1 ? PWM_MIN + (i + 1) * step - 1 : PWM_MAX,
        }))
    }

    // Check overlaps across all programs (ignoring self)
    function bandOverlap(programs: Program[], selfIdx: number): boolean {
        for (let i = 0; i < programs.length; i++) {
            if (i === selfIdx) continue
            const a = programs[selfIdx], b = programs[i]
            if (a.bandMin_us <= b.bandMax_us && a.bandMax_us >= b.bandMin_us) return true
        }
        return false
    }

    let programs: Program[] = (() => {
        const bands = autoBands(3)
        return [
            createProgramWithBand('Off', bands[0].min, bands[0].max, true),
            createProgramWithBand('Flight', bands[1].min, bands[1].max),
            createProgramWithBand('Landing', bands[2].min, bands[2].max),
        ]
    })()
    let activeProgram = 1
    let nextEventId = 1

    function createProgramWithBand(name: string, min_us: number, max_us: number, allDisabled = false): Program {
        return {
            name,
            bandMin_us: min_us,
            bandMax_us: max_us,
            channels: Array.from({ length: CHANNEL_COUNT }, () => ({
                enabled: !allDisabled,
                mode: 'events' as ChannelMode,
                preset: 'Custom',
                events: [],
                brightness: 100,
                landingSlot: 0,
            })),
            playing: false,
        }
    }

    function addProgram() {
        // find first gap or extend beyond last max
        const sorted = [...programs].sort((a, b) => a.bandMin_us - b.bandMin_us)
        const lastMax = sorted.length > 0 ? sorted[sorted.length - 1].bandMax_us : PWM_MIN - 1
        const gap = PWM_MAX - lastMax
        const newMin = Math.min(lastMax + 1, PWM_MAX)
        const newMax = Math.min(newMin + Math.max(Math.floor(gap / 2), 50) - 1, PWM_MAX)
        programs = [...programs, createProgramWithBand(`Program ${programs.length + 1}`, newMin, newMax)]
        activeProgram = programs.length - 1
    }

    function removeProgram(idx: number) {
        if (programs.length <= 1) return
        programs = programs.filter((_, i) => i !== idx)
        if (activeProgram >= programs.length) activeProgram = programs.length - 1
    }

    $: prog = programs[activeProgram]

    // ─── Per-channel play state ───
    let channelPlaying: boolean[] = Array(CHANNEL_COUNT).fill(false)

    // ─── Event editing ───
    let addEventCh: number | null = null
    let addEventType = 0
    let addEventParams: Record<string, number> = {}

    function openAddEvent(chIdx: number) {
        addEventCh = chIdx; addEventType = 0; resetAddParams()
    }
    function resetAddParams() {
        addEventParams = {}
        for (const p of eventTypes[addEventType].params) addEventParams[p.key] = p.defaultVal
    }
    function addEvent(chIdx: number) {
        const evtDef = eventTypes[addEventType]
        prog.channels[chIdx].events = [...prog.channels[chIdx].events, {
            id: nextEventId++, type: evtDef.value, typeName: evtDef.name, params: { ...addEventParams },
        }]
        prog.channels[chIdx].preset = 'Custom'
        addEventCh = null; programs = programs
    }
    function removeEvent(chIdx: number, evtId: number) {
        prog.channels[chIdx].events = prog.channels[chIdx].events.filter(e => e.id !== evtId)
        prog.channels[chIdx].preset = 'Custom'; programs = programs
    }
    function applyPreset(chIdx: number) {
        const name = prog.channels[chIdx].preset
        const preset = presets.find(p => p.name === name)
        if (!preset || name === 'Custom') return
        prog.channels[chIdx].events = preset.events.map(e => ({
            id: nextEventId++, type: e.type,
            typeName: eventTypes.find(et => et.value === e.type)?.name ?? e.type,
            params: { ...e.params },
        }))
        programs = programs
    }

    // ─── Commands ───
    function ch(idx: number): number { return idx + 1 }

    function sendChannelSeq(chIdx: number) {
        const c = ch(chIdx)
        SendCommand(`seq.clear ${c}`)
        for (const evt of prog.channels[chIdx].events) {
            const p = evt.params
            switch (evt.type) {
                case 'on':      SendCommand(`seq.add ${c} on ${p.duration} ${p.brightness} ${p.pwmDuty ?? 0}`); break
                case 'off':     SendCommand(`seq.add ${c} off ${p.duration}`); break
                case 'flash':   SendCommand(`seq.add ${c} flash ${p.interval} ${p.duration} ${p.brightness} ${p.duty ?? 50}`); break
                case 'fadein':  SendCommand(`seq.add ${c} fadein ${p.duration} ${p.brightness}`); break
                case 'fadeout': SendCommand(`seq.add ${c} fadeout ${p.duration} ${p.brightness}`); break
                case 'fading':  SendCommand(`seq.add ${c} fading ${p.cycle} ${p.duration} ${p.min ?? 0} ${p.max ?? 100}`); break
                case 'beacon':  SendCommand(`seq.add ${c} beacon ${p.cycle} ${p.duration} ${p.flashPct ?? 15} ${p.max ?? 100} ${p.min ?? 0}`); break
            }
        }
        SendCommand(`seq.start ${c}`)
    }
    function stopChannel(chIdx: number) { SendCommand(`seq.stop ${ch(chIdx)}`) }

    function playChannel(chIdx: number) {
        sendChannelSeq(chIdx); channelPlaying[chIdx] = true; channelPlaying = channelPlaying
    }
    function stopChannelPlay(chIdx: number) {
        stopChannel(chIdx); channelPlaying[chIdx] = false; channelPlaying = channelPlaying
    }

    function playProgram() {
        prog.channels.forEach((c, i) => { if (c.enabled && c.mode === 'events' && c.events.length > 0) sendChannelSeq(i) })
        prog.playing = true; programs = programs
    }
    function stopProgram() {
        prog.channels.forEach((_, i) => { SendCommand(`seq.stop ${ch(i)}`); channelPlaying[i] = false })
        prog.playing = false; channelPlaying = channelPlaying; programs = programs
    }

    function enableChannel(chIdx: number) { SendCommand(`enable ${ch(chIdx)}`); prog.channels[chIdx].enabled = true; programs = programs }
    function disableChannel(chIdx: number) { SendCommand(`disable ${ch(chIdx)}`); prog.channels[chIdx].enabled = false; programs = programs }

    function setDirectBrightness(chIdx: number) {
        SendCommand(`led ${ch(chIdx)} ${prog.channels[chIdx].brightness}`)
    }

    // ─── Event summary ───
    function durLabel(ms: number): string { return ms === 0 ? '∞' : `${ms}ms` }

    function eventSummary(evt: SeqEvent): string {
        const p = evt.params
        switch (evt.type) {
            case 'on':      return `${durLabel(p.duration)} @ ${p.brightness}%` + (p.duration === 0 ? ' ⛔ no loop' : '')
            case 'off':     return durLabel(p.duration) + (p.duration === 0 ? ' ⛔ no loop' : '')
            case 'flash':   return `⚡ ${p.interval}ms × ${durLabel(p.duration)} ${p.brightness}% duty:${p.duty}%`
            case 'fadein':  return `↗ ${p.duration}ms → ${p.brightness}%`
            case 'fadeout': return `↘ ${p.duration}ms → ${p.brightness}%`
            case 'fading':  return `~ ${p.cycle}ms cycle, ${durLabel(p.duration)} (${p.min}-${p.max}%)`
            case 'beacon':  return `◉ ${p.cycle}ms cycle, ${durLabel(p.duration)} flash ${p.flashPct}% peak ${p.max}%`
            default: return ''
        }
    }

    // ─── Landing Lights (right column) ───
    // Non-slave: 2 slots (servo 2 & 3), Slave: 3 slots (all servos)
    $: landingSlotCount = slaveMode ? 3 : 2

    interface LandingSlot {
        servo: number      // servo id (1-3)
        ledChannel: number // LED channel (1-8)
        deploy_us: number
        retract_us: number
        brightness: number
        bound: boolean
        servoEnabled: boolean
        enabled: boolean
    }

    // Always keep 3 backing slots; display only landingSlotCount
    let landingSlots: LandingSlot[] = [
        { servo: 2, ledChannel: 1, deploy_us: 2000, retract_us: 1000, brightness: 100, bound: false, servoEnabled: true, enabled: true },
        { servo: 3, ledChannel: 2, deploy_us: 2000, retract_us: 1000, brightness: 100, bound: false, servoEnabled: true, enabled: true },
        { servo: 1, ledChannel: 3, deploy_us: 2000, retract_us: 1000, brightness: 100, bound: false, servoEnabled: true, enabled: true },
    ]
    $: visibleSlots = landingSlots.slice(0, landingSlotCount)

    function bindLanding(idx: number) {
        const s = landingSlots[idx]
        SendCommand(`landing.bind ${idx + 1} ${s.servo} ${s.ledChannel} ${s.deploy_us} ${s.retract_us} ${s.brightness}`)
        landingSlots[idx].bound = true; landingSlots = landingSlots
    }
    function unbindLanding(idx: number) {
        SendCommand(`landing.unbind ${idx + 1}`); landingSlots[idx].bound = false; landingSlots = landingSlots
    }
    function deployLanding(idx: number) { SendCommand(`landing.deploy ${idx + 1}`) }
    function retractLanding(idx: number) { SendCommand(`landing.retract ${idx + 1}`) }
    function deployAllLanding() { SendCommand('landing.deploy') }
    function retractAllLanding() { SendCommand('landing.retract') }

    // ─── Servo definitions ───
    // Non-slave: Servo 1 = input, Servos 2-3 available for landing lights
    // Slave: all 3 servos available
    $: availableServos = slaveMode
        ? [{ id: 1, name: 'Servo 1' }, { id: 2, name: 'Servo 2' }, { id: 3, name: 'Servo 3' }]
        : [{ id: 2, name: 'Servo 2' }, { id: 3, name: 'Servo 3' }]

    // ─── Master brightness ───
    let masterBrightness = 100
    function setMasterBrightness() { SendCommand(`brightness ${masterBrightness}`) }
    function allOff() { SendCommand('led.off 0') }
</script>

<div class="tab-root">
    <!-- ─── Title bar ─── -->
    <div class="tab-title-bar">
        <h2>{boardLabel}</h2>

        <div class="master-ctrl">
            <span class="field-label">Master</span>
            <input type="range" bind:value={masterBrightness} min="0" max="100" class="slider" style="width: 80px" />
            <span class="slider-val">{masterBrightness}%</span>
            <button class="small" on:click={setMasterBrightness}>Set</button>
            <button class="small danger" on:click={allOff}>All Off</button>
        </div>

        <div class="mode-toggle">
            <!-- svelte-ignore a11y-label-has-associated-control -->
            <label class="toggle-label">
                <input type="checkbox" bind:checked={slaveMode} />
                <span>Slave Mode</span>
            </label>
            {#if slaveMode}
                <span class="mode-hint" class:connected={hubFxConnected}>
                    {hubFxConnected ? 'HubFX connected' : 'Waiting for HubFX…'}
                </span>
            {/if}
        </div>
    </div>

    <!-- ─── Input bar ─── -->
    <div class="input-bar">
        {#if slaveMode}
            <span class="bar-label">Input Channel</span>
            <select bind:value={inputChannel} class="input-select">
                {#each slaveInputChannels as ch, i}
                    <option value={i}>{ch}</option>
                {/each}
            </select>
        {:else}
            <span class="bar-label">Input (Servo 1)</span>
        {/if}
        <div class="input-bar-track">
            <div class="input-bar-fill" style="width: {inputValue}%"></div>
        </div>
        <span class="input-bar-val">{inputValue}%</span>
        <span class="bar-hint">Band selects active program</span>
    </div>

    <!-- ─── Content: two-column ─── -->
    <div class="tab-scroll">
        <div class="content-wrap" class:controls-disabled={controlsDisabled}>
            {#if controlsDisabled}
                <div class="disabled-overlay"><span>Slave mode — connect HubFX to control channels</span></div>
            {/if}

            <div class="two-col">
                <!-- ═══════ LEFT: Programs & Channels ═══════ -->
                <div class="col">
                    <!-- Program selector bar -->
                    <div class="program-bar">
                        <div class="program-tabs">
                            {#each programs as p, i}
                                <button class="prog-tab" class:active={i === activeProgram}
                                        on:click={() => activeProgram = i}>
                                    {p.name}
                                </button>
                            {/each}
                            <button class="prog-tab add-tab" on:click={addProgram} title="Add program">+</button>
                        </div>
                        <div class="program-actions">
                            {#if prog.playing}
                                <button class="small danger" on:click={stopProgram}>■ Stop</button>
                            {:else}
                                <button class="small primary" on:click={playProgram}>▶ Play</button>
                            {/if}
                        </div>
                    </div>

                    <!-- Program settings -->
                    <div class="program-settings">
                        <div class="form-row">
                            <span class="field-label">Name</span>
                            <input type="text" bind:value={prog.name} class="field-input"
                                   style="width: 140px" disabled={controlsDisabled} />
                            {#if programs.length > 1}
                                <button class="small danger" style="margin-left: auto"
                                        on:click={() => removeProgram(activeProgram)}
                                        disabled={controlsDisabled}>✕ Remove</button>
                            {/if}
                        </div>
                        <div class="band-row">
                            <span class="field-label band-label"
                                  class:band-overlap={bandOverlap(programs, activeProgram)}
                                  title="PWM input range (µs) — must not overlap other programs">Band µs</span>
                            <span class="band-val">{prog.bandMin_us}</span>
                            <div class="band-slider-track">
                                <div class="band-fill" style="left: {(prog.bandMin_us - PWM_MIN) / (PWM_MAX - PWM_MIN) * 100}%; right: {(PWM_MAX - prog.bandMax_us) / (PWM_MAX - PWM_MIN) * 100}%"></div>
                                <input type="range" class="band-range" bind:value={prog.bandMin_us}
                                       min={PWM_MIN} max={PWM_MAX} step={10}
                                       on:input={() => { if (prog.bandMin_us > prog.bandMax_us) prog.bandMax_us = prog.bandMin_us }}
                                       disabled={controlsDisabled} />
                                <input type="range" class="band-range" bind:value={prog.bandMax_us}
                                       min={PWM_MIN} max={PWM_MAX} step={10}
                                       on:input={() => { if (prog.bandMax_us < prog.bandMin_us) prog.bandMin_us = prog.bandMax_us }}
                                       disabled={controlsDisabled} />
                            </div>
                            <span class="band-val">{prog.bandMax_us}</span>
                            {#if bandOverlap(programs, activeProgram)}
                                <span class="band-warn" title="Overlaps another program's range">⚠</span>
                            {/if}
                        </div>
                    </div>

                    <!-- Channels list -->
                    <section class="card channels-card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/></svg> Channels</h3>
                        </div>

                        {#each prog.channels as channel, idx}
                            <div class="channel" class:ch-disabled={!channel.enabled}
                                 class:ch-landing={channel.mode === 'landing'}>
                                <!-- Channel header -->
                                <div class="ch-header">
                                    <span class="ch-num">{ch(idx)}</span>

                                    <label class="ch-enable-toggle" title={channel.enabled ? 'Disable' : 'Enable'}>
                                        <input type="checkbox" checked={channel.enabled}
                                               on:change={() => channel.enabled ? disableChannel(idx) : enableChannel(idx)} />
                                    </label>

                                    <!-- Mode selector -->
                                    <div class="ch-mode-chips">
                                        <button class="mode-chip" class:active={channel.mode === 'events'}
                                                on:click={() => { channel.mode = 'events'; programs = programs }}
                                                disabled={!channel.enabled}>Events</button>
                                        <button class="mode-chip" class:active={channel.mode === 'landing'}
                                                on:click={() => { channel.mode = 'landing'; programs = programs }}
                                                disabled={!channel.enabled}>Landing</button>
                                    </div>

                                    {#if channel.mode === 'events'}
                                        <select class="ch-preset" bind:value={channel.preset}
                                                on:change={() => applyPreset(idx)} disabled={!channel.enabled}>
                                            <option value="Custom">Custom</option>
                                            {#each presetGroups.filter(g => g !== '') as group}
                                                <optgroup label={group}>
                                                    {#each presets.filter(p => p.group === group) as preset}
                                                        <option value={preset.name}>{preset.name}</option>
                                                    {/each}
                                                </optgroup>
                                            {/each}
                                        </select>

                                        <span class="ch-summary">{channel.events.length} evt{channel.events.length !== 1 ? 's' : ''}</span>
                                    {:else}
                                        <span class="ch-summary landing-hint">→ Landing Slot {channel.landingSlot + 1}</span>
                                    {/if}

                                    <!-- play/stop per channel -->
                                    <div class="ch-play">
                                        {#if channelPlaying[idx]}
                                            <button class="tiny danger" on:click={() => stopChannelPlay(idx)} title="Stop">■</button>
                                        {:else}
                                            <button class="tiny primary" on:click={() => playChannel(idx)} title="Play"
                                                    disabled={!channel.enabled || channel.mode !== 'events' || channel.events.length === 0}>▶</button>
                                        {/if}
                                    </div>
                                </div>

                                <!-- Expanded event list (always visible for event-mode channels) -->
                                {#if channel.enabled && channel.mode === 'events'}
                                    <div class="ch-detail">
                                        <!-- Direct brightness -->
                                        <div class="detail-row">
                                            <span class="field-label">Bright</span>
                                            <input type="range" bind:value={channel.brightness}
                                                   min="0" max="100" class="slider" style="width: 100px" />
                                            <span class="slider-val-sm">{channel.brightness}%</span>
                                            <button class="tiny" on:click={() => setDirectBrightness(idx)}>Set</button>
                                        </div>

                                        <!-- Event list -->
                                        {#if channel.events.length > 0}
                                            <div class="event-list">
                                                {#each channel.events as evt, evtIdx (evt.id)}
                                                    <div class="event-item">
                                                        <span class="evt-idx">{evtIdx + 1}</span>
                                                        <span class="evt-type">{evt.typeName}</span>
                                                        <span class="evt-params">{eventSummary(evt)}</span>
                                                        <button class="tiny danger" on:click={() => removeEvent(idx, evt.id)} title="Remove">✕</button>
                                                    </div>
                                                {/each}
                                            </div>
                                        {:else}
                                            <div class="empty-events">No events — select a preset or add manually</div>
                                        {/if}

                                        <!-- Add event form -->
                                        {#if addEventCh === idx}
                                            <div class="add-event-form">
                                                <div class="add-event-top">
                                                    <select bind:value={addEventType} on:change={resetAddParams}
                                                            class="evt-type-select">
                                                        {#each eventTypes as et, eti}
                                                            <option value={eti}>{et.name}</option>
                                                        {/each}
                                                    </select>
                                                    {#each eventTypes[addEventType].params as p}
                                                        <div class="param-field">
                                                            <span class="param-label">{p.label}</span>
                                                            <input type="number" bind:value={addEventParams[p.key]}
                                                                   class="param-input" min={p.min} max={p.max} step={p.step} />
                                                            {#if p.unit}<span class="param-unit">{p.unit}</span>{/if}
                                                        </div>
                                                    {/each}
                                                </div>
                                                <div class="add-event-actions">
                                                    <button class="small primary" on:click={() => addEvent(idx)}>Add</button>
                                                    <button class="small" on:click={() => addEventCh = null}>Cancel</button>
                                                </div>
                                            </div>
                                        {:else}
                                            <button class="add-btn" on:click={() => openAddEvent(idx)}>+ Add Event</button>
                                        {/if}
                                    </div>
                                {/if}

                                <!-- Landing mode binding selector -->
                                {#if channel.enabled && channel.mode === 'landing'}
                                    <div class="ch-detail landing-detail">
                                        <span class="field-label">Landing Slot</span>
                                        <select bind:value={channel.landingSlot} class="field-input" style="width: 80px">
                                            {#each Array.from({length: landingSlotCount}, (_, i) => i) as s}
                                                <option value={s}>Slot {s + 1}</option>
                                            {/each}
                                        </select>
                                        <span class="landing-slot-info">
                                            Servo {landingSlots[channel.landingSlot].servo},
                                            LED {landingSlots[channel.landingSlot].ledChannel}
                                        </span>
                                    </div>
                                {/if}
                            </div>
                        {/each}
                    </section>
                </div>

                <!-- ═══════ RIGHT: Landing Lights & Servos ═══════ -->
                <div class="col">
                    <!-- Landing Lights Settings -->
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18h6"/><path d="M10 22h4"/><path d="M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z"/></svg> Landing Lights</h3>
                            <div class="header-actions">
                                <button class="small primary" disabled={controlsDisabled}
                                        on:click={deployAllLanding}>Deploy All</button>
                                <button class="small" disabled={controlsDisabled}
                                        on:click={retractAllLanding}>Retract All</button>
                            </div>
                        </div>

                        <div class="landing-slots">
                            {#each visibleSlots as slot, idx}
                                <div class="landing-slot" class:slot-bound={slot.bound}
                                     class:slot-off={!slot.enabled}>
                                    <div class="slot-header">
                                        <span class="slot-label">Slot {idx + 1}</span>
                                        <!-- svelte-ignore a11y-label-has-associated-control -->
                                        <label class="slot-enable-toggle" title={slot.enabled ? 'Disable' : 'Enable'}>
                                            <input type="checkbox" bind:checked={slot.enabled}
                                                   disabled={controlsDisabled} />
                                            <span class="slot-enable-text">{slot.enabled ? 'ON' : 'OFF'}</span>
                                        </label>
                                        {#if slot.bound}
                                            <span class="slot-badge bound">BOUND</span>
                                        {:else}
                                            <span class="slot-badge unbound">UNBOUND</span>
                                        {/if}
                                    </div>

                                    <div class="slot-fields">
                                        <div class="field-col">
                                            <span class="flbl">Servo</span>
                                            <select bind:value={slot.servo} class="field-input narrow"
                                                    disabled={controlsDisabled}>
                                                {#each availableServos as s}
                                                    <option value={s.id}>{s.name}</option>
                                                {/each}
                                            </select>
                                        </div>
                                        <div class="field-col">
                                            <span class="flbl">LED Ch</span>
                                            <select bind:value={slot.ledChannel} class="field-input narrow"
                                                    disabled={controlsDisabled}>
                                                {#each Array.from({length: 8}, (_, i) => i + 1) as l}
                                                    <option value={l}>{l}</option>
                                                {/each}
                                            </select>
                                        </div>
                                        <div class="field-col">
                                            <span class="flbl">Deploy µs</span>
                                            <input type="number" bind:value={slot.deploy_us}
                                                   class="field-input" min="500" max="2500" step="10"
                                                   disabled={controlsDisabled} />
                                        </div>
                                        <div class="field-col">
                                            <span class="flbl">Retract µs</span>
                                            <input type="number" bind:value={slot.retract_us}
                                                   class="field-input" min="500" max="2500" step="10"
                                                   disabled={controlsDisabled} />
                                        </div>
                                        <div class="field-col">
                                            <span class="flbl">Bright %</span>
                                            <input type="number" bind:value={slot.brightness}
                                                   class="field-input narrow" min="0" max="100"
                                                   disabled={controlsDisabled} />
                                        </div>
                                    </div>

                                    <!-- Servo enable toggle -->
                                    <div class="slot-servo-toggle">
                                        <!-- svelte-ignore a11y-label-has-associated-control -->
                                        <label class="toggle-label">
                                            <input type="checkbox" bind:checked={slot.servoEnabled}
                                                   disabled={controlsDisabled} />
                                            <span>Servo Output</span>
                                        </label>
                                    </div>

                                    <!-- Servo output view when enabled -->
                                    {#if slot.servoEnabled}
                                        <div class="servo-output-view">
                                            <div class="servo-bar-track">
                                                <div class="servo-bar-deploy" style="left: {((slot.deploy_us - 500) / 2000) * 100}%"></div>
                                                <div class="servo-bar-retract" style="left: {((slot.retract_us - 500) / 2000) * 100}%"></div>
                                            </div>
                                            <div class="servo-bar-labels">
                                                <span>500µs</span><span>1500µs</span><span>2500µs</span>
                                            </div>
                                        </div>
                                    {/if}

                                    <div class="slot-actions">
                                        {#if slot.bound}
                                            <button class="small" on:click={() => deployLanding(idx)}
                                                    disabled={controlsDisabled}>Deploy</button>
                                            <button class="small" on:click={() => retractLanding(idx)}
                                                    disabled={controlsDisabled}>Retract</button>
                                            <button class="small danger" on:click={() => unbindLanding(idx)}
                                                    disabled={controlsDisabled}>Unbind</button>
                                        {:else}
                                            <button class="small primary" on:click={() => bindLanding(idx)}
                                                    disabled={controlsDisabled}>Bind</button>
                                        {/if}
                                    </div>
                                </div>
                            {/each}
                        </div>
                    </section>

                    <!-- Servo widget -->
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"/></svg> Servos</h3>
                            {#if !slaveMode}
                                <span class="header-hint">Servo 1 = Input (see bar above)</span>
                            {/if}
                        </div>
                        <ServoWidget servos={availableServos} disabled={controlsDisabled}
                                     label="Servos" showHeader={false} />
                    </section>
                </div>
            </div>
        </div>
    </div>
</div>

<style>
    .tab-root {
        display: flex;
        flex-direction: column;
        height: 100%;
        min-height: 0;
    }

    /* ─── Title bar ─── */
    .tab-title-bar {
        display: flex;
        align-items: center;
        gap: 16px;
        padding: 10px 16px;
        background: var(--bg-raised);
        border-bottom: 1px solid var(--border);
        flex-shrink: 0;
    }

    .tab-title-bar h2 {
        font-size: 16px;
        font-weight: 600;
        color: var(--text-bright);
    }

    .master-ctrl {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .mode-toggle {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-left: auto;
    }

    .toggle-label {
        display: flex;
        align-items: center;
        gap: 6px;
        font-size: 11px;
        font-weight: 600;
        color: var(--text);
        cursor: pointer;
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .toggle-label input { accent-color: var(--accent); }

    .mode-hint {
        font-size: 10px;
        color: var(--warning, #d7ba7d);
        font-style: italic;
    }

    .mode-hint.connected {
        color: var(--ok, #4ec9b0);
        font-style: normal;
    }

    /* ─── Input bar ─── */
    .input-bar {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 7px 16px;
        background: color-mix(in srgb, var(--accent) 4%, var(--bg-surface));
        border-bottom: 1px solid var(--border);
        flex-shrink: 0;
    }

    .bar-label {
        font-size: 11px;
        font-weight: 600;
        color: var(--text-bright);
        text-transform: uppercase;
        letter-spacing: 0.3px;
        white-space: nowrap;
    }

    .input-select {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 4px;
        color: var(--text-bright);
        font-size: 12px;
        font-family: var(--font-mono);
        padding: 3px 6px;
        cursor: pointer;
    }

    .input-bar-track {
        flex: 1;
        height: 8px;
        background: var(--bg-input);
        border-radius: 4px;
        overflow: hidden;
        max-width: 200px;
    }

    .input-bar-fill {
        height: 100%;
        background: var(--accent);
        border-radius: 4px;
        transition: width 0.15s;
    }

    .input-bar-val {
        font-family: var(--font-mono);
        font-size: 11px;
        font-weight: 600;
        color: var(--accent);
        min-width: 32px;
        text-align: right;
    }

    .bar-hint {
        font-size: 10px;
        color: var(--text-dim);
        font-style: italic;
    }

    /* ─── Scrollable area ─── */
    .tab-scroll {
        flex: 1;
        overflow-y: auto;
        padding: 14px 16px;
        min-height: 0;
    }

    .content-wrap { position: relative; }

    .content-wrap.controls-disabled {
        opacity: 0.4;
        pointer-events: none;
    }

    .disabled-overlay {
        position: absolute;
        inset: 0;
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 2;
        background: color-mix(in srgb, var(--bg-base) 60%, transparent);
        border-radius: 6px;
        pointer-events: none;
    }

    .disabled-overlay span {
        font-size: 12px;
        font-weight: 600;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.4px;
        padding: 8px 16px;
        background: var(--bg-raised);
        border: 1px solid var(--border);
        border-radius: 4px;
    }

    /* ─── Two-Column Layout ─── */
    .two-col {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 16px;
    }

    .col {
        display: flex;
        flex-direction: column;
        gap: 14px;
        min-width: 0;
    }

    /* ─── Card ─── */
    .card {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 14px 16px;
    }

    .card-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 12px;
    }

    .card-header h3 {
        font-size: 14px;
        font-weight: 600;
        color: var(--text-bright);
        text-transform: uppercase;
        letter-spacing: 0.5px;
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .card-header h3 svg {
        opacity: 0.7;
        flex-shrink: 0;
    }

    .header-actions {
        display: flex;
        gap: 6px;
    }

    .header-hint {
        font-size: 10px;
        color: var(--text-dim);
        font-style: italic;
    }

    /* ─── Program bar ─── */
    .program-bar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 8px;
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 8px 12px;
    }

    .program-tabs {
        display: flex;
        gap: 4px;
    }

    .prog-tab {
        font-size: 12px;
        font-weight: 600;
        padding: 4px 14px;
        border-radius: 4px;
        background: var(--bg-raised);
        border: 1px solid var(--border);
        color: var(--text-dim);
        cursor: pointer;
        transition: all 0.15s;
    }

    .prog-tab.active {
        background: color-mix(in srgb, var(--accent) 20%, var(--bg-raised));
        color: var(--accent);
        border-color: var(--accent);
    }

    .prog-tab:hover:not(.active) {
        background: color-mix(in srgb, var(--accent) 8%, var(--bg-raised));
    }

    .add-tab {
        border-style: dashed;
        color: var(--text-dim);
        padding: 4px 10px;
    }

    .add-tab:hover { color: var(--accent); border-color: var(--accent); }

    .program-actions {
        display: flex;
        gap: 6px;
    }

    /* ─── Program settings ─── */
    .program-settings {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 8px 12px;
    }

    .form-row {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
    }

    /* ─── Channels card ─── */
    .channels-card {
        padding: 10px 12px;
    }

    .channels-card .card-header { margin-bottom: 8px; }

    /* ─── Channel ─── */
    .channel {
        border: 1px solid color-mix(in srgb, var(--border) 60%, transparent);
        border-radius: 4px;
        margin-bottom: 4px;
        overflow: hidden;
        transition: opacity 0.15s;
    }

    .channel.ch-disabled { opacity: 0.5; }

    .channel.ch-landing {
        border-color: color-mix(in srgb, var(--ok, #4ec9b0) 40%, transparent);
    }

    .ch-header {
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 5px 8px;
        background: var(--bg-raised);
        transition: background 0.1s;
    }

    .ch-num {
        font-family: var(--font-mono);
        font-size: 12px;
        font-weight: 700;
        color: var(--text-bright);
        width: 16px;
        text-align: center;
    }

    .ch-enable-toggle {
        cursor: pointer;
        display: flex;
        align-items: center;
    }

    .ch-enable-toggle input { accent-color: var(--accent); margin: 0; }

    .ch-mode-chips {
        display: flex;
        gap: 2px;
    }

    .mode-chip {
        font-size: 10px;
        font-weight: 600;
        padding: 2px 8px;
        border-radius: 3px;
        background: var(--bg-input);
        border: 1px solid var(--border);
        color: var(--text-dim);
        cursor: pointer;
        transition: all 0.15s;
    }

    .mode-chip.active {
        background: color-mix(in srgb, var(--accent) 18%, var(--bg-raised));
        color: var(--accent);
        border-color: var(--accent);
    }

    .mode-chip:disabled { opacity: 0.4; cursor: not-allowed; }

    .ch-preset {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        font-size: 10px;
        font-family: var(--font-mono);
        padding: 2px 4px;
        cursor: pointer;
        width: 100px;
    }

    .ch-summary {
        font-size: 10px;
        color: var(--text-dim);
        font-family: var(--font-mono);
        flex: 1;
        text-align: right;
        padding-right: 4px;
    }

    .landing-hint {
        color: var(--ok, #4ec9b0);
        font-weight: 600;
    }

    .ch-play {
        display: flex;
        gap: 2px;
    }

    /* ─── Channel detail ─── */
    .ch-detail {
        padding: 6px 10px 8px 28px;
        background: var(--bg-surface);
        border-top: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }

    .landing-detail {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 6px 10px 6px 28px;
    }

    .landing-slot-info {
        font-size: 10px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .detail-row {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 6px;
    }

    /* ─── Event list ─── */
    .event-list {
        border: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        border-radius: 3px;
        margin-bottom: 6px;
        overflow: hidden;
    }

    .event-item {
        display: flex;
        align-items: center;
        gap: 5px;
        padding: 3px 6px;
        border-bottom: 1px solid color-mix(in srgb, var(--border) 30%, transparent);
    }

    .event-item:last-child { border-bottom: none; }

    .evt-idx {
        font-family: var(--font-mono);
        font-size: 9px;
        color: var(--text-dim);
        width: 14px;
        text-align: right;
    }

    .evt-type {
        font-family: var(--font-mono);
        font-size: 10px;
        font-weight: 600;
        color: var(--accent);
        min-width: 54px;
    }

    .evt-params {
        font-family: var(--font-mono);
        font-size: 9px;
        color: var(--text);
        flex: 1;
    }

    .empty-events {
        font-size: 10px;
        color: var(--text-dim);
        padding: 6px;
        text-align: center;
        font-style: italic;
    }

    /* ─── Add event form ─── */
    .add-event-form {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 4px;
        padding: 6px;
        margin-top: 4px;
    }

    .add-event-top {
        display: flex;
        align-items: flex-end;
        gap: 6px;
        flex-wrap: wrap;
    }

    .evt-type-select {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text-bright);
        font-size: 10px;
        font-family: var(--font-mono);
        font-weight: 600;
        padding: 3px 4px;
        cursor: pointer;
    }

    .param-field {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .param-label {
        font-size: 9px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .param-input {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        font-family: var(--font-mono);
        font-size: 10px;
        padding: 2px 4px;
        width: 60px;
    }

    .param-input:focus { border-color: var(--border-focus); outline: none; }

    .param-unit {
        font-size: 8px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .add-event-actions {
        display: flex;
        gap: 4px;
        margin-top: 6px;
    }

    .add-btn {
        margin-top: 4px;
        width: 100%;
        text-align: center;
        border-style: dashed;
        color: var(--text-dim);
        font-size: 10px;
        padding: 3px 8px;
    }

    .add-btn:hover { color: var(--accent); border-color: var(--accent); }

    /* ─── Landing Lights ─── */
    .landing-slots {
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .landing-slot {
        background: var(--bg-raised);
        border: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        border-radius: 5px;
        padding: 10px;
        transition: border-color 0.2s;
    }

    .landing-slot.slot-bound {
        border-color: color-mix(in srgb, var(--ok, #4ec9b0) 50%, transparent);
    }

    .landing-slot.slot-off {
        opacity: 0.45;
    }

    .slot-header {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 8px;
    }

    .slot-enable-toggle {
        display: flex;
        align-items: center;
        gap: 4px;
        font-size: 10px;
        font-weight: 700;
        cursor: pointer;
    }

    .slot-enable-toggle input[type="checkbox"] {
        accent-color: var(--accent);
        margin: 0;
    }

    .slot-enable-text {
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .slot-label {
        font-family: var(--font-mono);
        font-size: 12px;
        font-weight: 700;
        color: var(--text-bright);
    }

    .slot-badge {
        font-size: 9px;
        font-weight: 700;
        padding: 1px 6px;
        border-radius: 3px;
    }

    .slot-badge.bound {
        color: var(--ok, #4ec9b0);
        background: color-mix(in srgb, var(--ok, #4ec9b0) 12%, transparent);
    }

    .slot-badge.unbound {
        color: var(--text-dim);
        background: color-mix(in srgb, var(--border) 40%, transparent);
    }

    .slot-fields {
        display: flex;
        align-items: flex-end;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 8px;
    }

    .field-col {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .flbl {
        font-size: 9px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .slot-servo-toggle {
        margin-bottom: 8px;
    }

    /* ─── Servo output view ─── */
    .servo-output-view {
        margin-bottom: 8px;
    }

    .servo-bar-track {
        position: relative;
        height: 10px;
        background: var(--bg-input);
        border-radius: 5px;
        overflow: visible;
    }

    .servo-bar-deploy, .servo-bar-retract {
        position: absolute;
        top: -2px;
        width: 4px;
        height: 14px;
        border-radius: 2px;
        transform: translateX(-2px);
    }

    .servo-bar-deploy {
        background: var(--ok, #4ec9b0);
    }

    .servo-bar-retract {
        background: var(--accent);
    }

    .servo-bar-labels {
        display: flex;
        justify-content: space-between;
        margin-top: 2px;
    }

    .servo-bar-labels span {
        font-size: 8px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .slot-actions {
        display: flex;
        gap: 4px;
    }

    /* ─── Shared ─── */
    .field-label {
        font-size: 12px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
        white-space: nowrap;
    }

    .field-input {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        font-family: var(--font-mono);
        font-size: 12px;
        padding: 3px 6px;
        width: 72px;
    }

    .field-input.narrow { width: 50px; }
    .field-input:focus { border-color: var(--border-focus); outline: none; }

    /* ─── Band range slider ─── */
    .band-row {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-top: 6px;
    }

    .band-slider-track {
        position: relative;
        flex: 1;
        height: 20px;
        display: flex;
        align-items: center;
    }

    .band-slider-track::before {
        content: '';
        position: absolute;
        left: 0; right: 0;
        height: 4px;
        background: var(--bg-input);
        border-radius: 2px;
        top: 50%;
        transform: translateY(-50%);
    }

    .band-fill {
        position: absolute;
        height: 4px;
        background: var(--accent);
        border-radius: 2px;
        top: 50%;
        transform: translateY(-50%);
        pointer-events: none;
        opacity: 0.6;
    }

    .band-range {
        position: absolute;
        width: 100%;
        height: 20px;
        -webkit-appearance: none;
        appearance: none;
        background: transparent;
        pointer-events: none;
        margin: 0;
        padding: 0;
    }

    .band-range::-webkit-slider-thumb {
        -webkit-appearance: none;
        width: 14px;
        height: 14px;
        border-radius: 50%;
        background: var(--accent);
        border: 2px solid var(--bg-surface);
        cursor: pointer;
        pointer-events: auto;
        box-shadow: 0 1px 3px rgba(0,0,0,0.3);
    }

    .band-range::-moz-range-thumb {
        width: 14px;
        height: 14px;
        border-radius: 50%;
        background: var(--accent);
        border: 2px solid var(--bg-surface);
        cursor: pointer;
        pointer-events: auto;
        box-shadow: 0 1px 3px rgba(0,0,0,0.3);
    }

    .band-range::-webkit-slider-runnable-track {
        height: 4px;
        background: transparent;
    }

    .band-range::-moz-range-track {
        height: 4px;
        background: transparent;
    }

    .band-val {
        font-family: var(--font-mono);
        font-size: 11px;
        color: var(--text-dim);
        min-width: 32px;
        text-align: center;
    }

    .band-label.band-overlap {
        color: var(--warning);
    }

    .band-warn {
        font-size: 14px;
        color: var(--warning);
        margin-left: 2px;
    }

    .slider { accent-color: var(--accent); }

    .slider-val {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text-dim);
        min-width: 36px;
        text-align: right;
    }

    .slider-val-sm {
        font-family: var(--font-mono);
        font-size: 10px;
        color: var(--text-dim);
        min-width: 28px;
        text-align: right;
    }

    /* ─── Buttons ─── */
    button {
        background: var(--bg-raised);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        cursor: pointer;
        transition: background 0.1s, border-color 0.1s;
    }

    button:hover:not(:disabled) {
        background: color-mix(in srgb, var(--accent) 10%, var(--bg-raised));
        border-color: var(--accent);
    }

    button:disabled {
        opacity: 0.45;
        cursor: not-allowed;
    }

    .small {
        font-size: 11px;
        padding: 3px 10px;
    }

    .tiny {
        font-size: 9px;
        padding: 1px 6px;
        line-height: 1.2;
    }

    .primary {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
        border-color: var(--accent);
        color: var(--accent);
    }

    .danger {
        color: var(--error);
        border-color: color-mix(in srgb, var(--error) 40%, transparent);
    }

    .danger:hover:not(:disabled) {
        background: color-mix(in srgb, var(--error) 15%, var(--bg-raised));
        border-color: var(--error);
    }
</style>
