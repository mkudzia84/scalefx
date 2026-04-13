<!-- ScaleFX Studio — LightFX Tab -->
<!-- Two-column: Left=Programs & Channels, Right=Landing Light Groups (servo bindings) -->
<!-- Top bar: slave mode toggle, input channel bar view, mode-dependent servo routing. -->
<script lang="ts">
    import { SendCommand } from '../../../wailsjs/go/main/App'
    import { connectionInfo } from '../stores'
    import { eventTypes, presets, presetGroups, type EventTypeDef, type ParamDef, type LightPreset } from '../light-data'
    import SaveConfigDialog from '../dialogs/SaveConfigDialog.svelte'
    import { LightConfigVerifier, type LightConfig, type GroupPolicy } from '../config/light-verifier'
    import { generateLightYaml } from '../config/config-yaml-gen'
    import { EMPTY_RESULT, type VerifyResult } from '../config/config-verifier'

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
        prog.channels.forEach((_, i) => { SendCommand(`seq.stop ${ch(i)}`); channelPlaying[i] = false })
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
            let cmd = `servo.config ${b.servo} ${b.servoMin_us} ${b.servoMax_us}`
            cmd += ` ${b.servoSpeed} ${b.servoAccel} ${b.servoDecel}`
            if (b.servoReversed) cmd += ' rev'
            SendCommand(cmd)
        }
        // 2. Bind group: slot, servo (0=none), channels, brightness
        SendCommand(`landing.bind ${slot} ${servoId} ${channels} 100`)
        // 3. Deploy
        SendCommand(`landing.deploy ${slot}`)
    }
    function retractGroup(gi: number) {
        const mask = groupChannelMask(gi)
        if (mask === 0 && !groups[gi].binding) return
        SendCommand(`landing.retract ${gi + 1}`)
    }
    function deployAllGroups() { for (let gi = 0; gi < groups.length; gi++) deployGroup(gi) }
    function retractAllGroups() { for (let gi = 0; gi < groups.length; gi++) retractGroup(gi) }

    // ─── Servo control (within group binding) ───
    function setServoPosition(gi: number) {
        const s = groups[gi].binding!
        SendCommand(`servo set ${s.servo} ${s.servoPulse_us}`)
    }
    function applyServoConfig(gi: number) {
        const s = groups[gi].binding!
        let cmd = `servo.config ${s.servo} ${s.servoMin_us} ${s.servoMax_us}`
        cmd += ` ${s.servoSpeed} ${s.servoAccel} ${s.servoDecel}`
        if (s.servoReversed) cmd += ' rev'
        SendCommand(cmd)
    }
    function centerServo(gi: number) {
        const s = groups[gi].binding!
        s.servoPulse_us = Math.round((s.servoMin_us + s.servoMax_us) / 2)
        groups = groups; setServoPosition(gi)
    }
    function minServoPos(gi: number) {
        groups[gi].binding!.servoPulse_us = groups[gi].binding!.servoMin_us
        groups = groups; setServoPosition(gi)
    }
    function maxServoPos(gi: number) {
        groups[gi].binding!.servoPulse_us = groups[gi].binding!.servoMax_us
        groups = groups; setServoPosition(gi)
    }
    function setServoMinHere(gi: number) {
        groups[gi].binding!.servoMin_us = groups[gi].binding!.servoPulse_us
        groups = groups; applyServoConfig(gi)
    }
    function setServoMaxHere(gi: number) {
        groups[gi].binding!.servoMax_us = groups[gi].binding!.servoPulse_us
        groups = groups; applyServoConfig(gi)
    }
    function resetServoDefaults(gi: number) {
        const b = groups[gi].binding!
        b.servoMin_us = 500; b.servoMax_us = 2500
        b.servoSpeed = 4000; b.servoAccel = 8000; b.servoDecel = 8000
        b.servoReversed = false
        groups = groups; applyServoConfig(gi)
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
    function setMasterBrightness() { SendCommand(`brightness ${masterBrightness}`) }
    function allOff() { SendCommand('led.off 0') }

    // ─── Save Config Dialog + Verification ───
    const lightVerifier = new LightConfigVerifier()
    let saveDialogOpen = false
    let saveVerifyResult: VerifyResult = EMPTY_RESULT
    let saveConfigText = ''

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
                })),
                groupPolicies: [...p.groupPolicies],
            })),
            groups: groups.map((g, gi) => ({
                name: g.name,
                memberChannels: groupMemberChannels(gi),
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
            channelCount: CHANNEL_COUNT,
            pwmMin: PWM_MIN,
            pwmMax: PWM_MAX,
            hubAvailable: isHubFX || slaveMode,
        }
    }

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

    function openSaveDialog() {
        const cfg = buildLightConfig()
        saveVerifyResult = lightVerifier.verify(cfg)
        saveConfigText = generateLightYaml(cfg)
        saveDialogOpen = true
    }
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
            <button class="primary small" on:click={openSaveDialog}>💾 Save Config…</button>
            {#if !liveResult.valid}
                <span class="verify-badge error" title="{liveResult.counts.error} error(s), {liveResult.counts.warning} warning(s)">
                    ⚠ {liveResult.counts.error + liveResult.counts.warning}
                </span>
            {:else if liveResult.counts.warning > 0}
                <span class="verify-badge warning" title="{liveResult.counts.warning} warning(s)">
                    ⚠ {liveResult.counts.warning}
                </span>
            {/if}
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
                            </div>
                        {/each}
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

                                            <!-- ─── Servo Configuration (always visible) ─── -->
                                            <div class="servo-config-panel">
                                                <span class="scfg-section-title">Servo {group.binding.servo} Config</span>

                                                <!-- Position slider -->
                                                <div class="scfg-row">
                                                    <span class="scfg-label">Position</span>
                                                    <input type="range" bind:value={group.binding.servoPulse_us}
                                                           min={group.binding.servoMin_us} max={group.binding.servoMax_us} step="1"
                                                           class="slider wide" disabled={controlsDisabled} />
                                                    <input type="number" bind:value={group.binding.servoPulse_us}
                                                           class="field-input narrow" min="500" max="2500" step="10"
                                                           disabled={controlsDisabled} />
                                                    <span class="scfg-unit">µs</span>
                                                    <button class="small primary" on:click={() => setServoPosition(gi)}
                                                            disabled={controlsDisabled}>Set</button>
                                                </div>
                                                <!-- Quick position buttons -->
                                                <div class="scfg-quick">
                                                    <button class="tiny" on:click={() => minServoPos(gi)}
                                                            disabled={controlsDisabled}>Min</button>
                                                    <button class="tiny" on:click={() => centerServo(gi)}
                                                            disabled={controlsDisabled}>Center</button>
                                                    <button class="tiny" on:click={() => maxServoPos(gi)}
                                                            disabled={controlsDisabled}>Max</button>
                                                </div>

                                                <!-- Limits -->
                                                <div class="scfg-section">
                                                    <span class="scfg-section-title">Limits</span>
                                                    <div class="scfg-grid">
                                                        <div class="scfg-field">
                                                            <span class="scfg-label">Min µs</span>
                                                            <input type="number" bind:value={group.binding.servoMin_us}
                                                                   class="field-input" min="300" max="2700" step="10"
                                                                   disabled={controlsDisabled} />
                                                        </div>
                                                        <div class="scfg-field">
                                                            <span class="scfg-label">Max µs</span>
                                                            <input type="number" bind:value={group.binding.servoMax_us}
                                                                   class="field-input" min="300" max="2700" step="10"
                                                                   disabled={controlsDisabled} />
                                                        </div>
                                                        <div class="scfg-field scfg-actions-inline">
                                                            <button class="tiny set-btn" on:click={() => setServoMinHere(gi)}
                                                                    disabled={controlsDisabled}>↓ Set Min Here</button>
                                                            <button class="tiny set-btn" on:click={() => setServoMaxHere(gi)}
                                                                    disabled={controlsDisabled}>↑ Set Max Here</button>
                                                        </div>
                                                    </div>
                                                </div>

                                                <!-- Direction -->
                                                <div class="scfg-section">
                                                    <span class="scfg-section-title">Direction</span>
                                                    <!-- svelte-ignore a11y-label-has-associated-control -->
                                                    <label class="toggle-label">
                                                        <input type="checkbox" bind:checked={group.binding.servoReversed}
                                                               disabled={controlsDisabled} />
                                                        <span class="scfg-label">Reversed</span>
                                                    </label>
                                                    <span class="scfg-hint">
                                                        {group.binding.servoReversed ? 'Open = Min, Close = Max' : 'Open = Max, Close = Min'}
                                                    </span>
                                                </div>

                                                <!-- Motion Profile -->
                                                <div class="scfg-section">
                                                    <span class="scfg-section-title">Motion Profile</span>
                                                    <div class="scfg-grid">
                                                        <div class="scfg-field">
                                                            <span class="scfg-label">Speed <span class="scfg-unit">µs/s</span></span>
                                                            <input type="number" bind:value={group.binding.servoSpeed}
                                                                   class="field-input" min="0" max="65535" step="100"
                                                                   disabled={controlsDisabled} />
                                                        </div>
                                                        <div class="scfg-field">
                                                            <span class="scfg-label">Accel <span class="scfg-unit">µs/s²</span></span>
                                                            <input type="number" bind:value={group.binding.servoAccel}
                                                                   class="field-input" min="0" max="65535" step="100"
                                                                   disabled={controlsDisabled} />
                                                        </div>
                                                        <div class="scfg-field">
                                                            <span class="scfg-label">Decel <span class="scfg-unit">µs/s²</span></span>
                                                            <input type="number" bind:value={group.binding.servoDecel}
                                                                   class="field-input" min="0" max="65535" step="100"
                                                                   disabled={controlsDisabled} />
                                                        </div>
                                                    </div>
                                                </div>

                                                <!-- Apply / Defaults -->
                                                <div class="scfg-actions">
                                                    <button class="small primary" on:click={() => applyServoConfig(gi)}
                                                            disabled={controlsDisabled}>Apply Config</button>
                                                    <button class="small" on:click={() => resetServoDefaults(gi)}
                                                            disabled={controlsDisabled}>Defaults</button>
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
    boardType="lightfx"
    boardLabel={boardLabel}
    verifyResult={saveVerifyResult}
    configText={saveConfigText}
    open={saveDialogOpen}
    onSave={() => { SendCommand('config.save'); saveDialogOpen = false }}
    onClose={() => { saveDialogOpen = false }}
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

    .field-input.narrow { width: 50px; }
    .field-input.servo-select { width: 90px; }

    /* ─── Master Control ─── */
    .master-ctrl {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    /* ─── Input Bar ─── */
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

    /* ─── Servo Config (always visible within binding) ─── */
    .servo-config-panel {
        margin-top: 8px;
        padding: 10px;
        background: var(--bg-surface);
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
        border-radius: 4px;
    }

    .scfg-row {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 6px;
    }

    .scfg-quick {
        display: flex;
        gap: 4px;
        margin-bottom: 10px;
    }

    .scfg-section { margin-bottom: 10px; }
    .scfg-section:last-of-type { margin-bottom: 0; }

    .scfg-section-title {
        display: block;
        font-size: 9px;
        font-weight: 700;
        text-transform: uppercase;
        letter-spacing: 0.5px;
        color: var(--text-dim);
        margin-bottom: 6px;
        padding-bottom: 3px;
        border-bottom: 1px solid color-mix(in srgb, var(--border) 30%, transparent);
    }

    .scfg-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(90px, 1fr));
        gap: 8px;
    }

    .scfg-field {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .scfg-actions-inline {
        display: flex;
        flex-direction: column;
        gap: 4px;
        justify-content: flex-end;
    }

    .scfg-label {
        font-size: 9px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
        flex-shrink: 0;
    }

    .scfg-unit {
        font-weight: 400;
        font-size: 8px;
        text-transform: none;
        letter-spacing: 0;
        opacity: 0.7;
    }

    .scfg-hint {
        font-size: 11px;
        color: var(--text-dim);
        font-style: italic;
        margin-top: 2px;
    }

    .scfg-actions {
        margin-top: 8px;
        display: flex;
        gap: 6px;
    }

    .set-btn {
        font-size: 9px;
        padding: 2px 6px;
        color: var(--accent);
        border-color: color-mix(in srgb, var(--accent) 30%, transparent);
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
</style>
