<!-- ScaleFX Studio — LightFX Tab -->
<!-- Two-column: Left=Programs & Channels, Right=Landing Light Groups (servo bindings) -->
<!-- Top bar: slave mode toggle, input channel bar view, mode-dependent servo routing. -->
<script lang="ts">
    import { SendCommand, UploadConfig } from '../../../wailsjs/go/main/App'
    import { EventsOn, EventsOff } from '../../../wailsjs/runtime/runtime'
    import { connectionInfo } from '../stores'
    import { eventTypes, presets, presetGroups } from '../light-data'
    import SaveConfigDialog from '../dialogs/SaveConfigDialog.svelte'
    import LightFxBoardDialog from '../dialogs/LightFxBoardDialog.svelte'
    import ServoCalibrationDialog from '../dialogs/ServoCalibrationDialog.svelte'
    import { LightConfigVerifier, type LightConfig, type GroupPolicy } from '../config/light-verifier'
    import { generateLightYaml, parseLightYaml } from '../config/config-yaml-gen'
    import { EMPTY_RESULT, type VerifyResult } from '../config/config-verifier'
    import type { BoardConfigDriver } from '../config/board-driver'
    import { autoLoadOnConnect, loadConfigFromDevice } from '../config/config-loader'
    import { createLivePusher, pushBadgeText } from '../live-push'
    import { onMount, onDestroy } from 'svelte'

    export let boardLabel: string = 'LightFX'

    // ─── Mode (inferred from connected controller) ───
    // Slave mode is automatic — set when the user is configuring a LightFX
    // through HubFX (controllerType='hubfx'). Direct LightFX = controllerType='lightfx'.
    $: isHubFX = $connectionInfo.controllerType === 'hubfx'
    $: hubFxConnected = isHubFX && $connectionInfo.connected
    $: slaveMode = isHubFX
    $: controlsDisabled = slaveMode && !hubFxConnected
    // "Direct" = directly attached to a LightFX Pico (not slaved through HubFX).
    // Mirrors GearControlTab: settings saved here go to /lightfx.yaml on the
    // local board, NOT to the HubFX-side slave config.
    $: isDirect = $connectionInfo.connected && $connectionInfo.controllerType === 'lightfx'

    // Persistent dismissal of the direct-mode warning
    const WARN_KEY = 'lightfx.direct.warning.dismissed'
    let warningDismissed = (typeof localStorage !== 'undefined') && localStorage.getItem(WARN_KEY) === '1'
    function dismissWarning(persist: boolean) {
        warningDismissed = true
        if (persist && typeof localStorage !== 'undefined') localStorage.setItem(WARN_KEY, '1')
    }

    // ─── Channel count ───
    const CHANNEL_COUNT = 8

    // ─── Input bar ───
    // Non-slave: Servo 3 (GP10, servo2 in the broadcast) doubles as the RC PWM
    //   input — its measured pulse width is the live program selector.
    // Slave: HubFX sends input channel 1-24. The selected channel index is a
    //   tab-local UI hint — the firmware doesn't echo the assignment back.
    let liveInput_us = 0              // raw PWM µs from broadcast (servo2 = SRV3 / GP10 in direct mode)
    let inputChannel = 0              // slave: selected input CH index (0-based)

    const slaveInputChannels = Array.from({ length: 24 }, (_, i) => `CH ${i + 1}`)

    // ─── Program Definitions ───
    interface SeqEvent {
        id: number; type: string; typeName: string; params: Record<string, number>
    }

    type ChannelMode = 'events' | 'group'

    interface ChannelState {
        enabled: boolean
        mode: ChannelMode
        groupIndex: number  // which group (0-based index into groups[])
        preset: string
        events: SeqEvent[]
        brightness: number
    }

    interface Program {
        name: string
        bandMin_us: number      // PWM range low  (1000-2000 µs)
        bandMax_us: number      // PWM range high (1000-2000 µs)
        channels: ChannelState[]
        groupPolicies: GroupPolicy[]  // one per group
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
            createProgramWithBand('Off', bands[0].min, bands[0].max, true, ['off']),
            createProgramWithBand('Flight', bands[1].min, bands[1].max, false, ['off']),
            createProgramWithBand('Landing', bands[2].min, bands[2].max, false, ['on']),
        ]
    })()
    let activeProgram = 1
    let nextEventId = 1

    function createProgramWithBand(name: string, min_us: number, max_us: number, allDisabled = false, groupPolicies: GroupPolicy[] = []): Program {
        return {
            name,
            bandMin_us: min_us,
            bandMax_us: max_us,
            channels: Array.from({ length: CHANNEL_COUNT }, () => ({
                enabled: !allDisabled,
                mode: 'events' as ChannelMode,
                groupIndex: 0,
                preset: 'Custom',
                events: [],
                brightness: 100,
            })),
            groupPolicies: [...groupPolicies],
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
        const defaultPolicies = groups.map(() => 'off' as GroupPolicy)
        const newProg = createProgramWithBand(`Program ${programs.length + 1}`, newMin, newMax, false, defaultPolicies)
        // Sync channel modes + groupIndex from existing programs
        if (programs.length > 0) {
            const ref = programs[0]
            for (let i = 0; i < CHANNEL_COUNT; i++) {
                newProg.channels[i].mode = ref.channels[i].mode
                newProg.channels[i].groupIndex = ref.channels[i].groupIndex
            }
        }
        programs = [...programs, newProg]
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
        SendCommand(`light:seq.clear ${c}`)
        for (const evt of prog.channels[chIdx].events) {
            const p = evt.params
            switch (evt.type) {
                case 'on':      SendCommand(`light:seq.add ${c} on ${p.duration} ${p.brightness} ${p.pwmDuty ?? 0}`); break
                case 'off':     SendCommand(`light:seq.add ${c} off ${p.duration}`); break
                case 'flash':   SendCommand(`light:seq.add ${c} flash ${p.interval} ${p.duration} ${p.brightness} ${p.duty ?? 50}`); break
                case 'fadein':  SendCommand(`light:seq.add ${c} fadein ${p.duration} ${p.brightness}`); break
                case 'fadeout': SendCommand(`light:seq.add ${c} fadeout ${p.duration} ${p.brightness}`); break
                case 'fading':  SendCommand(`light:seq.add ${c} fading ${p.cycle} ${p.duration} ${p.min ?? 0} ${p.max ?? 100}`); break
                case 'beacon':  SendCommand(`light:seq.add ${c} beacon ${p.cycle} ${p.duration} ${p.flashPct ?? 15} ${p.max ?? 100} ${p.min ?? 0}`); break
            }
        }
        SendCommand(`light:seq.start ${c}`)
    }
    function stopChannel(chIdx: number) { SendCommand(`light:seq.stop ${ch(chIdx)}`) }

    function playChannel(chIdx: number) {
        sendChannelSeq(chIdx); channelPlaying[chIdx] = true; channelPlaying = channelPlaying
    }
    function stopChannelPlay(chIdx: number) {
        stopChannel(chIdx); channelPlaying[chIdx] = false; channelPlaying = channelPlaying
    }

    function playProgram() {
        prog.channels.forEach((c, i) => {
            if (!c.enabled) return
            if (c.mode === 'events' && c.events.length > 0) sendChannelSeq(i)
        })
        // Per-group landing policies: deploy or retract
        for (let gi = 0; gi < groups.length; gi++) {
            const policy = prog.groupPolicies[gi] ?? 'off'
            if (policy === 'on') deployGroup(gi)
            else if (policy === 'off') retractGroup(gi)
            // 'gear' — controlled by HubFX, no explicit action here
        }
        prog.playing = true; programs = programs
    }
    function stopProgram() {
        prog.channels.forEach((_, i) => { SendCommand(`light:seq.stop ${ch(i)}`); channelPlaying[i] = false })
        prog.playing = false; channelPlaying = channelPlaying; programs = programs
    }

    // ─── Cross-program sync (channel mode + group designation is global) ───
    function setChannelMode(chIdx: number, mode: ChannelMode) {
        for (const p of programs) p.channels[chIdx].mode = mode
        programs = programs
    }
    function setChannelGroup(chIdx: number, groupIdx: number) {
        for (const p of programs) p.channels[chIdx].groupIndex = groupIdx
        programs = programs
    }

    function enableChannel(chIdx: number) { SendCommand(`light:enable ${ch(chIdx)}`); prog.channels[chIdx].enabled = true; programs = programs }
    function disableChannel(chIdx: number) { SendCommand(`light:disable ${ch(chIdx)}`); prog.channels[chIdx].enabled = false; programs = programs }

    function setDirectBrightness(chIdx: number) {
        SendCommand(`light:led ${ch(chIdx)} ${prog.channels[chIdx].brightness}`)
    }
    function scheduleDirectBrightnessPush(chIdx: number) {
        scheduleLivePush(`led.${chIdx}`, () => `light:led ${ch(chIdx)} ${prog.channels[chIdx].brightness}`)
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

    // ─── Landing Groups ───
    interface ServoBindingState {
        servo: number      // servo id (1-3)
        // Servo configuration (always visible)
        servoPulse_us: number
        servoMin_us: number
        servoMax_us: number
        servoSpeed: number
        servoAccel: number
        servoDecel: number
        servoReversed: boolean
    }

    interface LandingGroupState {
        name: string
        binding: ServoBindingState | null   // at most one servo binding per group
    }

    function defaultBinding(servoId: number = 2): ServoBindingState {
        return {
            servo: servoId,
            servoPulse_us: 1500, servoMin_us: 500, servoMax_us: 2500, servoSpeed: 4000, servoAccel: 8000, servoDecel: 8000, servoReversed: false,
        }
    }

    let groups: LandingGroupState[] = [
        { name: 'Group 1', binding: null },
    ]

    function addGroup() {
        groups = [...groups, { name: `Group ${groups.length + 1}`, binding: null }]
        // Add a policy entry for each existing program
        for (const p of programs) p.groupPolicies.push('off')
        programs = programs
    }
    function removeGroup(gi: number) {
        groups = groups.filter((_, i) => i !== gi)
        // Remove policy entry and fix channel groupIndex references
        for (const p of programs) {
            p.groupPolicies.splice(gi, 1)
            for (const c of p.channels) {
                if (c.mode === 'group') {
                    if (c.groupIndex === gi) { c.mode = 'events' as ChannelMode; c.groupIndex = 0 }
                    else if (c.groupIndex > gi) c.groupIndex--
                }
            }
        }
        programs = programs
    }
    function addBinding(gi: number) {
        const nextServo = availableServos[0]?.id ?? 1
        groups[gi].binding = defaultBinding(nextServo)
        groups = groups
    }
    function removeBinding(gi: number) {
        groups[gi].binding = null
        groups = groups
    }

    // ─── Landing group commands ───

    /** Build a channel bitmask for a group (bit0=ch1 .. bit7=ch8). */
    function groupChannelMask(gi: number): number {
        let mask = 0
        const ref = programs.length > 0 ? programs[0] : null
        if (!ref) return 0
        for (let ci = 0; ci < ref.channels.length; ci++) {
            if (ref.channels[ci].mode === 'group' && ref.channels[ci].groupIndex === gi) {
                mask |= (1 << ci)
            }
        }
        return mask
    }

    function deployGroup(gi: number) {
        const mask = groupChannelMask(gi)
        const b = groups[gi].binding
        if (mask === 0 && !b) return  // nothing to deploy
        const slot = gi + 1
        const servoId = b ? b.servo : 0
        const channels = groupMemberChannels(gi).join(',') || '1'  // fallback
        // 1. Apply servo config if binding exists
        if (b) {
            let cmd = `light:servo.config ${b.servo} ${b.servoMin_us} ${b.servoMax_us}`
            cmd += ` ${b.servoSpeed} ${b.servoAccel} ${b.servoDecel}`
            if (b.servoReversed) cmd += ' rev'
            SendCommand(cmd)
        }
        // 2. Bind group: slot, servo (0=none), channels, brightness
        SendCommand(`light:landing.bind ${slot} ${servoId} ${channels} 100`)
        // 3. Deploy
        SendCommand(`light:landing.deploy ${slot}`)
    }
    function retractGroup(gi: number) {
        const mask = groupChannelMask(gi)
        if (mask === 0 && !groups[gi].binding) return
        SendCommand(`light:landing.retract ${gi + 1}`)
    }
    function deployAllGroups() { for (let gi = 0; gi < groups.length; gi++) deployGroup(gi) }
    function retractAllGroups() { for (let gi = 0; gi < groups.length; gi++) retractGroup(gi) }

    // ─── Live servo positions (per-servo µs, indexed 0..2 = Servo 1..3) ───
    // Mirrors GearControl's liveDoor_us pattern: drives the inline live-bar in
    // each landing-light binding so the operator sees the servo's actual
    // position vs. its configured [min,max] range without leaving the tab.
    let liveServo_us: number[] = [0, 0, 0]

    // Map a live PWM µs reading to a 0–100% bar position within [openUs, closeUs].
    // open and close may be inverted (close < open) — we normalize so the bar
    // grows as the servo moves toward whichever endpoint is "more open" visually.
    function servoPct(value: number, openUs: number, closeUs: number): number {
        if (!value) return 0
        const lo = Math.min(openUs, closeUs)
        const hi = Math.max(openUs, closeUs)
        if (hi <= lo) return 0
        const pct = ((value - lo) / (hi - lo)) * 100
        return Math.max(0, Math.min(100, pct))
    }

    // Build / schedule / apply for a binding's servo config. The shared
    // ServoCalibrationDialog still owns full calibration UX (Rule 28); these
    // helpers drive the inline Min/Max/Speed/Reversed editors that mirror
    // GearControl's door-servo block.
    function buildBindingServoCmd(gi: number): string | null {
        const b = groups[gi]?.binding
        if (!b) return null
        if (b.servoMin_us < 500 || b.servoMax_us > 2500 || b.servoMin_us >= b.servoMax_us) return null
        if (b.servoSpeed < 100 || b.servoSpeed > 10000) return null
        let cmd = `light:servo.config ${b.servo} ${b.servoMin_us} ${b.servoMax_us} ${b.servoSpeed} ${b.servoAccel} ${b.servoDecel}`
        if (b.servoReversed) cmd += ' rev'
        return cmd
    }
    function scheduleBindingServoPush(gi: number) {
        scheduleLivePush(`servo:group${gi}`, () => buildBindingServoCmd(gi))
    }
    function applyBindingServoConfig(gi: number) {
        const cmd = buildBindingServoCmd(gi)
        if (cmd) SendCommand(cmd)
    }

    // ─── Servo calibration (shared ServoCalibrationDialog — see Rule 28) ───
    // Per Rule 28, all per-board servo setup uses the same dialog as
    // GearControl: live-jog position throttled to 30ms, debounced cfg push.
    // The dialog widens the on-board PWM limits while open so the user can
    // probe true end-stops; Save commits the narrow [min,max] range and
    // calls onCalibApply with the final config.
    let calibDialogOpen = false
    let calibServoId = 0
    let calibServoName = ''
    let calibTargetGroup = -1   // group index whose binding gets updated on Save
    let calibInit = { min_us: 500, max_us: 2500, speed: 4000, accel: 0, decel: 0, reversed: false }

    function openServoCalibration(gi: number) {
        const b = groups[gi]?.binding
        if (!b) return
        calibServoId = b.servo
        calibServoName = `${groups[gi].name} · Servo ${b.servo}`
        calibInit = {
            min_us: b.servoMin_us, max_us: b.servoMax_us,
            speed:  b.servoSpeed,  accel:  b.servoAccel, decel: b.servoDecel,
            reversed: b.servoReversed,
        }
        calibTargetGroup = gi
        calibDialogOpen = true
    }

    function onCalibApply(cfg: { min_us: number; max_us: number; speed: number; accel: number; decel: number; reversed: boolean }) {
        if (calibTargetGroup < 0 || calibTargetGroup >= groups.length) return
        const b = groups[calibTargetGroup].binding
        if (!b) return
        b.servoMin_us  = cfg.min_us
        b.servoMax_us  = cfg.max_us
        b.servoSpeed   = cfg.speed
        b.servoAccel   = cfg.accel
        b.servoDecel   = cfg.decel
        b.servoReversed = cfg.reversed
        // Re-clamp current position into the new bounds so the slider stays valid.
        b.servoPulse_us = Math.max(b.servoMin_us, Math.min(b.servoMax_us, b.servoPulse_us))
        groups = groups
    }

    // ─── Group member channels (reactive) ───
    // Returns the list of LED channel numbers (1-based) assigned to a given group
    function groupMemberChannels(gi: number): number[] {
        // Channel mode/groupIndex is global across programs, so just read from the first
        const ref = programs.length > 0 ? programs[0] : null
        if (!ref) return []
        const members: number[] = []
        for (let ci = 0; ci < ref.channels.length; ci++) {
            if (ref.channels[ci].mode === 'group' && ref.channels[ci].groupIndex === gi) {
                members.push(ci + 1) // 1-based
            }
        }
        return members
    }

    // ─── Servo definitions ───
    // Non-slave: Servo 1 = input, Servos 2-3 available for landing lights
    // Slave: all 3 servos available
    $: availableServos = slaveMode
        ? [{ id: 1, name: 'Servo 1' }, { id: 2, name: 'Servo 2' }, { id: 3, name: 'Servo 3' }]
        : [{ id: 2, name: 'Servo 2' }, { id: 3, name: 'Servo 3' }]

    // ─── Master brightness ───
    let masterBrightness = 100
    function setMasterBrightness() { SendCommand(`light:brightness ${masterBrightness}`) }
    function scheduleMasterBrightnessPush() {
        scheduleLivePush('brightness', () => `light:brightness ${masterBrightness}`)
    }
    function allOff() { SendCommand('light:led.off 0') }

    // ─── Battery ───
    // Monitor is always on. Defaults match firmware (LiPo, auto-detect cells,
    // auto-cutoff ON — safer for a battery-powered prop).
    let batteryAutoCutoff   = true
    let batteryChemistry    = 'lipo'
    let batteryCellCount    = 0  // 0 = auto-detect
    let batteryVoltage_mV   = 0
    let batteryLowTriggered = false
    const batteryChemistries = ['lipo', 'liion', 'nimh']
    const chemistryLabels: Record<string, string> = { lipo: 'LiPo', liion: 'Li-Ion', nimh: 'NiMH' }
    // Cell voltage bounds (mV) — must match BatteryProfiles in battery_types.h
    // (fullCharge_mV, nominal_mV, low_mV, critical_mV).
    const cellVoltageBounds: Record<string, { min: number; max: number; nominal: number }> = {
        lipo:  { min: 3000, max: 4200, nominal: 3700 },
        liion: { min: 2800, max: 4200, nominal: 3600 },
        nimh:  { min:  900, max: 1400, nominal: 1200 },
    }
    const MIN_DETECT_mV = 3000
    $: inferredCellCount = (() => {
        if (batteryVoltage_mV < MIN_DETECT_mV) return 0
        const nominal = cellVoltageBounds[batteryChemistry]?.nominal ?? 3700
        const cells = Math.round(batteryVoltage_mV / nominal)
        return Math.min(6, Math.max(1, cells))
    })()
    $: effectiveCellCount = batteryCellCount > 0 ? batteryCellCount : inferredCellCount
    $: battMinV = (cellVoltageBounds[batteryChemistry]?.min ?? 3000) * effectiveCellCount
    $: battMaxV = (cellVoltageBounds[batteryChemistry]?.max ?? 4200) * effectiveCellCount
    $: batteryVolts = (batteryVoltage_mV / 1000).toFixed(2)
    $: batteryPct = (battMaxV > battMinV && batteryVoltage_mV > 0)
        ? Math.min(100, Math.max(0, Math.round((batteryVoltage_mV - battMinV) / (battMaxV - battMinV) * 100)))
        : 0
    $: batteryLow = batteryVoltage_mV > 0 && effectiveCellCount > 0 && batteryPct < 15

    function applyBattery() {
        const cells = batteryCellCount > 0 ? `cells:${batteryCellCount}` : 'auto'
        SendCommand(`light:battery ${batteryChemistry} ${cells}`)
        SendCommand(`light:battery.cutoff ${batteryAutoCutoff ? 'on' : 'off'}`)
    }

    // ─── Live status broadcast listener ───
    interface LightFxBroadcast {
        ledBrightness: number[]
        seqPlaying: boolean[]
        enabled: boolean[]
        servo0_us: number; servo1_us: number; servo2_us: number
        landingSlots: { phase: number; phaseName: string }[]
        masterBrightness: number
        battery_mV: number; cellCount: number; batteryPct: number
        batteryPresent: boolean
        batteryAutoCutoff: boolean; batteryLowTriggered: boolean
    }

    // Per-channel runtime state mirrored from the broadcast — drives the pip
    // strip in the top status card so the user sees instantly which LEDs are
    // active / disabled / running a sequence.
    let ledLive = Array.from({ length: CHANNEL_COUNT }, () => ({
        bri: 0, seq: false, enabled: true,
    }))

    function handleStatusBroadcast(data: LightFxBroadcast) {
        batteryVoltage_mV   = data.battery_mV
        batteryLowTriggered = data.batteryLowTriggered
        // Don't fight a user mid-edit on the auto-cutoff toggle
        if (!live.isPending('battery.cutoff')) {
            batteryAutoCutoff = data.batteryAutoCutoff
        }
        // Servo 2 (SRV3 / GP10) doubles as the RC PWM input on direct LightFX boards.
        if (data.servo2_us !== undefined) liveInput_us = data.servo2_us
        // Live servo positions for the inline binding bars (Servo 1..3).
        if (data.servo0_us !== undefined) liveServo_us[0] = data.servo0_us
        if (data.servo1_us !== undefined) liveServo_us[1] = data.servo1_us
        if (data.servo2_us !== undefined) liveServo_us[2] = data.servo2_us
        liveServo_us = liveServo_us
        // Mirror per-channel LED state into the pip strip
        for (let i = 0; i < CHANNEL_COUNT; i++) {
            ledLive[i] = {
                bri:     data.ledBrightness?.[i] ?? 0,
                seq:     data.seqPlaying?.[i]    ?? false,
                enabled: data.enabled?.[i]       ?? true,
            }
        }
        ledLive = ledLive
    }

    // ─── Active program inference (fold the live PWM into the configured bands) ───
    // Direct-mode only — slave mode lets HubFX pick the program.
    $: liveActiveProgram = (() => {
        if (slaveMode || liveInput_us <= 0) return -1
        for (let i = 0; i < programs.length; i++) {
            const p = programs[i]
            if (liveInput_us >= p.bandMin_us && liveInput_us <= p.bandMax_us) return i
        }
        return -1
    })()

    function configReload() { SendCommand('config.reload') }

    let boardDialogOpen = false

    onMount(() => {
        EventsOn('lightfx:status', handleStatusBroadcast)
    })
    onDestroy(() => {
        EventsOff('lightfx:status')
    })

    // ─── Live-push helper (Rule 24) ───
    const live = createLivePusher(SendCommand)
    const scheduleLivePush = live.schedule
    const pushStatus = live.status

    function buildBatteryCmd(): string {
        const cells = batteryCellCount > 0 ? `cells:${batteryCellCount}` : 'auto'
        return `light:battery ${batteryChemistry} ${cells}`
    }
    function buildBatteryCutoffCmd(): string {
        return `light:battery.cutoff ${batteryAutoCutoff ? 'on' : 'off'}`
    }
    function scheduleBatteryPush() {
        scheduleLivePush('battery', () => buildBatteryCmd())
    }
    function scheduleBatteryCutoffPush() {
        scheduleLivePush('battery.cutoff', () => buildBatteryCutoffCmd())
    }

    // ─── Save Config Dialog + Verification ───
    const lightVerifier = new LightConfigVerifier()
    let saveDialogOpen = false

    // Build a LightConfig snapshot from current UI state
    function buildLightConfig(): LightConfig {
        return {
            programs: programs.map(p => ({
                name: p.name,
                bandMin_us: p.bandMin_us,
                bandMax_us: p.bandMax_us,
                channels: p.channels.map(c => ({
                    enabled: c.enabled,
                    mode: c.mode as 'events' | 'group',
                    groupIndex: c.groupIndex,
                    events: c.events.map(e => ({
                        id: e.id,
                        type: e.type,
                        params: { ...e.params },
                    })),
                    // Tab edits the lightfx-side board only; hubfx channel
                    // defs round-trip through parser/generator without UI.
                    board: 'lightfx' as const,
                })),
                groupPolicies: [...p.groupPolicies],
            })),
            groups: groups.map((g, gi) => ({
                name: g.name,
                memberChannels: groupMemberChannels(gi),
                hubfxMemberChannels: [],     // tab is lightfx-side only
                servoBoard: 'lightfx' as const,
                servoBindings: g.binding ? [{
                    servo: g.binding.servo,
                    servoMin_us: g.binding.servoMin_us,
                    servoMax_us: g.binding.servoMax_us,
                    servoSpeed: g.binding.servoSpeed,
                    servoAccel: g.binding.servoAccel,
                    servoDecel: g.binding.servoDecel,
                    servoReversed: g.binding.servoReversed,
                }] : [],
            })),
            masterBrightness,
            battery: {
                autoCutoff: batteryAutoCutoff,
                chemistry: batteryChemistry,
                cellCount: batteryCellCount,
            },
            channelCount: CHANNEL_COUNT,
            pwmMin: PWM_MIN,
            pwmMax: PWM_MAX,
            hubAvailable: isHubFX || slaveMode,
        }
    }

    // Push a parsed LightConfig back into the tab's reactive state so the UI
    // reflects an imported configuration. Programs are rebuilt from scratch;
    // groups are mapped back to the tab's richer LandingGroupState shape.
    function applyLightConfig(cfg: LightConfig) {
        masterBrightness = cfg.masterBrightness
        if (cfg.battery) {
            batteryAutoCutoff = cfg.battery.autoCutoff
            batteryChemistry  = cfg.battery.chemistry || 'lipo'
            batteryCellCount  = cfg.battery.cellCount ?? 0
        }

        // Groups: rebuild from parsed shape; keep member-channel mapping implicit
        // (channels below reference group by index via mode='group').
        groups = cfg.groups.map(g => {
            const b = g.servoBindings[0]
            return {
                name: g.name,
                binding: b ? {
                    servo: b.servo,
                    servoPulse_us: Math.round((b.servoMin_us + b.servoMax_us) / 2),
                    servoMin_us: b.servoMin_us,
                    servoMax_us: b.servoMax_us,
                    servoSpeed: b.servoSpeed,
                    servoAccel: b.servoAccel,
                    servoDecel: b.servoDecel,
                    servoReversed: b.servoReversed,
                } : null,
            }
        })
        if (groups.length === 0) groups = [{ name: 'Group 1', binding: null }]

        programs = cfg.programs.map(p => {
            const channels: ChannelState[] = Array.from({ length: CHANNEL_COUNT }, (_, ci) => {
                const src = p.channels[ci]
                return {
                    enabled: src?.enabled ?? false,
                    mode: (src?.mode ?? 'events') as ChannelMode,
                    groupIndex: src?.groupIndex ?? 0,
                    preset: 'Custom',
                    events: (src?.events ?? []).map(e => ({
                        id: nextEventId++,
                        type: e.type,
                        typeName: eventTypes.find(et => et.value === e.type)?.name ?? e.type,
                        params: { ...e.params },
                    })),
                    brightness: 100,
                }
            })
            // Backfill members declared via landing_groups.memberChannels — those
            // channels should come back as mode='group' after import.
            for (let gi = 0; gi < cfg.groups.length; gi++) {
                for (const chNum of cfg.groups[gi].memberChannels) {
                    if (chNum >= 1 && chNum <= CHANNEL_COUNT) {
                        channels[chNum - 1].mode = 'group'
                        channels[chNum - 1].groupIndex = gi
                        channels[chNum - 1].enabled = true
                    }
                }
            }
            return {
                name: p.name,
                bandMin_us: p.bandMin_us,
                bandMax_us: p.bandMax_us,
                channels,
                groupPolicies: [...p.groupPolicies] as GroupPolicy[],
                playing: false,
            }
        })
        if (programs.length === 0) {
            programs = [createProgramWithBand('Program 1', PWM_MIN, PWM_MAX, false, groups.map(() => 'off' as GroupPolicy))]
        }
        activeProgram = 0
        console.log(`[LightFX] applyState → programs×${programs.length}, groups×${groups.length}, masterBrightness=${masterBrightness}, battery={chem:${batteryChemistry},cells:${batteryCellCount},cutoff:${batteryAutoCutoff}}`)
    }

    const lightDriver: BoardConfigDriver<LightConfig> = {
        boardType: 'lightfx',
        boardLabel,
        buildState: buildLightConfig,
        generateYaml: (s) => generateLightYaml(s, false),
        parseYaml:    (t) => parseLightYaml(t, false, CHANNEL_COUNT, PWM_MIN, PWM_MAX, isHubFX || slaveMode),
        applyState:   applyLightConfig,
        verify:       (s) => lightVerifier.verify(s),
    }

    onMount(() => autoLoadOnConnect(lightDriver, ['lightfx']))

    // Live verification — re-run whenever programs, groups, or brightness change
    let liveResult: VerifyResult = EMPTY_RESULT
    $: {
        // Any reactive read of programs / groups / masterBrightness triggers re-verify
        void programs; void groups; void masterBrightness
        liveResult = lightVerifier.verify(buildLightConfig())
    }

    // Severity lookup — reactive declaration so Svelte tracks liveResult as a dependency.
    // When liveResult changes, sev is reassigned (new function ref), causing all template
    // expressions using sev(...) to re-evaluate (clearing stale highlights).
    let sev: (path: string) => string | null
    $: sev = (() => {
        void liveResult  // reactive dependency
        return (path: string): string | null => lightVerifier.severityForPath(path)
    })()

    function openSaveDialog() { saveDialogOpen = true }
</script>

<div class="tab-root">
    <!-- ═══ Title Bar ═══ -->
    <div class="tab-title-bar">
        <h2>{boardLabel}</h2>
        <div class="title-actions">
            <button class="small" on:click={configReload} disabled={controlsDisabled}
                    title="Reload /lightfx.yaml from flash">↻ Reload</button>
            <button class="small" on:click={openSaveDialog} disabled={controlsDisabled}
                    title="Verify and save config to flash">💾 Save…</button>
            <button class="small" on:click={() => boardDialogOpen = true}
                    title="Show LightFX board pin assignments">🗺 View Diagram</button>
            {#if liveResult.counts.error > 0}
                <span class="verify-badge error" title="{liveResult.counts.error} error(s) — see Save dialog">{liveResult.counts.error} ✕</span>
            {:else if liveResult.counts.warning > 0}
                <span class="verify-badge warning" title="{liveResult.counts.warning} warning(s)">{liveResult.counts.warning} ⚠</span>
            {/if}
        </div>
    </div>

    {#if isDirect && !warningDismissed}
        <div class="direct-warning">
            <span class="direct-warning-text">⚠ Direct connection — settings will not persist as slave configuration.
            When configured as slave, manage settings via HubFX.</span>
            <button class="small" on:click={() => dismissWarning(false)}>Accept</button>
            <button class="small" on:click={() => dismissWarning(true)} title="Hide this warning permanently">Don't show again</button>
        </div>
    {/if}

    <!-- ═══ Scrollable Content ═══ -->
    <div class="tab-scroll">
        <div class="content-wrap" class:controls-disabled={controlsDisabled}>
            {#if controlsDisabled}
                <div class="disabled-overlay"><span>Slave mode — connect HubFX to control channels</span></div>
            {/if}

            <!-- ═══ Top spanning section: Input + Master / All Off / Channel pips ═══ -->
            <section class="card top-span">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83"/><circle cx="12" cy="12" r="3"/></svg> LightFX Status</h3>
                    {#if liveActiveProgram >= 0}
                        <span class="header-tag ok">▶ {programs[liveActiveProgram]?.name ?? `Program ${liveActiveProgram + 1}`}</span>
                    {:else if slaveMode}
                        <span class="header-tag">via HubFX · CH{inputChannel + 1}</span>
                    {:else if liveInput_us > 0}
                        <span class="header-tag warn">No band match @ {liveInput_us} µs</span>
                    {:else}
                        <span class="header-tag">— No signal</span>
                    {/if}
                </div>

                <div class="top-grid">
                    <!-- Left: Input bar with program-band overlay -->
                    <div class="lfx-input-block">
                        <div class="lfx-input-header">
                            <span class="lfx-input-label">
                                Input
                                <span class="lfx-input-pin dim">
                                    {#if slaveMode}via HubFX · {slaveInputChannels[inputChannel]}{:else}Servo 3 · GP10 · RC PWM{/if}
                                </span>
                            </span>
                        </div>

                        <!-- PWM band track with one tinted region per program + live cursor -->
                        <div class="lfx-band-track" title="PWM band map. Live cursor shows the current pulse width.">
                            {#each programs as p, i}
                                <div class="lfx-band-region"
                                     class:active={i === liveActiveProgram}
                                     style="left: {((p.bandMin_us - PWM_MIN) / (PWM_MAX - PWM_MIN)) * 100}%; width: {((p.bandMax_us - p.bandMin_us) / (PWM_MAX - PWM_MIN)) * 100}%;">
                                    <span class="lfx-band-label">{p.name}</span>
                                </div>
                            {/each}
                            {#if !slaveMode && liveInput_us >= PWM_MIN && liveInput_us <= PWM_MAX}
                                <div class="lfx-band-cursor"
                                     style="left: {((liveInput_us - PWM_MIN) / (PWM_MAX - PWM_MIN)) * 100}%"></div>
                            {/if}
                        </div>

                        <div class="lfx-input-readout">
                            <span class="lfx-input-pulse">
                                {#if slaveMode}—{:else if liveInput_us > 0}{liveInput_us} µs{:else}— µs{/if}
                            </span>
                            {#if slaveMode}
                                <span class="lfx-input-thr-label">channel</span>
                                <select bind:value={inputChannel} class="field-input narrow"
                                        title="Which HubFX-relayed input channel drives this board">
                                    {#each slaveInputChannels as ch, i}
                                        <option value={i}>{ch}</option>
                                    {/each}
                                </select>
                            {:else}
                                <span class="lfx-input-thr-label">range</span>
                                <span class="dim">{PWM_MIN}–{PWM_MAX} µs</span>
                            {/if}
                            <span class="push-hint" title="Input is read-only — bands are persisted via Save.">⏷ save-only</span>
                        </div>
                    </div>

                    <!-- Right: Master brightness + All Off + per-channel pip strip -->
                    <div class="lfx-master-block">
                        <div class="lfx-master-row">
                            <span class="lfx-master-label">Master Brightness</span>
                            <input type="range" bind:value={masterBrightness} min="0" max="100" class="slider lfx-master-slider"
                                   on:input={scheduleMasterBrightnessPush}
                                   title="Master LED brightness 0–100%. Live-pushed (~350ms)." />
                            <span class="lfx-master-value">{masterBrightness}%</span>
                            <span class="push-badge push-{$pushStatus['brightness'] || ''}" title="Live-push status (~350ms)">{pushBadgeText($pushStatus['brightness'])}</span>
                            <button class="small primary" on:click={setMasterBrightness}
                                    title="Force resend `brightness` now">Set</button>
                            <button class="small danger" on:click={allOff}
                                    title="Turn off every LED channel immediately">All Off</button>
                        </div>

                        <!-- Per-channel pip strip (8 channels) -->
                        <div class="lfx-channel-strip">
                            {#each ledLive as st, i}
                                <div class="lfx-pip"
                                     class:pip-on={st.enabled && st.bri > 0}
                                     class:pip-seq={st.enabled && st.seq}
                                     class:pip-disabled={!st.enabled}>
                                    <span class="pip-dot"></span>
                                    <span class="pip-name">CH{i + 1}</span>
                                    {#if !st.enabled}
                                        <span class="pip-badge disabled">OFF</span>
                                    {:else if st.seq}
                                        <span class="pip-badge seq">▶</span>
                                    {:else if st.bri > 0}
                                        <span class="pip-badge on">{st.bri}</span>
                                    {/if}
                                </div>
                            {/each}
                        </div>
                    </div>
                </div>
            </section>

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
                        <div class="band-row" class:verify-error={sev(`programs[${activeProgram}].band`) === 'error'}
                             class:verify-warn={sev(`programs[${activeProgram}].band`) === 'warning'}
                             class:verify-ok={sev(`programs[${activeProgram}].band`) === null}>
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
                                 class:ch-group={channel.mode === 'group'}
                                 class:verify-error={sev(`programs[${activeProgram}].channels[${idx}]`) === 'error'}
                                 class:verify-warn={sev(`programs[${activeProgram}].channels[${idx}]`) === 'warning'}
                                 class:verify-ok={channel.enabled && sev(`programs[${activeProgram}].channels[${idx}]`) === null}>
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
                                                on:click={() => setChannelMode(idx, 'events')}
                                                disabled={!channel.enabled}>Events</button>
                                        <button class="mode-chip" class:active={channel.mode === 'group'}
                                                on:click={() => setChannelMode(idx, 'group')}
                                                disabled={!channel.enabled}>Group</button>
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
                                        <select class="ch-group-select" bind:value={channel.groupIndex}
                                                on:change={() => setChannelGroup(idx, channel.groupIndex)}
                                                disabled={!channel.enabled || groups.length === 0}>
                                            {#each groups as g, gi}
                                                <option value={gi}>{g.name}</option>
                                            {/each}
                                        </select>
                                        {#if groups.length === 0}
                                            <span class="ch-summary group-hint">No groups defined</span>
                                        {/if}
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
                                                   min="0" max="100" class="slider" style="width: 100px"
                                                   on:input={() => scheduleDirectBrightnessPush(idx)}
                                                   title="Direct LED brightness 0–100%. Live-pushed (~350ms)." />
                                            <span class="slider-val-sm">{channel.brightness}%</span>
                                            <span class="push-badge push-{$pushStatus['led.' + idx] || ''}" title="Live-push status (~350ms)">{pushBadgeText($pushStatus['led.' + idx])}</span>
                                            <button class="tiny" on:click={() => setDirectBrightness(idx)}
                                                    title="Force resend led {idx + 1} now">Set</button>
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
                            </div>
                        {/each}
                    </section>

                    <!-- ── Battery ── -->
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="1" y="6" width="18" height="12" rx="2" ry="2"/><line x1="23" y1="13" x2="23" y2="11"/></svg> Battery</h3>
                        </div>

                        <!-- Voltage display -->
                        <div class="batt-display">
                            <div class="batt-bar-track">
                                <div class="batt-bar-fill" class:low={batteryLow}
                                     style="width: {batteryPct}%"></div>
                            </div>
                            <div class="batt-info">
                                <span class="batt-voltage" class:low={batteryLow}>
                                    {batteryVoltage_mV > 0 ? `${batteryVolts} V` : '— V'}
                                </span>
                                {#if batteryVoltage_mV > 0}
                                    <span class="batt-pct" class:low={batteryLow}>{batteryPct}%</span>
                                {/if}
                                {#if batteryLow}
                                    <span class="batt-warn">⚠ LOW</span>
                                {/if}
                                {#if batteryLowTriggered}
                                    <span class="batt-warn" title="Low-voltage cutoff has fired — programs auto-stop until cleared">CUTOFF FIRED</span>
                                {/if}
                            </div>
                        </div>

                        <div class="form-row">
                            <!-- svelte-ignore a11y-label-has-associated-control -->
                            <label class="toggle">
                                <input type="checkbox" bind:checked={batteryAutoCutoff}
                                       on:change={scheduleBatteryCutoffPush}
                                       disabled={controlsDisabled} />
                                <span class="toggle-text" title="When ON, programs auto-stop on low voltage (cell-aware threshold).">Auto-Cutoff on Low Voltage</span>
                            </label>
                            <span class="push-badge push-{$pushStatus['battery.cutoff'] || ''}" style="margin-left: auto" title="Live-push status (~350ms)">{pushBadgeText($pushStatus['battery.cutoff'])}</span>
                            <button class="small primary"
                                    disabled={controlsDisabled}
                                    title="Force resend `battery` + `battery.cutoff` now"
                                    on:click={applyBattery}>Apply</button>
                        </div>

                        <div class="form-row">
                            <div class="setting">
                                <span class="field-label">Chemistry</span>
                                <select bind:value={batteryChemistry} class="field-input"
                                        on:change={scheduleBatteryPush}
                                        title="Battery chemistry — sets per-cell voltage thresholds. Live-pushed (~350ms)."
                                        disabled={controlsDisabled}>
                                    {#each batteryChemistries as chem}
                                        <option value={chem}>{chemistryLabels[chem]}</option>
                                    {/each}
                                </select>
                            </div>
                            <div class="setting" style="margin-left: 16px">
                                <span class="field-label">Cell Count</span>
                                <div class="setting-input">
                                    <input type="number" bind:value={batteryCellCount}
                                           on:input={scheduleBatteryPush}
                                           class="field-input narrow" min="0" max="6" step="1"
                                           title="0 = auto-detect from voltage. Live-pushed (~350ms)."
                                           disabled={controlsDisabled} />
                                    <span class="unit">{batteryCellCount === 0 ? 'auto' : 'S'}</span>
                                </div>
                            </div>
                            <span class="push-badge push-{$pushStatus['battery'] || ''}" style="margin-left: auto" title="Live-push status (~350ms)">{pushBadgeText($pushStatus['battery'])}</span>
                            {#if batteryCellCount === 0}
                                <span class="field-hint" style="flex-basis: 100%; margin-top: 4px;">
                                    {#if inferredCellCount > 0}
                                        Auto-detect: <strong>{inferredCellCount}S</strong>
                                        ({(batteryVoltage_mV / inferredCellCount / 1000).toFixed(2)} V/cell)
                                    {:else}
                                        Auto-detect on connect
                                    {/if}
                                </span>
                            {/if}
                        </div>
                    </section>
                </div>

                <!-- ═══════ RIGHT: Landing Groups ═══════ -->
                <div class="col">
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18h6"/><path d="M10 22h4"/><path d="M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z"/></svg> Landing Light Groups</h3>
                            <div class="header-actions">
                                <button class="small" on:click={addGroup}>+ Group</button>
                                <button class="small primary" disabled={controlsDisabled || groups.length === 0}
                                        on:click={() => deployAllGroups()}>Deploy All</button>
                                <button class="small" disabled={controlsDisabled || groups.length === 0}
                                        on:click={() => retractAllGroups()}>Retract All</button>
                            </div>
                        </div>

                        {#if !slaveMode}
                            <p class="slot-hint">Servo 1 = Input. Servos 2–3 available for bindings.</p>
                        {/if}

                        {#if groups.length === 0}
                            <p class="empty-hint">No landing light groups. Click "+ Group" to create one.</p>
                        {/if}

                        {#each groups as group, gi}
                            <div class="group-card"
                                 class:verify-error={sev(`groups[${gi}]`) === 'error'}
                                 class:verify-warn={sev(`groups[${gi}]`) === 'warning'}>
                                <div class="group-header">
                                    <input type="text" bind:value={group.name} class="group-name-input"
                                           placeholder="Group name" disabled={controlsDisabled}
                                           on:input={() => { groups = groups }} />
                                    <button class="small danger" on:click={() => removeGroup(gi)}
                                            disabled={controlsDisabled}>✕</button>
                                </div>

                                <!-- Per-group policy for active program -->
                                <div class="group-policy-row">
                                    <span class="group-policy-label">{programs[activeProgram]?.name ?? 'Program'}</span>
                                    <select class="group-policy-select" bind:value={prog.groupPolicies[gi]}
                                            on:change={() => { programs = programs }}
                                            class:verify-warn={sev(`programs[${activeProgram}].groupPolicies[${gi}]`) === 'warning'}>
                                        <option value="off">Off</option>
                                        <option value="on">On</option>
                                        {#if isHubFX || slaveMode}
                                            <option value="gear">Gear</option>
                                        {/if}
                                    </select>
                                    <button class="small" on:click={() => deployGroup(gi)}
                                            disabled={controlsDisabled}>Deploy</button>
                                    <button class="small" on:click={() => retractGroup(gi)}
                                            disabled={controlsDisabled}>Retract</button>
                                </div>

                                <!-- Member channels -->
                                {#if groupMemberChannels(gi).length > 0}
                                    <div class="group-members">
                                        <span class="group-members-label">Channels:</span>
                                        {#each groupMemberChannels(gi) as chNum}
                                            <span class="group-member-chip">{chNum}</span>
                                        {/each}
                                    </div>
                                {:else}
                                    <div class="group-members empty">
                                        <span class="group-members-label">No channels assigned — set channels to "Group" mode on the left</span>
                                    </div>
                                {/if}

                                <div class="binding-list">
                                    {#if group.binding}
                                        <div class="binding-card"
                                             class:verify-error={sev(`groups[${gi}].bindings[0]`) === 'error'}
                                             class:verify-warn={sev(`groups[${gi}].bindings[0]`) === 'warning'}>
                                            <div class="binding-header">
                                                <span class="binding-label">Servo Binding</span>
                                                <button class="tiny danger" style="margin-left: auto"
                                                        on:click={() => removeBinding(gi)}
                                                        disabled={controlsDisabled}>✕ Remove</button>
                                            </div>

                                            <div class="binding-fields">
                                                <div class="field-col">
                                                    <span class="flbl">Servo</span>
                                                    <select bind:value={group.binding.servo} class="field-input servo-select"
                                                            disabled={controlsDisabled}
                                                            on:change={() => { groups = groups }}
                                                            class:verify-error={sev(`groups[${gi}].bindings[0].servo`) === 'error'}
                                                            class:verify-warn={sev(`groups[${gi}].bindings[0].servo`) === 'warning'}>
                                                        {#each availableServos as s}
                                                            <option value={s.id}>{s.name}</option>
                                                        {/each}
                                                    </select>
                                                </div>
                                            </div>

                                            <!-- ─── Servo Configuration (door-servo-style inline editor — Rule 28 calibrate dialog still wired) ─── -->
                                            <div class="servo-edit-block">
                                                <div class="servo-edit-header">
                                                    <span class="servo-edit-label">Servo {group.binding.servo}</span>
                                                    <span class="servo-edit-pin dim">{group.name}</span>
                                                    <span class="servo-live-value">{liveServo_us[group.binding.servo - 1] || '—'} µs</span>
                                                </div>
                                                <div class="servo-live-bar-track">
                                                    <div class="servo-live-bar-fill"
                                                         style="width: {servoPct(liveServo_us[group.binding.servo - 1], group.binding.servoMin_us, group.binding.servoMax_us)}%"></div>
                                                </div>
                                                <div class="servo-edit-grid">
                                                    <div class="servo-edit-field">
                                                        <span class="field-label">Min µs</span>
                                                        <input type="number" bind:value={group.binding.servoMin_us}
                                                               on:input={() => scheduleBindingServoPush(gi)}
                                                               class="field-input" min="500" max="2500" step="10"
                                                               title="Pulse width at one end-stop. Live-pushed (~350ms)."
                                                               disabled={controlsDisabled} />
                                                    </div>
                                                    <div class="servo-edit-field">
                                                        <span class="field-label">Max µs</span>
                                                        <input type="number" bind:value={group.binding.servoMax_us}
                                                               on:input={() => scheduleBindingServoPush(gi)}
                                                               class="field-input" min="500" max="2500" step="10"
                                                               title="Pulse width at the other end-stop. Must be > Min. Live-pushed (~350ms)."
                                                               disabled={controlsDisabled} />
                                                    </div>
                                                    <div class="servo-edit-field">
                                                        <span class="field-label">Speed µs/s</span>
                                                        <input type="number" bind:value={group.binding.servoSpeed}
                                                               on:input={() => scheduleBindingServoPush(gi)}
                                                               class="field-input" min="100" max="10000" step="100"
                                                               title="Cruise speed in µs per second. Live-pushed (~350ms)."
                                                               disabled={controlsDisabled} />
                                                    </div>
                                                </div>
                                                <div class="servo-edit-footer">
                                                    <!-- svelte-ignore a11y-label-has-associated-control -->
                                                    <label class="toggle">
                                                        <input type="checkbox" bind:checked={group.binding.servoReversed}
                                                               on:change={() => scheduleBindingServoPush(gi)}
                                                               disabled={controlsDisabled} />
                                                        <span class="toggle-text">Reversed (deploy @ min)</span>
                                                    </label>
                                                    <span class="push-badge push-{$pushStatus[`servo:group${gi}`] || ''}" title="Live-push (~350ms)">{pushBadgeText($pushStatus[`servo:group${gi}`])}</span>
                                                    <button class="small" style="margin-left: auto"
                                                            disabled={controlsDisabled}
                                                            title="Open the shared calibration dialog (live jog + debounced config push)"
                                                            on:click={() => openServoCalibration(gi)}>🎯 Calibrate…</button>
                                                    <button class="small"
                                                            disabled={controlsDisabled}
                                                            title="Force resend `servo.config` now"
                                                            on:click={() => applyBindingServoConfig(gi)}>Apply</button>
                                                </div>
                                            </div>
                                        </div>
                                    {:else}
                                        <button class="add-binding-btn" on:click={() => addBinding(gi)}
                                                disabled={controlsDisabled}>+ Add Servo Binding</button>
                                    {/if}
                                </div>
                            </div>
                        {/each}
                    </section>

                </div>
            </div>
        </div>
    </div>
</div>

<SaveConfigDialog
    driver={lightDriver}
    bind:open={saveDialogOpen}
    initialResult={liveResult}
    onSave={async (yaml) => {
        await UploadConfig(yaml)
        // Round-trip: re-download the just-saved YAML and apply it back so the
        // tab controls reflect the authoritative on-device state (and not the
        // optimistic in-memory snapshot). Same path as the connect-time
        // auto-loader, so console output / parse warnings stay consistent.
        await loadConfigFromDevice(lightDriver)
    }}
    onClose={() => { saveDialogOpen = false }}
/>

<LightFxBoardDialog
    bind:open={boardDialogOpen}
    {ledLive}
    {slaveMode}
    {liveInput_us}
    {batteryChemistry}
    {batteryCellCount}
    {batteryVoltage_mV}
    onClose={() => { boardDialogOpen = false }}
/>

<ServoCalibrationDialog
    bind:open={calibDialogOpen}
    prefix="light"
    servoId={calibServoId}
    servoName={calibServoName}
    min_us={calibInit.min_us}
    max_us={calibInit.max_us}
    speed={calibInit.speed}
    accel={calibInit.accel}
    decel={calibInit.decel}
    reversed={calibInit.reversed}
    supportsAccelDecel={true}
    onApply={onCalibApply}
    onClose={() => { calibDialogOpen = false }}
/>

<style>
    /* LightFxTab-specific — shared styles in style.css */

    /* ─── Verification badge ─── */
    .verify-badge {
        font-size: 11px;
        font-weight: 600;
        padding: 1px 7px;
        border-radius: 8px;
        line-height: 1.4;
    }
    .verify-badge.error {
        background: color-mix(in srgb, var(--error) 20%, transparent);
        color: var(--error);
    }
    .verify-badge.warning {
        background: color-mix(in srgb, var(--warning) 20%, transparent);
        color: var(--warning);
    }

    /* ─── Verification state highlights ─── */
    .verify-error {
        border-color: var(--error) !important;
        background: color-mix(in srgb, var(--error) 8%, var(--bg-surface)) !important;
        box-shadow: 0 0 0 1px color-mix(in srgb, var(--error) 35%, transparent);
    }
    .verify-warn {
        border-color: var(--warning) !important;
        box-shadow: 0 0 0 1px color-mix(in srgb, var(--warning) 35%, transparent);
    }
    .verify-ok {
        border-color: color-mix(in srgb, var(--ok, #4ec9b0) 50%, transparent) !important;
    }

    /* ─── Overrides ─── */
    .tab-root { min-height: 0; }
    .tab-title-bar { background: var(--bg-raised); }
    .field-label { white-space: nowrap; }

    .field-input {
        font-size: 12px;
        padding: 3px 6px;
        width: 72px;
    }

    .field-input.narrow { width: 60px; }
    .field-input.servo-select { width: 90px; }

    .dim { color: var(--text-dim); font-weight: 500; }

    /* ─── Title Actions (mirrors GearControlTab) ─── */
    .title-actions {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-left: auto;
    }

    /* ─── Direct-Mode Warning Banner ─── */
    .direct-warning {
        padding: 8px 14px;
        margin: 0 0 2px;
        border-radius: 4px;
        font-size: 12px;
        font-weight: 500;
        color: var(--warning, #d7ba7d);
        background: color-mix(in srgb, var(--warning, #d7ba7d) 8%, var(--bg-surface));
        border: 1px solid color-mix(in srgb, var(--warning, #d7ba7d) 30%, transparent);
        display: flex;
        align-items: center;
        gap: 12px;
    }
    .direct-warning-text { flex: 1; }
    .direct-warning button { flex-shrink: 0; }

    /* ─── Header Tag (status pill in card header) ─── */
    .header-tag {
        font-size: 10px;
        font-weight: 700;
        padding: 1px 8px;
        border-radius: 3px;
        white-space: nowrap;
        font-family: var(--font-mono);
        margin-left: auto;
    }
    .header-tag.ok    { color: var(--ok, #4ec9b0); background: color-mix(in srgb, var(--ok, #4ec9b0) 10%, transparent); }
    .header-tag.warn  { color: var(--warning, #d7ba7d); background: color-mix(in srgb, var(--warning, #d7ba7d) 10%, transparent); }
    .header-tag.error { color: var(--error); background: color-mix(in srgb, var(--error) 12%, transparent); }

    /* ─── Top Spanning Section (Input + Master / All-Off / Channel pips) ─── */
    .top-span { margin-bottom: 12px; }
    .top-grid {
        display: grid;
        grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
        gap: 14px;
        align-items: start;
    }
    @media (max-width: 900px) {
        .top-grid { grid-template-columns: 1fr; }
    }

    .lfx-input-block, .lfx-master-block {
        padding: 10px 12px;
        background: var(--bg-raised);
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
        border-radius: 4px;
    }

    /* ─── Input block: band map + live cursor ─── */
    .lfx-input-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 8px;
    }
    .lfx-input-label {
        font-family: var(--font-mono);
        font-size: 12px;
        font-weight: 700;
        color: var(--text);
        display: inline-flex;
        gap: 8px;
        align-items: center;
    }
    .lfx-input-pin { font-size: 10px; font-weight: 500; }

    .lfx-band-track {
        position: relative;
        height: 22px;
        background: var(--bg-input);
        border-radius: 4px;
        overflow: hidden;
        margin-bottom: 8px;
    }
    .lfx-band-region {
        position: absolute;
        top: 0; bottom: 0;
        background: color-mix(in srgb, var(--accent) 18%, transparent);
        border-right: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        display: flex;
        align-items: center;
        justify-content: center;
        overflow: hidden;
        transition: background 0.15s;
    }
    .lfx-band-region:last-child { border-right: none; }
    .lfx-band-region.active {
        background: color-mix(in srgb, var(--ok, #4ec9b0) 32%, transparent);
    }
    .lfx-band-label {
        font-family: var(--font-mono);
        font-size: 10px;
        font-weight: 700;
        color: var(--text-bright);
        padding: 0 4px;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }
    .lfx-band-cursor {
        position: absolute;
        top: -2px; bottom: -2px;
        width: 3px;
        background: var(--warning, #d7ba7d);
        box-shadow: 0 0 6px color-mix(in srgb, var(--warning, #d7ba7d) 60%, transparent);
        transform: translateX(-50%);
        z-index: 2;
        transition: left 0.15s;
    }

    .lfx-input-readout {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        font-family: var(--font-mono);
        font-size: 11px;
    }
    .lfx-input-pulse {
        font-weight: 700;
        color: var(--text);
        min-width: 70px;
    }
    .lfx-input-thr-label {
        color: var(--text-dim);
        font-size: 10px;
    }
    .push-hint {
        margin-left: auto;
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        font-style: italic;
    }

    /* ─── Master block: brightness slider + channel pips ─── */
    .lfx-master-row {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 10px;
    }
    .lfx-master-label {
        font-family: var(--font-mono);
        font-size: 12px;
        font-weight: 700;
        color: var(--text);
    }
    .lfx-master-slider {
        flex: 1;
        min-width: 120px;
    }
    .lfx-master-value {
        font-family: var(--font-mono);
        font-size: 11px;
        font-weight: 700;
        color: var(--accent);
        min-width: 40px;
        text-align: right;
    }

    .lfx-channel-strip {
        display: grid;
        grid-template-columns: repeat(4, minmax(0, 1fr));
        gap: 6px;
        padding: 8px 0 0;
        border-top: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }
    .lfx-pip {
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 5px 8px;
        border-radius: 4px;
        background: var(--bg-surface);
        border: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        transition: border-color 0.15s, background 0.15s;
    }
    .lfx-pip.pip-on  { border-color: color-mix(in srgb, var(--warning, #d7ba7d) 55%, transparent); }
    .lfx-pip.pip-seq { border-color: color-mix(in srgb, var(--ok, #4ec9b0) 60%, transparent); }
    .lfx-pip.pip-disabled { opacity: 0.45; }

    .pip-dot {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        flex-shrink: 0;
        background: var(--text-dim);
    }
    .pip-on  .pip-dot { background: var(--warning, #d7ba7d); }
    .pip-seq .pip-dot { background: var(--ok, #4ec9b0); animation: pulse 1s ease-in-out infinite; }

    .pip-name {
        font-size: 11px;
        font-weight: 600;
        color: var(--text);
        font-family: var(--font-mono);
    }

    .pip-badge {
        font-size: 9px;
        font-weight: 700;
        padding: 0 5px;
        border-radius: 2px;
        margin-left: auto;
        font-family: var(--font-mono);
    }
    .pip-badge.on { color: var(--warning, #d7ba7d); background: color-mix(in srgb, var(--warning, #d7ba7d) 15%, transparent); }
    .pip-badge.seq { color: var(--ok, #4ec9b0); background: color-mix(in srgb, var(--ok, #4ec9b0) 15%, transparent); }
    .pip-badge.disabled { color: var(--text-dim); background: color-mix(in srgb, var(--border) 50%, transparent); }

    @keyframes pulse {
        0%, 100% { opacity: 1; }
        50% { opacity: 0.3; }
    }

    /* ─── Program Bar ─── */
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

    .program-tabs { display: flex; gap: 4px; }

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

    .program-actions { display: flex; gap: 6px; align-items: center; }

    .group-policy-label {
        font-size: 10px;
        font-weight: 600;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .group-policy-select {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text-bright);
        font-size: 11px;
        font-family: var(--font-mono);
        font-weight: 600;
        padding: 3px 6px;
        cursor: pointer;
    }

    /* ─── Program Settings ─── */
    .program-settings {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 8px 12px;
    }

    /* ─── Channels ─── */
    .channels-card { padding: 10px 12px; }
    .channels-card .card-header { margin-bottom: 8px; }

    .channel {
        border: 1px solid color-mix(in srgb, var(--border) 60%, transparent);
        border-radius: 4px;
        margin-bottom: 4px;
        overflow: hidden;
        transition: opacity 0.15s;
    }

    .channel.ch-disabled { opacity: 0.5; }
    .channel.ch-group { border-color: color-mix(in srgb, var(--ok, #4ec9b0) 40%, transparent); }

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

    .ch-mode-chips { display: flex; gap: 2px; }

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

    .group-hint { color: var(--ok, #4ec9b0); font-weight: 600; }
    .ch-play { display: flex; gap: 2px; }

    /* ─── Channel Detail ─── */
    .ch-detail {
        padding: 6px 10px 8px 28px;
        background: var(--bg-surface);
        border-top: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }

    .detail-row {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 6px;
    }

    /* ─── Event List ─── */
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

    /* ─── Add Event Form ─── */
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

    .param-field { display: flex; flex-direction: column; gap: 2px; }

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

    .add-event-actions { display: flex; gap: 4px; margin-top: 6px; }

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

    /* ─── Landing Light Group Bindings ─── */
    .binding-list { display: flex; flex-direction: column; gap: 10px; }

    .binding-card {
        background: var(--bg-raised);
        border: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        border-radius: 5px;
        padding: 10px;
        transition: border-color 0.2s;
    }

    .binding-header {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 8px;
    }

    .binding-label {
        font-family: var(--font-mono);
        font-size: 12px;
        font-weight: 700;
        color: var(--text-bright);
    }

    .binding-fields {
        display: flex;
        align-items: flex-end;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 8px;
    }

    .field-col { display: flex; flex-direction: column; gap: 2px; }

    .flbl {
        font-size: 9px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .slot-hint {
        font-size: 11px;
        color: var(--text-dim);
        margin: 0 0 8px 0;
        font-style: italic;
    }

    /* ─── Landing Group Cards ─── */
    .group-card {
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 10px;
        margin-bottom: 10px;
        background: var(--bg-surface);
    }

    .group-header {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 8px;
    }

    .group-name-input {
        font-size: 12px;
        font-weight: 700;
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text-bright);
        padding: 3px 8px;
        width: 120px;
        font-family: var(--font-mono);
    }
    .group-name-input:focus { border-color: var(--border-focus); outline: none; }

    .group-policy-row {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 6px;
        padding: 4px 0;
    }

    .group-members {
        display: flex;
        align-items: center;
        gap: 4px;
        flex-wrap: wrap;
        margin-bottom: 8px;
        padding: 4px 0;
    }
    .group-members.empty { opacity: 0.6; }

    .group-members-label {
        font-size: 9px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
        white-space: nowrap;
    }

    .group-member-chip {
        font-family: var(--font-mono);
        font-size: 10px;
        font-weight: 700;
        color: var(--ok, #4ec9b0);
        background: color-mix(in srgb, var(--ok, #4ec9b0) 12%, transparent);
        border-radius: 3px;
        padding: 1px 6px;
    }

    .empty-hint {
        font-size: 11px;
        color: var(--text-dim);
        text-align: center;
        padding: 16px;
        font-style: italic;
    }

    .add-binding-btn {
        width: 100%;
        text-align: center;
        border-style: dashed;
        color: var(--text-dim);
        font-size: 10px;
        padding: 5px 8px;
        margin-top: 6px;
    }
    .add-binding-btn:hover { color: var(--accent); border-color: var(--accent); }

    .ch-group-select {
        background: var(--bg-input);
        border: 1px solid color-mix(in srgb, var(--ok, #4ec9b0) 40%, transparent);
        border-radius: 3px;
        color: var(--ok, #4ec9b0);
        font-size: 10px;
        font-family: var(--font-mono);
        font-weight: 600;
        padding: 2px 4px;
        cursor: pointer;
    }

    /* ─── Servo binding editor (mirrors GearControl's .door-servo-* layout) ─── */
    .servo-edit-block {
        padding: 8px 10px;
        margin-top: 8px;
        background: var(--bg-raised);
        border-radius: 4px;
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }
    .servo-edit-header {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-bottom: 6px;
    }
    .servo-edit-label {
        font-size: 11px;
        font-weight: 700;
        color: var(--text);
        font-family: var(--font-mono);
        text-transform: uppercase;
        letter-spacing: 0.5px;
    }
    .servo-edit-pin {
        font-family: var(--font-mono);
        font-size: 10px;
    }
    .servo-edit-grid {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 1fr));
        gap: 8px;
        margin-top: 6px;
    }
    .servo-edit-field {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }
    .servo-edit-field .field-label {
        font-size: 9px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }
    .servo-edit-footer {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-top: 6px;
    }
    .servo-live-bar-track {
        flex: 1;
        height: 5px;
        background: var(--bg-input);
        border-radius: 3px;
        overflow: hidden;
    }
    .servo-live-bar-fill {
        height: 100%;
        background: var(--accent);
        border-radius: 3px;
        transition: width 0.2s;
    }
    .servo-live-value {
        margin-left: auto;
        font-family: var(--font-mono);
        font-size: 11px;
        font-weight: 700;
        color: var(--text);
        min-width: 64px;
        text-align: right;
    }

    .slider { accent-color: var(--accent); }
    .slider.wide { flex: 1; min-width: 60px; }

    /* ─── Band Range Slider ─── */
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

    .band-range::-webkit-slider-runnable-track { height: 4px; background: transparent; }
    .band-range::-moz-range-track { height: 4px; background: transparent; }

    .band-val {
        font-family: var(--font-mono);
        font-size: 11px;
        color: var(--text-dim);
        min-width: 32px;
        text-align: center;
    }

    .band-label.band-overlap { color: var(--warning); }
    .band-warn { font-size: 14px; color: var(--warning); margin-left: 2px; }

    .slider-val-sm {
        font-family: var(--font-mono);
        font-size: 10px;
        color: var(--text-dim);
        min-width: 28px;
        text-align: right;
    }

    /* ─── Button Overrides ─── */
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

    .primary {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
        border-color: var(--accent);
        color: var(--accent);
    }

    /* ─── Battery card ─── */
    .batt-display { margin-bottom: 10px; }

    .batt-bar-track {
        height: 6px;
        background: var(--bg-input);
        border-radius: 3px;
        overflow: hidden;
        margin-bottom: 6px;
    }
    .batt-bar-fill {
        height: 100%;
        background: var(--ok, #4ec9b0);
        border-radius: 3px;
        transition: width 0.3s;
    }
    .batt-bar-fill.low { background: var(--error); }

    .batt-info {
        display: flex;
        align-items: center;
        gap: 10px;
    }
    .batt-voltage {
        font-size: 16px;
        font-weight: 700;
        font-family: var(--font-mono);
        color: var(--text-bright);
    }
    .batt-voltage.low { color: var(--error); }
    .batt-pct {
        font-size: 12px;
        font-weight: 600;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }
    .batt-pct.low { color: var(--error); }
    .batt-warn {
        font-size: 10px;
        font-weight: 700;
        color: var(--error);
        background: color-mix(in srgb, var(--error) 12%, transparent);
        padding: 1px 6px;
        border-radius: 3px;
    }

    /* ─── Live-push status badge (Rule 24) ─── */
    .push-badge {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        padding: 1px 6px;
        border-radius: 3px;
        white-space: nowrap;
        min-height: 14px;
        display: inline-block;
    }
    .push-badge.push-pending { color: var(--accent);  background: color-mix(in srgb, var(--accent)  12%, transparent); }
    .push-badge.push-sent    { color: var(--ok, #4ec9b0); background: color-mix(in srgb, var(--ok, #4ec9b0) 12%, transparent); }
    .push-badge.push-invalid { color: var(--warning, #d7ba7d); background: color-mix(in srgb, var(--warning, #d7ba7d) 14%, transparent); }

    /* ─── Toggle text + setting-input ─── */
    .toggle-text {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text);
    }
    .setting-input {
        display: flex;
        align-items: center;
        gap: 6px;
    }
</style>
