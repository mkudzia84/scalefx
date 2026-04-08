<!-- ScaleFX Studio — LightSetup (shared component) -->
<!-- Reusable LED channel/sequence editor. Used by both HubFX (6 ch) and LightFX (8 ch). -->
<script lang="ts">
    import { SendCommand } from '../../../wailsjs/go/main/App'

    export let channelCount: number = 8
    export let sectionLabel: string = 'Lights'
    export let channelStartIndex: number = 1
    export let masterBrightness: number = 100

    // ─── Event Type Definitions ───
    interface EventTypeDef {
        name: string
        value: string
        params: ParamDef[]
    }

    interface ParamDef {
        label: string
        key: string
        min: number
        max: number
        step: number
        defaultVal: number
        unit?: string
    }

    const eventTypes: EventTypeDef[] = [
        {
            name: 'ON', value: 'on',
            params: [
                { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 10, defaultVal: 1000, unit: 'ms' },
                { label: 'Brightness', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
            ]
        },
        {
            name: 'OFF', value: 'off',
            params: [
                { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 10, defaultVal: 1000, unit: 'ms' },
            ]
        },
        {
            name: 'FLASH', value: 'flash',
            params: [
                { label: 'Interval', key: 'interval', min: 10, max: 10000, step: 10, defaultVal: 100, unit: 'ms' },
                { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 10, defaultVal: 2000, unit: 'ms' },
                { label: 'Brightness', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
                { label: 'Duty', key: 'duty', min: 1, max: 100, step: 1, defaultVal: 50, unit: '%' },
            ]
        },
        {
            name: 'FADE IN', value: 'fadein',
            params: [
                { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 10, defaultVal: 1000, unit: 'ms' },
                { label: 'Brightness', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
            ]
        },
        {
            name: 'FADE OUT', value: 'fadeout',
            params: [
                { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 10, defaultVal: 1000, unit: 'ms' },
                { label: 'Brightness', key: 'brightness', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
            ]
        },
        {
            name: 'FADING', value: 'fading',
            params: [
                { label: 'Cycle', key: 'cycle', min: 10, max: 10000, step: 10, defaultVal: 2000, unit: 'ms' },
                { label: 'Duration', key: 'duration', min: 10, max: 60000, step: 100, defaultVal: 10000, unit: 'ms' },
                { label: 'Min', key: 'min', min: 0, max: 100, step: 1, defaultVal: 0, unit: '%' },
                { label: 'Max', key: 'max', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
            ]
        },
        {
            name: 'BEACON', value: 'beacon',
            params: [
                { label: 'Cycle', key: 'cycle', min: 100, max: 10000, step: 50, defaultVal: 1000, unit: 'ms' },
                { label: 'Duration', key: 'duration', min: 100, max: 60000, step: 100, defaultVal: 10000, unit: 'ms' },
                { label: 'Flash %', key: 'flashPct', min: 1, max: 50, step: 1, defaultVal: 15, unit: '%' },
                { label: 'Peak', key: 'max', min: 0, max: 100, step: 1, defaultVal: 100, unit: '%' },
            ]
        },
    ]

    // ─── Light Presets ───
    // Timing reference: LedFlashing full cycle = interval × 2, on-time = cycle × duty / 100
    // FAA 14 CFR §25.1401: anti-collision 40-100 flashes/min (600-1500ms cycle)
    interface LightPreset {
        name: string
        group: string
        events: { type: string; params: Record<string, number> }[]
    }

    const presetGroups = ['', 'Aircraft', 'Vehicle', 'Naval', 'Effects'] as const

    const presets: LightPreset[] = [
        { name: 'Custom', group: '', events: [] },

        // ── Aircraft ──
        { name: 'Nav Position', group: 'Aircraft', events: [
            { type: 'on', params: { duration: 0, brightness: 100 } },
        ]},
        { name: 'Nav Dimmed', group: 'Aircraft', events: [
            { type: 'on', params: { duration: 0, brightness: 40 } },
        ]},
        { name: 'Nav Flashing', group: 'Aircraft', events: [
            { type: 'flash', params: { interval: 500, duration: 0, brightness: 100, duty: 50 } },
        ]},
        { name: 'Anti-Col Beacon', group: 'Aircraft', events: [
            { type: 'beacon', params: { cycle: 1300, duration: 0, flashPct: 12, max: 100 } },
        ]},
        { name: 'Beacon Rotating', group: 'Aircraft', events: [
            { type: 'beacon', params: { cycle: 1300, duration: 0, flashPct: 30, max: 100 } },
        ]},
        { name: 'Strobe Single', group: 'Aircraft', events: [
            { type: 'flash', params: { interval: 650, duration: 0, brightness: 100, duty: 4 } },
        ]},
        { name: 'Strobe Double', group: 'Aircraft', events: [
            { type: 'on', params: { duration: 50, brightness: 100 } },
            { type: 'off', params: { duration: 100 } },
            { type: 'on', params: { duration: 50, brightness: 100 } },
            { type: 'off', params: { duration: 1100 } },
        ]},
        { name: 'Landing Light', group: 'Aircraft', events: [
            { type: 'fadein', params: { duration: 400, brightness: 100 } },
            { type: 'on', params: { duration: 0, brightness: 100 } },
        ]},
        { name: 'Taxi / Hover', group: 'Aircraft', events: [
            { type: 'on', params: { duration: 0, brightness: 80 } },
        ]},
        // ── Sequenced (assign each to separate channel for phased flash) ──
        { name: 'Seq: Beacon (1/3)', group: 'Aircraft', events: [
            { type: 'beacon', params: { cycle: 1500, duration: 0, flashPct: 15, max: 100 } },
        ]},
        { name: 'Seq: Strobe (2/3)', group: 'Aircraft', events: [
            { type: 'off', params: { duration: 500 } },
            { type: 'on', params: { duration: 50, brightness: 100 } },
            { type: 'off', params: { duration: 950 } },
        ]},
        { name: 'Seq: Nav (3/3)', group: 'Aircraft', events: [
            { type: 'off', params: { duration: 1000 } },
            { type: 'on', params: { duration: 80, brightness: 100 } },
            { type: 'off', params: { duration: 420 } },
        ]},

        // ── Vehicle ──
        { name: 'Headlight Lo', group: 'Vehicle', events: [
            { type: 'on', params: { duration: 0, brightness: 70 } },
        ]},
        { name: 'Turn Signal', group: 'Vehicle', events: [
            { type: 'flash', params: { interval: 350, duration: 0, brightness: 100, duty: 50 } },
        ]},
        { name: 'Brake Light', group: 'Vehicle', events: [
            { type: 'on', params: { duration: 0, brightness: 100 } },
        ]},

        // ── Naval ──
        { name: 'Masthead', group: 'Naval', events: [
            { type: 'on', params: { duration: 0, brightness: 100 } },
        ]},
        { name: 'Sidelight', group: 'Naval', events: [
            { type: 'on', params: { duration: 0, brightness: 80 } },
        ]},

        // ── Effects ──
        { name: 'Rotating Light', group: 'Effects', events: [
            { type: 'beacon', params: { cycle: 2000, duration: 0, flashPct: 25, max: 100 } },
        ]},
        { name: 'Breathing', group: 'Effects', events: [
            { type: 'fading', params: { cycle: 3000, duration: 0, min: 5, max: 80 } },
        ]},
    ]

    // ─── Channel State ───
    interface SeqEvent {
        id: number
        type: string          // event value ('on','off','flash',...)
        typeName: string      // display name
        params: Record<string, number>
    }

    interface ChannelState {
        enabled: boolean
        preset: string         // preset name or 'Custom'
        events: SeqEvent[]
        expanded: boolean
        brightness: number     // direct brightness 0-100
    }

    let channels: ChannelState[] = Array.from({ length: channelCount }, () => ({
        enabled: true,
        preset: 'Custom',
        events: [],
        expanded: false,
        brightness: 0,
    }))

    let nextEventId = 1

    // ─── Event editing ───
    let addEventCh: number | null = null // which channel's add form is open
    let addEventType = 0                 // index into eventTypes
    let addEventParams: Record<string, number> = {}

    function openAddEvent(chIdx: number) {
        addEventCh = chIdx
        addEventType = 0
        resetAddParams()
    }

    function resetAddParams() {
        const evtDef = eventTypes[addEventType]
        addEventParams = {}
        for (const p of evtDef.params) {
            addEventParams[p.key] = p.defaultVal
        }
    }

    function onEventTypeChange() {
        resetAddParams()
    }

    function addEvent(chIdx: number) {
        const evtDef = eventTypes[addEventType]
        const evt: SeqEvent = {
            id: nextEventId++,
            type: evtDef.value,
            typeName: evtDef.name,
            params: { ...addEventParams },
        }
        channels[chIdx].events = [...channels[chIdx].events, evt]
        channels[chIdx].preset = 'Custom'
        addEventCh = null
    }

    function removeEvent(chIdx: number, evtId: number) {
        channels[chIdx].events = channels[chIdx].events.filter(e => e.id !== evtId)
        channels[chIdx].preset = 'Custom'
        channels = channels
    }

    // ─── Preset application ───
    function applyPreset(chIdx: number) {
        const name = channels[chIdx].preset
        const preset = presets.find(p => p.name === name)
        if (!preset || name === 'Custom') return
        channels[chIdx].events = preset.events.map(e => {
            const def = eventTypes.find(et => et.value === e.type)
            return {
                id: nextEventId++,
                type: e.type,
                typeName: def?.name ?? e.type,
                params: { ...e.params },
            }
        })
        channels = channels
    }

    // ─── Commands ───
    function ch(idx: number): number { return channelStartIndex + idx }

    function sendSequence(chIdx: number) {
        const c = ch(chIdx)
        SendCommand(`seq.clear ${c}`)
        for (const evt of channels[chIdx].events) {
            const p = evt.params
            switch (evt.type) {
                case 'on':
                    SendCommand(`seq.add ${c} on ${p.duration} ${p.brightness} ${p.pwmDuty ?? 0}`); break
                case 'off':
                    SendCommand(`seq.add ${c} off ${p.duration}`); break
                case 'flash':
                    SendCommand(`seq.add ${c} flash ${p.interval} ${p.duration} ${p.brightness} ${p.duty ?? 50}`); break
                case 'fadein':
                    SendCommand(`seq.add ${c} fadein ${p.duration} ${p.brightness}`); break
                case 'fadeout':
                    SendCommand(`seq.add ${c} fadeout ${p.duration} ${p.brightness}`); break
                case 'fading':
                    SendCommand(`seq.add ${c} fading ${p.cycle} ${p.duration} ${p.min ?? 0} ${p.max ?? 100}`); break
                case 'beacon':
                    SendCommand(`seq.add ${c} beacon ${p.cycle} ${p.duration} ${p.flashPct ?? 15} ${p.max ?? 100} ${p.min ?? 0}`); break
            }
        }
        SendCommand(`seq.start ${c}`)
    }

    function stopSequence(chIdx: number) { SendCommand(`seq.stop ${ch(chIdx)}`) }
    function clearSequence(chIdx: number) {
        SendCommand(`seq.clear ${ch(chIdx)}`)
        channels[chIdx].events = []
        channels = channels
    }

    function setDirectBrightness(chIdx: number) {
        SendCommand(`led ${ch(chIdx)} ${channels[chIdx].brightness}`)
    }

    function enableChannel(chIdx: number) {
        SendCommand(`enable ${ch(chIdx)}`)
        channels[chIdx].enabled = true; channels = channels
    }
    function disableChannel(chIdx: number) {
        SendCommand(`disable ${ch(chIdx)}`)
        channels[chIdx].enabled = false; channels = channels
    }

    function setMasterBrightness() { SendCommand(`brightness ${masterBrightness}`) }
    function allOff() { SendCommand('led.off 0') }

    function toggleExpand(chIdx: number) {
        channels[chIdx].expanded = !channels[chIdx].expanded
        channels = channels
    }

    // ─── Helper: format event summary ───
    function durLabel(ms: number): string { return ms === 0 ? '∞' : `${ms}ms` }

    function eventSummary(evt: SeqEvent): string {
        const p = evt.params
        switch (evt.type) {
            case 'on':     return `${durLabel(p.duration)} @ ${p.brightness}%` + (p.duration === 0 ? ' ⛔ no loop' : '')
            case 'off':    return durLabel(p.duration) + (p.duration === 0 ? ' ⛔ no loop' : '')
            case 'flash':  return `⚡ ${p.interval}ms × ${durLabel(p.duration)} ${p.brightness}% duty:${p.duty}%`
            case 'fadein': return `↗ ${p.duration}ms → ${p.brightness}%`
            case 'fadeout':return `↘ ${p.duration}ms → ${p.brightness}%`
            case 'fading': return `~ ${p.cycle}ms cycle, ${durLabel(p.duration)} (${p.min}-${p.max}%)`
            case 'beacon': return `◉ ${p.cycle}ms cycle, ${durLabel(p.duration)} flash ${p.flashPct}% peak ${p.max}%`
            default: return ''
        }
    }
</script>

<section class="light-setup">
    <div class="section-header">
        <h3>{sectionLabel}</h3>
        <div class="master-controls">
            <span class="field-label">Master</span>
            <input type="range" bind:value={masterBrightness} min="0" max="100" class="slider master-slider" />
            <span class="slider-val">{masterBrightness}%</span>
            <button class="small" on:click={setMasterBrightness}>Set</button>
            <button class="small danger" on:click={allOff}>All Off</button>
        </div>
    </div>

    <!-- Channel list -->
    {#each channels as channel, idx}
        <div class="channel" class:disabled={!channel.enabled}>
            <!-- Channel header row -->
            <div class="channel-header" on:click={() => toggleExpand(idx)}
                 on:keydown={(e) => e.key === 'Enter' && toggleExpand(idx)}
                 role="button" tabindex="0">
                <span class="ch-expand">{channel.expanded ? '▾' : '▸'}</span>
                <span class="ch-number">{ch(idx)}</span>

                <!-- Enable toggle -->
                <!-- svelte-ignore a11y-label-has-associated-control -->
                <!-- svelte-ignore a11y-click-events-have-key-events -->
                <label class="ch-enable" on:click|stopPropagation={() => {}}>
                    <input type="checkbox" checked={channel.enabled}
                           on:change={() => channel.enabled ? disableChannel(idx) : enableChannel(idx)} />
                </label>

                <!-- Preset dropdown -->
                <select class="ch-preset" bind:value={channel.preset}
                        on:change={() => applyPreset(idx)}
                        on:click|stopPropagation={() => {}}>
                    <option value="Custom">Custom</option>
                    {#each presetGroups.filter(g => g !== '') as group}
                        <optgroup label={group}>
                            {#each presets.filter(p => p.group === group) as preset}
                                <option value={preset.name}>{preset.name}</option>
                            {/each}
                        </optgroup>
                    {/each}
                </select>

                <!-- Events summary -->
                <span class="ch-summary">
                    {channel.events.length} event{channel.events.length !== 1 ? 's' : ''}
                </span>

                <!-- Quick controls -->
                <!-- svelte-ignore a11y-click-events-have-key-events -->
                <!-- svelte-ignore a11y-no-static-element-interactions -->
                <div class="ch-quick" on:click|stopPropagation={() => {}}>
                    <button class="small primary" on:click={() => sendSequence(idx)}
                            disabled={channel.events.length === 0} title="Upload & play sequence">▶</button>
                    <button class="small" on:click={() => stopSequence(idx)} title="Stop sequence">■</button>
                </div>
            </div>

            <!-- Expanded channel detail -->
            {#if channel.expanded}
                <div class="channel-detail">
                    <!-- Direct brightness -->
                    <div class="detail-row">
                        <span class="field-label">Direct</span>
                        <input type="range" bind:value={channel.brightness} min="0" max="100" class="slider" style="width: 120px"/>
                        <span class="slider-val-sm">{channel.brightness}%</span>
                        <button class="small" on:click={() => setDirectBrightness(idx)}>Set</button>
                        <span class="spacer"></span>
                        <button class="small" on:click={() => clearSequence(idx)}>Clear Seq</button>
                    </div>

                    <!-- Event list -->
                    {#if channel.events.length > 0}
                        <div class="event-list">
                            {#each channel.events as evt, evtIdx (evt.id)}
                                <div class="event-item">
                                    <span class="evt-index">{evtIdx + 1}</span>
                                    <span class="evt-type">{evt.typeName}</span>
                                    <span class="evt-params">{eventSummary(evt)}</span>
                                    <button class="tiny danger" on:click={() => removeEvent(idx, evt.id)} title="Remove">✕</button>
                                </div>
                            {/each}
                        </div>
                    {:else}
                        <div class="empty-events">No events — select a preset or add manually.</div>
                    {/if}

                    <!-- Add event form -->
                    {#if addEventCh === idx}
                        <div class="add-event-form">
                            <div class="add-event-top">
                                <select bind:value={addEventType} on:change={onEventTypeChange} class="evt-type-select">
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
                        <button class="small add-btn" on:click={() => openAddEvent(idx)}>+ Add Event</button>
                    {/if}
                </div>
            {/if}
        </div>
    {/each}
</section>

<style>
    .light-setup {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 14px 16px;
    }

    /* ─── Section Header ─── */
    .section-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 12px;
        padding-bottom: 8px;
        border-bottom: 1px solid var(--border);
    }

    .section-header h3 {
        font-size: 13px;
        font-weight: 600;
        color: var(--text-bright);
        text-transform: uppercase;
        letter-spacing: 0.5px;
    }

    .master-controls {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .master-slider { width: 100px; }

    /* ─── Channel ─── */
    .channel {
        border: 1px solid color-mix(in srgb, var(--border) 60%, transparent);
        border-radius: 4px;
        margin-bottom: 4px;
        overflow: hidden;
        transition: opacity 0.15s;
    }

    .channel.disabled { opacity: 0.5; }

    .channel-header {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 6px 10px;
        cursor: pointer;
        background: var(--bg-raised);
        transition: background 0.1s;
    }

    .channel-header:hover { background: color-mix(in srgb, var(--accent) 5%, var(--bg-raised)); }

    .ch-expand {
        font-size: 10px;
        color: var(--text-dim);
        width: 12px;
        text-align: center;
    }

    .ch-number {
        font-family: var(--font-mono);
        font-size: 12px;
        font-weight: 700;
        color: var(--text-bright);
        width: 18px;
        text-align: center;
    }

    .ch-enable {
        cursor: pointer;
        display: flex;
        align-items: center;
    }

    .ch-enable input { accent-color: var(--accent); margin: 0; }

    .ch-preset {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        font-size: 11px;
        font-family: var(--font-mono);
        padding: 2px 6px;
        cursor: pointer;
        width: 120px;
    }

    .ch-summary {
        font-size: 10px;
        color: var(--text-dim);
        font-family: var(--font-mono);
        flex: 1;
        text-align: right;
        padding-right: 8px;
    }

    .ch-quick {
        display: flex;
        gap: 3px;
    }

    /* ─── Channel Detail (expanded) ─── */
    .channel-detail {
        padding: 8px 10px 10px 42px;
        background: var(--bg-surface);
        border-top: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }

    .detail-row {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 8px;
    }

    .spacer { flex: 1; }

    /* ─── Event list ─── */
    .event-list {
        border: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        border-radius: 3px;
        margin-bottom: 8px;
        overflow: hidden;
    }

    .event-item {
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 4px 8px;
        border-bottom: 1px solid color-mix(in srgb, var(--border) 30%, transparent);
    }

    .event-item:last-child { border-bottom: none; }

    .evt-index {
        font-family: var(--font-mono);
        font-size: 10px;
        color: var(--text-dim);
        width: 16px;
        text-align: right;
    }

    .evt-type {
        font-family: var(--font-mono);
        font-size: 11px;
        font-weight: 600;
        color: var(--accent);
        min-width: 60px;
    }

    .evt-params {
        font-family: var(--font-mono);
        font-size: 10px;
        color: var(--text);
        flex: 1;
    }

    .empty-events {
        font-size: 11px;
        color: var(--text-dim);
        padding: 8px;
        text-align: center;
        font-style: italic;
    }

    /* ─── Add Event Form ─── */
    .add-event-form {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 4px;
        padding: 8px;
        margin-top: 4px;
    }

    .add-event-top {
        display: flex;
        align-items: flex-end;
        gap: 8px;
        flex-wrap: wrap;
    }

    .evt-type-select {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text-bright);
        font-size: 11px;
        font-family: var(--font-mono);
        font-weight: 600;
        padding: 4px 6px;
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
        font-size: 11px;
        padding: 3px 6px;
        width: 70px;
    }

    .param-input:focus { border-color: var(--border-focus); outline: none; }

    .param-unit {
        font-size: 9px;
        color: var(--text-dim);
        font-family: var(--font-mono);
        position: absolute;
        right: 4px;
        bottom: 4px;
    }

    .add-event-actions {
        display: flex;
        gap: 6px;
        margin-top: 8px;
    }

    .add-btn {
        margin-top: 4px;
        width: 100%;
        text-align: center;
        border-style: dashed;
        color: var(--text-dim);
    }

    .add-btn:hover { color: var(--accent); border-color: var(--accent); }

    /* ─── Shared styles ─── */
    .field-label {
        font-size: 11px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .slider { accent-color: var(--accent); }

    .slider-val, .slider-val-sm {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text-dim);
        min-width: 36px;
        text-align: right;
    }

    .slider-val-sm { font-size: 11px; min-width: 30px; }

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

    .danger:hover {
        background: color-mix(in srgb, var(--error) 15%, var(--bg-raised));
        border-color: var(--error);
    }
</style>
