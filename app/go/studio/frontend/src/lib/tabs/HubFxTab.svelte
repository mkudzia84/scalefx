<!-- ScaleFX Studio — HubFX Tab -->
<!-- Two-column layout: Input Setup (left) | Engine FX Setup (right). -->
<!-- Audio, Storage, Config, USB in bottom sections. -->
<script lang="ts">
    import { connectionInfo, slaveInfo, showSaveConfig } from '../stores'
    import { SendCommand, GetSlaveInfo, UploadConfig } from '../../../wailsjs/go/main/App'
    import { onMount, onDestroy } from 'svelte'
    import SaveConfigDialog from '../dialogs/SaveConfigDialog.svelte'
    import {
        generateHubFxSettingsYaml, parseHubFxSettingsYaml,
        type HubFxSettings,
    } from '../config/config-yaml-gen'
    import { EMPTY_RESULT } from '../config/config-verifier'
    import type { BoardConfigDriver } from '../config/board-driver'
    import { autoLoadOnConnect, loadConfigFromDevice } from '../config/config-loader'

    export let boardLabel: string = 'HubFX'

    // ─── Slave Controllers ───
    let slaves: Array<{ type: string; name: string; connected: boolean; ready: boolean; enabled: boolean }> = []

    async function refreshSlaves() {
        try {
            const info = await GetSlaveInfo()
            slaves = (info || []).map(s => ({ ...s, enabled: s.enabled ?? true }))
            slaveInfo.set(slaves)
        } catch { /* ignore */ }
    }

    function toggleSlave(type: string) {
        slaves = slaves.map(s =>
            s.type === type ? { ...s, enabled: !s.enabled } : s
        )
        slaveInfo.set(slaves)
    }

    // ─── Input Channels (PWM) ───
    const NUM_CHANNELS = 10
    type InputMode = 'pwm' | 'ppm' | 'sbus'
    let inputMode: InputMode = 'pwm'

    interface ChannelState {
        value_us: number
        min_us: number
        max_us: number
        label: string
    }

    let channels: ChannelState[] = Array.from({ length: NUM_CHANNELS }, (_, i) => ({
        value_us: 1500,
        min_us: 1000,
        max_us: 2000,
        label: `CH ${i + 1}`,
    }))

    // Stub: simulate random PWM values for visual preview
    let channelInterval: ReturnType<typeof setInterval>

    function startChannelMonitor() {
        channelInterval = setInterval(() => {
            channels = channels.map(ch => ({
                ...ch,
                value_us: 1000 + Math.floor(Math.random() * 1000),
            }))
        }, 200)
    }

    function stopChannelMonitor() {
        if (channelInterval) clearInterval(channelInterval)
    }

    let monitoring = false

    function toggleMonitor() {
        if (monitoring) {
            stopChannelMonitor()
            monitoring = false
        } else {
            startChannelMonitor()
            monitoring = true
        }
    }

    function channelPct(ch: ChannelState): number {
        const range = ch.max_us - ch.min_us
        if (range <= 0) return 50
        return Math.max(0, Math.min(100, ((ch.value_us - ch.min_us) / range) * 100))
    }

    // ─── Engine Sound ───
    let engineRunning = false
    let engineType = 'turbine'
    let engineInputChannel = 1
    let engineThreshold_us = 1500
    let engineSoundStarting = '/sounds/ka50/engine_start.wav'
    let engineSoundRunning = '/sounds/ka50/engine_loop.wav'
    let engineSoundStopping = '/sounds/ka50/engine_stop.wav'
    let engineStartingOffset_ms = 60000
    let engineStoppingOffset_ms = 25000
    let engineOutputChannels = 'all'

    function engineStart() {
        SendCommand('hub:engine.start')
        engineRunning = true
    }

    function engineStop() {
        SendCommand('hub:engine.stop')
        engineRunning = false
    }

    function engineStatus() {
        SendCommand('hub:engine.status')
    }

    function testSound(path: string, channel: number = 3) {
        if (!path) return
        SendCommand(`hub:audio.play ${channel} ${path} 80`)
    }

    function stopTestSound(channel: number = 3) {
        SendCommand(`hub:audio.stop ${channel}`)
    }

    // ─── Audio Mixer ───
    let audioPath = ''
    let audioChannel = 0
    let audioVolume = 100
    let masterVolume = 100

    function audioPlay() {
        if (!audioPath) return
        SendCommand(`hub:audio.play ${audioChannel} ${audioPath} ${audioVolume}`)
    }

    function audioStop(ch: number | 'all' = 'all') {
        SendCommand(`hub:audio.stop ${ch}`)
    }

    function audioSetVolume(ch: number | 'master', vol: number) {
        SendCommand(`hub:audio.volume ${ch} ${vol}`)
    }

    function audioStatus() { SendCommand('hub:audio.status') }
    function codecStatus() { SendCommand('hub:codec.status') }

    // ─── Storage ───
    function sdInit() { SendCommand('sd.init') }
    function sdStatus() { SendCommand('sd.status') }
    function flashStatus() { SendCommand('flash.status') }

    // ─── Config ───
    function configReload() { SendCommand('config.reload') }
    function configStatus() { SendCommand('config.status') }

    // ─── Save Config Dialog ───
    // /hubfx.yaml carries hub-only settings (codec, future input mappings).
    // The hub's master copy of the light program lives in /lightfx.yaml on
    // the hub and is fanned to the LightFX slave on attach (Rule 26).
    let saveDialogOpen = false
    let codecSupplyVoltage: HubFxSettings['codecSupplyVoltage'] = '12v'

    function buildHubFxSettings(): HubFxSettings {
        return { codecSupplyVoltage }
    }

    function applyHubFxSettings(s: HubFxSettings) {
        codecSupplyVoltage = s.codecSupplyVoltage
    }

    const hubDriver: BoardConfigDriver<HubFxSettings> = {
        boardType: 'hubfx',
        boardLabel: 'HubFX ESP32-S3',
        buildState: buildHubFxSettings,
        generateYaml: (s) => generateHubFxSettingsYaml(s),
        parseYaml:    (t) => parseHubFxSettingsYaml(t),
        applyState:   applyHubFxSettings,
        verify:       () => EMPTY_RESULT,
    }

    function openSaveDialog() { saveDialogOpen = true }

    function configSave() {
        openSaveDialog()
    }

    // ─── File Browser ───
    function fileTree(target: string) { SendCommand(`file.tree ${target}`) }
    function fileList(target: string, path: string = '/') { SendCommand(`file.list ${target} ${path}`) }

    // ─── USB ───
    function usbDevices() { SendCommand('hub:usb.devices') }
    function usbReset() { SendCommand('hub:usb.reset') }

    // ─── Status ───
    function refreshStatus() { SendCommand('status') }

    // ─── Lifecycle ───
    onMount(() => {
        refreshSlaves()
        const unsubAutoLoad = autoLoadOnConnect(hubDriver, ['hubfx'])
        return () => { unsubAutoLoad() }
    })
    onDestroy(() => { stopChannelMonitor() })
</script>

<div class="tab-content">
    <!-- Header -->
    <div class="board-header">
        <span class="board-icon" style="color: var(--info)">⎔</span>
        <div class="board-info">
            <h2>{boardLabel}</h2>
            <span class="board-type">hubfx — master hub</span>
        </div>
        <!-- Slave enable toggles -->
        <div class="slave-toggles">
            {#each slaves as slave}
                <!-- svelte-ignore a11y-label-has-associated-control -->
                <label class="slave-toggle" title="{slave.name}: click to {slave.enabled ? 'disable' : 'enable'}">
                    <input type="checkbox" checked={slave.enabled}
                           on:change={() => toggleSlave(slave.type)} />
                    <span class="slave-toggle-label">{slave.name}</span>
                </label>
            {/each}
        </div>
        <button class="status-btn" on:click={refreshStatus} title="Refresh board status">Status</button>
    </div>

    <!-- ═══ Two-Column Main Area ═══ -->
    <div class="two-col">

        <!-- ──── LEFT: Input Setup ──── -->
        <div class="col">
            <section class="card">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/><line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/><line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/><line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/><line x1="17" y1="16" x2="23" y2="16"/></svg> Input Channels</h3>
                    <div class="header-actions">
                        <select bind:value={inputMode} class="field-input mode-select">
                            <option value="pwm">PWM</option>
                            <option value="ppm">PPM</option>
                            <option value="sbus">S.BUS</option>
                        </select>
                        <button class="small" class:active-toggle={monitoring} on:click={toggleMonitor}>
                            {monitoring ? '■ Stop' : '▶ Monitor'}
                        </button>
                    </div>
                </div>

                <!-- Channel bar chart -->
                <div class="channel-bars">
                    {#each channels as ch, i}
                        <div class="channel-row">
                            <span class="ch-label">{ch.label}</span>
                            <div class="bar-track">
                                <div class="bar-fill" style="width: {channelPct(ch)}%"></div>
                                <div class="bar-center"></div>
                            </div>
                            <span class="ch-value">{ch.value_us}</span>
                        </div>
                    {/each}
                </div>

                {#if inputMode === 'ppm' || inputMode === 'sbus'}
                    <div class="stub-notice">
                        {inputMode.toUpperCase()} input mode — firmware support coming soon.
                    </div>
                {/if}
            </section>
        </div>

        <!-- ──── RIGHT: Engine FX Setup ──── -->
        <div class="col">
            <section class="card">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg> Engine FX</h3>
                    <span class="state-badge" class:active={engineRunning}>
                        {engineRunning ? 'RUNNING' : 'STOPPED'}
                    </span>
                </div>

                <!-- Engine controls -->
                <div class="control-row" style="margin-bottom: 14px">
                    <button class="primary action-btn" on:click={engineStart}>
                        <span class="btn-icon">▶</span> Start
                    </button>
                    <button class="action-btn danger" on:click={engineStop}>
                        <span class="btn-icon">■</span> Stop
                    </button>
                    <button class="small" on:click={engineStatus}>Status</button>
                </div>

                <!-- Engine Setup -->
                <div class="subsection">
                    <h4>Engine Setup</h4>
                    <div class="form-grid cols-2">
                        <div class="form-field">
                            <span class="field-label">Engine Type</span>
                            <select bind:value={engineType} class="field-input">
                                <option value="turbine">Turbine</option>
                                <option value="radial">Radial</option>
                                <option value="diesel">Diesel</option>
                            </select>
                        </div>
                        <div class="form-field">
                            <span class="field-label">Output Channels</span>
                            <select bind:value={engineOutputChannels} class="field-input">
                                <option value="all">All</option>
                                <option value="ch1">Ch 1</option>
                                <option value="ch2">Ch 2</option>
                                <option value="ch1+ch2">Ch 1+2</option>
                            </select>
                        </div>
                        <div class="form-field">
                            <span class="field-label">Input Channel</span>
                            <select bind:value={engineInputChannel} class="field-input">
                                {#each Array(NUM_CHANNELS) as _, i}
                                    <option value={i + 1}>{i + 1}</option>
                                {/each}
                            </select>
                        </div>
                        <div class="form-field">
                            <span class="field-label">Threshold µs</span>
                            <input type="number" bind:value={engineThreshold_us} class="field-input"
                                   min="800" max="2200" step="10" />
                        </div>
                    </div>
                </div>

                <!-- Sound Files -->
                <div class="subsection">
                    <h4>Sound Files</h4>
                    <div class="sound-files">
                        <div class="sound-row">
                            <span class="sound-label">Starting</span>
                            <input type="text" bind:value={engineSoundStarting} class="field-input wide"
                                   placeholder="/sounds/ka50/engine_start.wav" />
                            <button class="small" on:click={() => testSound(engineSoundStarting)} disabled={!engineSoundStarting}>▶</button>
                            <button class="small" on:click={() => stopTestSound()}>■</button>
                        </div>
                        <div class="sound-row">
                            <span class="sound-label">Running</span>
                            <input type="text" bind:value={engineSoundRunning} class="field-input wide"
                                   placeholder="/sounds/ka50/engine_loop.wav" />
                            <button class="small" on:click={() => testSound(engineSoundRunning)} disabled={!engineSoundRunning}>▶</button>
                            <button class="small" on:click={() => stopTestSound()}>■</button>
                        </div>
                        <div class="sound-row">
                            <span class="sound-label">Stopping</span>
                            <input type="text" bind:value={engineSoundStopping} class="field-input wide"
                                   placeholder="/sounds/ka50/engine_stop.wav" />
                            <button class="small" on:click={() => testSound(engineSoundStopping)} disabled={!engineSoundStopping}>▶</button>
                            <button class="small" on:click={() => stopTestSound()}>■</button>
                        </div>
                    </div>
                </div>

                <!-- Transition Offsets -->
                <div class="subsection">
                    <h4>Transition Offsets</h4>
                    <div class="form-grid cols-2">
                        <div class="form-field">
                            <span class="field-label">Starting Offset ms</span>
                            <input type="number" bind:value={engineStartingOffset_ms} class="field-input"
                                   min="0" max="120000" step="1000" />
                            <span class="field-hint">Seek when restarting during shutdown</span>
                        </div>
                        <div class="form-field">
                            <span class="field-label">Stopping Offset ms</span>
                            <input type="number" bind:value={engineStoppingOffset_ms} class="field-input"
                                   min="0" max="120000" step="1000" />
                            <span class="field-hint">Seek when stopping during startup</span>
                        </div>
                    </div>
                </div>

                <!-- Config actions -->
                <div class="config-actions">
                    <button class="small" on:click={configReload}>Reload from Device</button>
                    <button class="primary small" on:click={openSaveDialog}>💾 Save Config…</button>
                </div>
            </section>
        </div>
    </div>

    <!-- ═══ Bottom sections (full width) ═══ -->
    <div class="sections">
        <!-- ═══ Audio Mixer ═══ -->
        <section class="card">
            <div class="card-header">
                <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"/></svg> Audio Mixer</h3>
                <div class="header-actions">
                    <button class="small" on:click={audioStatus}>Status</button>
                    <button class="small" on:click={codecStatus}>Codec</button>
                    <button class="small danger" on:click={() => audioStop('all')}>Stop All</button>
                </div>
            </div>
            <div class="audio-play-row">
                <span class="field-label">Channel</span>
                <select bind:value={audioChannel} class="field-input narrow">
                    {#each Array(8) as _, i}
                        <option value={i}>{i}</option>
                    {/each}
                </select>
                <span class="field-label">File</span>
                <input type="text" bind:value={audioPath} class="field-input wide"
                       placeholder="/sounds/file.wav" />
                <span class="field-label">Vol</span>
                <input type="number" bind:value={audioVolume} class="field-input narrow" min="0" max="100" />
                <button class="primary small" on:click={audioPlay} disabled={!audioPath}>Play</button>
                <button class="small" on:click={() => audioStop(audioChannel)}>Stop</button>
            </div>
            <div class="volume-row">
                <span class="field-label">Master Volume</span>
                <input type="range" bind:value={masterVolume} min="0" max="100" class="slider" />
                <span class="volume-val">{masterVolume}%</span>
                <button class="small" on:click={() => audioSetVolume('master', masterVolume)}>Set</button>
            </div>
        </section>

        <!-- ═══ Storage + Config + USB ═══ -->
        <div class="bottom-row">
            <section class="card bottom-card">
                <div class="card-header"><h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg> Storage</h3></div>
                <div class="storage-grid">
                    <div class="storage-section">
                        <h4>SD Card</h4>
                        <div class="control-row">
                            <button class="small" on:click={sdInit}>Init</button>
                            <button class="small" on:click={sdStatus}>Status</button>
                            <button class="small" on:click={() => fileTree('sd')}>Tree</button>
                        </div>
                    </div>
                    <div class="storage-section">
                        <h4>Flash</h4>
                        <div class="control-row">
                            <button class="small" on:click={flashStatus}>Status</button>
                            <button class="small" on:click={() => fileTree('flash')}>Tree</button>
                        </div>
                    </div>
                </div>
            </section>
            <section class="card bottom-card">
                <div class="card-header"><h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v8M8 6l4-4 4 4"/><rect x="4" y="14" width="6" height="6" rx="1"/><rect x="14" y="14" width="6" height="6" rx="1"/><path d="M7 14v-2h10v2"/><path d="M12 12v-2"/></svg> USB Host</h3></div>
                <div class="control-row">
                    <button class="small" on:click={usbDevices}>Devices</button>
                    <button class="small danger" on:click={usbReset}>Reset</button>
                </div>
            </section>
        </div>
    </div>
</div>

<SaveConfigDialog
    driver={hubDriver}
    bind:open={saveDialogOpen}
    onSave={async (yaml) => {
        await UploadConfig(yaml)
        // Round-trip: re-download + re-apply so the tab reflects on-device state.
        await loadConfigFromDevice(hubDriver)
    }}
    onClose={() => { saveDialogOpen = false }}
/>

<style>
    /* HubFxTab-specific overrides — shared styles in style.css */

    .col { gap: 16px; }
    .two-col { margin-bottom: 16px; }

    /* ─── Slave Toggles ─── */
    .slave-toggles {
        display: flex;
        gap: 12px;
        margin-left: auto;
    }

    .slave-toggle {
        display: flex;
        align-items: center;
        gap: 5px;
        cursor: pointer;
        font-size: 12px;
    }

    .slave-toggle input[type="checkbox"] {
        accent-color: var(--accent);
        width: 14px;
        height: 14px;
    }

    .slave-toggle-label {
        color: var(--text);
        font-size: 12px;
        font-weight: 500;
    }

    /* ─── Bottom Sections ─── */
    .sections {
        display: flex;
        flex-direction: column;
        gap: 16px;
    }

    .bottom-row {
        display: grid;
        grid-template-columns: 1fr 1fr 1fr;
        gap: 16px;
    }

    .bottom-card { min-width: 0; }

    /* ─── Channel Bars ─── */
    .channel-bars {
        display: flex;
        flex-direction: column;
        gap: 4px;
    }

    .channel-row {
        display: flex;
        align-items: center;
        gap: 8px;
        height: 22px;
    }

    .ch-label {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        min-width: 36px;
        text-align: right;
    }

    .bar-track {
        flex: 1;
        height: 12px;
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 2px;
        position: relative;
        overflow: hidden;
    }

    .bar-fill {
        position: absolute;
        top: 0;
        left: 0;
        height: 100%;
        background: var(--accent);
        opacity: 0.7;
        transition: width 0.15s ease;
        border-radius: 1px;
    }

    .bar-center {
        position: absolute;
        top: 0;
        left: 50%;
        width: 1px;
        height: 100%;
        background: var(--text-dim);
        opacity: 0.3;
    }

    .ch-value {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        min-width: 36px;
        text-align: left;
    }

    .mode-select {
        width: 70px;
        font-size: 11px;
        padding: 2px 6px;
    }

    .stub-notice {
        margin-top: 10px;
        padding: 8px 10px;
        background: color-mix(in srgb, var(--warning) 10%, var(--bg-raised));
        border: 1px solid color-mix(in srgb, var(--warning) 30%, transparent);
        border-radius: 4px;
        font-size: 11px;
        color: var(--warning);
    }

    .active-toggle {
        color: var(--accent);
        border-color: var(--accent);
    }

    .state-badge.active {
        background: color-mix(in srgb, var(--success) 15%, var(--bg-raised));
        color: var(--success);
        border-color: color-mix(in srgb, var(--success) 40%, transparent);
    }

    .form-grid {
        grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
    }

    /* ─── Sound Files ─── */
    .sound-files {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .sound-row {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .sound-label {
        font-size: 11px;
        font-weight: 600;
        color: var(--text);
        min-width: 58px;
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    /* ─── Config Actions ─── */
    .config-actions {
        margin-top: 12px;
        padding-top: 10px;
        border-top: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        display: flex;
        justify-content: flex-end;
        gap: 8px;
    }

    .field-input.narrow { width: 60px; }

    /* ─── Audio ─── */
    .audio-play-row {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 12px;
    }

    .volume-row {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .slider { max-width: 200px; }

    .volume-val {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text-dim);
        min-width: 36px;
        text-align: right;
    }

    /* ─── Storage ─── */
    .storage-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 12px;
    }

    .storage-section h4 {
        font-size: 12px;
        font-weight: 600;
        color: var(--text);
        margin-bottom: 8px;
    }
</style>
