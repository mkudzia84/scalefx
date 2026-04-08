<!-- ScaleFX Studio — GearControl Tab -->
<!-- Left: aggregate deploy/retract, calibration, battery.  Right: per-gear cards with config. -->
<script lang="ts">
    import { SendCommand } from '../../../wailsjs/go/main/App'
    import { connectionInfo } from '../stores'
    import ServoWidget from '../components/ServoWidget.svelte'

    export let boardLabel: string = 'GearControl'

    // ─── Slave mode ───
    let slaveMode = false

    $: isHubFX = $connectionInfo.controllerType === 'hubfx'
    $: hubFxConnected = isHubFX && $connectionInfo.connected
    $: controlsDisabled = slaveMode && !hubFxConnected

    // ─── Gear definitions ───
    const gearCount = 3
    const gearNames = ['Nose', 'Left Main', 'Right Main']

    // ─── Per-gear state ───
    type CalibState = 'uncalibrated' | 'calibrating' | 'calibrated' | 'error'
    type GearAction = 'idle' | 'deploying' | 'retracting'

    let calibStates: CalibState[] = ['uncalibrated', 'uncalibrated', 'uncalibrated']
    let gearActions: GearAction[] = ['idle', 'idle', 'idle']
    let gearEnabled: boolean[] = [true, true, true]
    let calibErrors: string[] = ['', '', '']

    let calibTimeout_s = 60
    let stallThreshold_mA = 500

    $: anyUncalibrated = calibStates.some(s => s !== 'calibrated')
    $: allCalibrated = calibStates.every(s => s === 'calibrated')
    $: hasErrors = calibStates.some(s => s === 'error')

    // ─── Aggregate actions ───
    function gearAllDeploy()  { SendCommand('deploy all'); gearActions = gearActions.map(() => 'deploying' as GearAction) }
    function gearAllRetract() { SendCommand('retract all'); gearActions = gearActions.map(() => 'retracting' as GearAction) }
    function gearAllStop()    { SendCommand('stop all'); gearActions = gearActions.map(() => 'idle' as GearAction) }
    function gearResetAll()   {
        SendCommand('reset all')
        calibStates = calibStates.map(s => s === 'error' ? 'uncalibrated' as CalibState : s)
        calibErrors = ['', '', '']
    }

    // ─── Calibration ───
    function calibrateAll() {
        SendCommand(`calibrate all ${calibTimeout_s}`)
        calibStates = calibStates.map(() => 'calibrating' as CalibState)
        calibErrors = ['', '', '']
    }
    function calibCancelAll() {
        SendCommand('calibrate.cancel all')
        calibStates = calibStates.map(s => s === 'calibrating' ? 'uncalibrated' as CalibState : s)
    }
    function resetGearState() {
        SendCommand('reset all')
        calibStates = calibStates.map(() => 'uncalibrated' as CalibState)
        gearActions = gearActions.map(() => 'idle' as GearAction)
        calibErrors = ['', '', '']
    }
    function calibrate(id: number) {
        SendCommand(`calibrate ${id} ${calibTimeout_s}`)
        calibStates[id] = 'calibrating'
        calibErrors[id] = ''
        calibStates = calibStates
    }
    function calibCancel(id: number) {
        SendCommand(`calibrate.cancel ${id}`)
        calibStates[id] = 'uncalibrated'
        calibStates = calibStates
    }
    function markCalibrated(id: number) {
        calibStates[id] = 'calibrated'
        calibErrors[id] = ''
        calibStates = calibStates
    }
    function markAllCalibrated() {
        calibStates = calibStates.map(() => 'calibrated' as CalibState)
        calibErrors = ['', '', '']
    }

    // Called when protocol reports calibration error for a specific gear
    function setCalibError(id: number, msg: string) {
        calibStates[id] = 'error'
        calibErrors[id] = msg
        calibStates = calibStates
        calibErrors = calibErrors
    }

    // ─── Per-gear actions ───
    function gearDeploy(id: number)  { SendCommand(`deploy ${id}`); gearActions[id] = 'deploying'; gearActions = gearActions }
    function gearRetract(id: number) { SendCommand(`retract ${id}`); gearActions[id] = 'retracting'; gearActions = gearActions }
    function gearStop(id: number)    { SendCommand(`stop ${id}`); gearActions[id] = 'idle'; gearActions = gearActions }
    function gearEnable(id: number)  { SendCommand(`enable ${id}`); gearEnabled[id] = true; gearEnabled = gearEnabled }
    function gearDisable(id: number) { SendCommand(`disable ${id}`); gearEnabled[id] = false; gearEnabled = gearEnabled }
    function gearReset(id: number) {
        SendCommand(`reset ${id}`)
        if (calibStates[id] === 'error') { calibStates[id] = 'uncalibrated'; calibStates = calibStates }
        calibErrors[id] = ''
        calibErrors = calibErrors
    }

    // ─── Door Config (per gear) ───
    interface DoorConfig {
        open0_us: number; close0_us: number
        open1_us: number; close1_us: number
    }

    let doorConfigs: DoorConfig[] = [
        { open0_us: 2000, close0_us: 1000, open1_us: 2000, close1_us: 1000 },
        { open0_us: 2000, close0_us: 1000, open1_us: 2000, close1_us: 1000 },
        { open0_us: 2000, close0_us: 1000, open1_us: 2000, close1_us: 1000 },
    ]

    function applyDoorConfig(id: number) {
        const dc = doorConfigs[id]
        SendCommand(`door.config ${id} ${dc.open0_us} ${dc.close0_us} ${dc.open1_us} ${dc.close1_us}`)
    }

    // ─── Door Mode (per gear) ───
    const doorModeNames = ['None', 'Single', 'Dual Sync', 'Dual Delay', 'Dual Seq']
    const doorModeValues = ['none', 'single', 'dual-sync', 'dual-delay', 'dual-seq']

    interface DoorModeConfig {
        preDeployMode: number
        postDeployMode: number
        delay_ms: number
    }

    let doorModes: DoorModeConfig[] = [
        { preDeployMode: 2, postDeployMode: 0, delay_ms: 500 },
        { preDeployMode: 2, postDeployMode: 0, delay_ms: 500 },
        { preDeployMode: 2, postDeployMode: 0, delay_ms: 500 },
    ]

    function applyDoorMode(id: number) {
        const dm = doorModes[id]
        SendCommand(`door.mode ${id} ${doorModeValues[dm.preDeployMode]} ${doorModeValues[dm.postDeployMode]} ${dm.delay_ms}`)
    }

    // ─── Gear Config (per gear) ───
    interface GearConfig {
        hasYaw: boolean
        stallCurrent_mA: number
        timeout_ms: number
    }

    let gearConfigs: GearConfig[] = [
        { hasYaw: true,  stallCurrent_mA: 500, timeout_ms: 60000 },
        { hasYaw: false, stallCurrent_mA: 500, timeout_ms: 60000 },
        { hasYaw: false, stallCurrent_mA: 500, timeout_ms: 60000 },
    ]

    function applyGearConfig(id: number) {
        const gc = gearConfigs[id]
        const flags = gc.hasYaw ? 0x01 : 0x00
        SendCommand(`gear.config ${id} ${flags} ${gc.stallCurrent_mA} ${gc.timeout_ms}`)
    }

    function saveAllGearConfig() {
        for (let i = 0; i < gearCount; i++) applyGearConfig(i)
    }

    // ─── Yaw Config (per gear) ───
    interface YawConfig {
        neutral_us: number; min_us: number; max_us: number
    }

    let yawConfigs: YawConfig[] = [
        { neutral_us: 1500, min_us: 1000, max_us: 2000 },
        { neutral_us: 1500, min_us: 1000, max_us: 2000 },
        { neutral_us: 1500, min_us: 1000, max_us: 2000 },
    ]

    let yawPosition_us = 1500
    let selectedGear = 0

    function applyYawConfig(id: number) {
        const yc = yawConfigs[id]
        SendCommand(`yaw.config ${id} ${yc.neutral_us} ${yc.min_us} ${yc.max_us}`)
    }

    function setYaw() { SendCommand(`yaw ${yawPosition_us}`) }

    // ─── Servo definitions ───
    const gcServos = [
        { id: 0, name: 'Nose Door A' },  { id: 1, name: 'Nose Door B' },
        { id: 2, name: 'Left Door A' },  { id: 3, name: 'Left Door B' },
        { id: 4, name: 'Right Door A' }, { id: 5, name: 'Right Door B' },
        { id: 6, name: 'Yaw' },          { id: 7, name: 'Spare' },
    ]

    // ─── Battery ───
    let batteryEnabled = false
    let batteryAutoDeploy = false
    let batteryVoltage_mV = 0
    let batteryLowThreshold_mV = 10500

    function applyBattery() {
        const on = batteryEnabled ? 'on' : 'off'
        const cmd = batteryAutoDeploy ? `battery ${on} autodeploy` : `battery ${on}`
        SendCommand(cmd)
    }

    $: batteryVolts = (batteryVoltage_mV / 1000).toFixed(2)
    $: batteryPct = Math.min(100, Math.max(0, Math.round((batteryVoltage_mV - 9000) / (12600 - 9000) * 100)))
    $: batteryLow = batteryVoltage_mV > 0 && batteryVoltage_mV < batteryLowThreshold_mV

    // ─── Status ───
    function refreshStatus() { SendCommand('status') }
</script>

<div class="tab-root">
    <!-- ═══ Title Bar ═══ -->
    <div class="tab-title-bar">
        <h2>{boardLabel}</h2>
        <div class="mode-toggle">
            <!-- svelte-ignore a11y-label-has-associated-control -->
            <label class="toggle-label">
                <input type="checkbox" bind:checked={slaveMode} />
                <span>Slave Mode</span>
            </label>
            {#if slaveMode}
                <span class="mode-hint" class:connected={hubFxConnected}>
                    {hubFxConnected ? 'HubFX connected — controls active' : 'Waiting for HubFX…'}
                </span>
            {/if}
        </div>
        <button class="status-btn" on:click={refreshStatus} title="Refresh board status">Status</button>
    </div>

    <!-- ═══ Scrollable Content ═══ -->
    <div class="tab-scroll">
        <div class="content-wrap" class:controls-disabled={controlsDisabled}>
            {#if controlsDisabled}
                <div class="disabled-overlay">
                    <span>Slave mode — connect HubFX to control GearControl</span>
                </div>
            {/if}

            <div class="two-col">
                <!-- ═══════════  LEFT COLUMN  ═══════════ -->
                <div class="col">

                    <!-- ── Aggregate Deploy / Retract ── -->
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2L2 7l10 5 10-5-10-5z"/><path d="M2 17l10 5 10-5"/><path d="M2 12l10 5 10-5"/></svg> Gear Control</h3>
                            {#if hasErrors}
                                <span class="header-tag error">✕ Error</span>
                            {:else if anyUncalibrated}
                                <span class="header-tag warn">⚠ Calibration required</span>
                            {:else}
                                <span class="header-tag ok">✓ Ready</span>
                            {/if}
                        </div>

                        <div class="agg-buttons">
                            <button class="action-btn primary" disabled={controlsDisabled || !allCalibrated}
                                    on:click={gearAllDeploy}>
                                <span class="icon down">▼</span> Deploy All
                            </button>
                            <button class="action-btn" disabled={controlsDisabled || !allCalibrated}
                                    on:click={gearAllRetract}>
                                <span class="icon up">▲</span> Retract All
                            </button>
                            <button class="action-btn danger" disabled={controlsDisabled}
                                    on:click={gearAllStop}>
                                <span class="icon">■</span> Stop All
                            </button>
                            <button class="small" disabled={controlsDisabled}
                                    on:click={gearResetAll} title="Clear all error states">↻ Reset All</button>
                        </div>

                        <!-- Mini per-gear status strip -->
                        <div class="gear-status-strip">
                            {#each gearNames as name, id}
                                <div class="gear-pip"
                                     class:pip-ok={calibStates[id] === 'calibrated' && gearActions[id] === 'idle'}
                                     class:pip-deploying={gearActions[id] === 'deploying'}
                                     class:pip-retracting={gearActions[id] === 'retracting'}
                                     class:pip-error={calibStates[id] === 'error'}
                                     class:pip-uncal={calibStates[id] === 'uncalibrated' || calibStates[id] === 'calibrating'}
                                     class:pip-disabled={!gearEnabled[id]}>
                                    <span class="pip-dot"></span>
                                    <span class="pip-name">{name}</span>
                                    {#if !gearEnabled[id]}<span class="pip-badge disabled">OFF</span>{/if}
                                    {#if calibStates[id] === 'error'}<span class="pip-badge err">ERR</span>{/if}
                                </div>
                            {/each}
                        </div>
                    </section>

                    <!-- ── Calibration ── -->
                    <section class="card" class:card-warn={anyUncalibrated} class:card-error={hasErrors}>
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg> Calibration</h3>
                            <div class="header-actions">
                                <button class="small primary" disabled={controlsDisabled}
                                        on:click={calibrateAll}>Calibrate All</button>
                                <button class="small danger" disabled={controlsDisabled}
                                        on:click={calibCancelAll}>Cancel All</button>
                                <button class="small" disabled={controlsDisabled}
                                        on:click={resetGearState} title="Reset all gear state (clears errors and calibration)">↻ Reset State</button>
                            </div>
                        </div>

                        <div class="calib-settings">
                            <div class="settings-row">
                                <div class="setting">
                                    <span class="field-label">Timeout</span>
                                    <div class="setting-input">
                                        <input type="number" bind:value={calibTimeout_s} class="field-input narrow"
                                               min="10" max="120" step="5" disabled={controlsDisabled} />
                                        <span class="unit">sec</span>
                                    </div>
                                </div>
                                <div class="setting">
                                    <span class="field-label">Stall Threshold</span>
                                    <div class="setting-input">
                                        <input type="number" bind:value={stallThreshold_mA}
                                               class="field-input narrow" min="50" max="5000" step="50"
                                               disabled={controlsDisabled} />
                                        <span class="unit">mA</span>
                                    </div>
                                </div>
                                <button class="small primary save-btn" disabled={controlsDisabled}
                                        on:click={saveAllGearConfig} title="Save config to all gears">
                                    💾 Save
                                </button>
                            </div>
                        </div>

                        <div class="calib-list">
                            {#each gearNames as name, id}
                                <div class="calib-row"
                                     class:row-uncal={calibStates[id] === 'uncalibrated'}
                                     class:row-active={calibStates[id] === 'calibrating'}
                                     class:row-done={calibStates[id] === 'calibrated'}
                                     class:row-error={calibStates[id] === 'error'}>
                                    <span class="gear-label">{id} — {name}</span>
                                    {#if calibStates[id] === 'calibrated'}
                                        <span class="calib-state ok">✓ Calibrated</span>
                                    {:else if calibStates[id] === 'calibrating'}
                                        <span class="calib-state active"><span class="spin">⚙</span> Running…</span>
                                    {:else if calibStates[id] === 'error'}
                                        <span class="calib-state error">✕ {calibErrors[id] || 'Error'}</span>
                                    {:else}
                                        <span class="calib-state warn">⚠ Required</span>
                                    {/if}
                                    <div class="calib-actions">
                                        {#if calibStates[id] === 'calibrating'}
                                            <button class="small danger" disabled={controlsDisabled}
                                                    on:click={() => calibCancel(id)}>Cancel</button>
                                        {:else if calibStates[id] === 'error'}
                                            <button class="small" disabled={controlsDisabled}
                                                    on:click={() => gearReset(id)}>↻ Reset</button>
                                            <button class="small primary" disabled={controlsDisabled}
                                                    on:click={() => calibrate(id)}>Retry</button>
                                        {:else if calibStates[id] === 'calibrated'}
                                            <button class="small" disabled={controlsDisabled}
                                                    on:click={() => { calibStates[id] = 'uncalibrated'; calibStates = calibStates }}>Reset</button>
                                        {:else}
                                            <button class="small primary" disabled={controlsDisabled}
                                                    on:click={() => calibrate(id)}>Start</button>
                                            <button class="small" disabled={controlsDisabled}
                                                    on:click={() => markCalibrated(id)}>Skip</button>
                                        {/if}
                                    </div>
                                </div>
                            {/each}
                        </div>

                        {#if anyUncalibrated}
                            <div class="calib-footer">
                                <button class="small" disabled={controlsDisabled}
                                        on:click={markAllCalibrated}>Mark All Calibrated</button>
                                <span class="field-hint">Skip calibration if gears are already known-good</span>
                            </div>
                        {/if}
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
                            </div>
                        </div>

                        <div class="form-row">
                            <!-- svelte-ignore a11y-label-has-associated-control -->
                            <label class="toggle">
                                <input type="checkbox" bind:checked={batteryEnabled}
                                       disabled={controlsDisabled} />
                                <span class="toggle-text">Monitoring</span>
                            </label>
                            <!-- svelte-ignore a11y-label-has-associated-control -->
                            <label class="toggle" style="margin-left: 16px">
                                <input type="checkbox" bind:checked={batteryAutoDeploy}
                                       disabled={controlsDisabled} />
                                <span class="toggle-text">Auto-Deploy on Low Voltage</span>
                            </label>
                            <button class="small primary" style="margin-left: auto"
                                    disabled={controlsDisabled}
                                    on:click={applyBattery}>Apply</button>
                        </div>

                        <div class="form-row">
                            <div class="setting">
                                <span class="field-label">Low Threshold</span>
                                <div class="setting-input">
                                    <input type="number" bind:value={batteryLowThreshold_mV}
                                           class="field-input narrow" min="8000" max="13000" step="100"
                                           disabled={controlsDisabled} />
                                    <span class="unit">mV</span>
                                </div>
                            </div>
                        </div>
                    </section>

                    <!-- ── Yaw Steering ── -->
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><polygon points="16.24 7.76 14.12 14.12 7.76 16.24 9.88 9.88 16.24 7.76"/></svg> Yaw Steering</h3>
                            <div class="gear-picker">
                                {#each gearNames as name, id}
                                    <button class="chip" class:active={selectedGear === id}
                                            on:click={() => selectedGear = id}>{id}</button>
                                {/each}
                            </div>
                        </div>

                        <div class="form-grid cols-3">
                            <div class="form-field">
                                <span class="field-label">Neutral µs</span>
                                <input type="number" bind:value={yawConfigs[selectedGear].neutral_us}
                                       class="field-input" min="500" max="2500" step="10"
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Min µs</span>
                                <input type="number" bind:value={yawConfigs[selectedGear].min_us}
                                       class="field-input" min="500" max="2500" step="10"
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Max µs</span>
                                <input type="number" bind:value={yawConfigs[selectedGear].max_us}
                                       class="field-input" min="500" max="2500" step="10"
                                       disabled={controlsDisabled} />
                            </div>
                        </div>
                        <div class="apply-row">
                            <button class="small" disabled={controlsDisabled}
                                    on:click={() => applyYawConfig(selectedGear)}>Apply Yaw Config</button>
                        </div>

                        <div class="subsection-inline">
                            <h4>Yaw Input</h4>
                            <div class="slider-row">
                                <input type="range" bind:value={yawPosition_us} min="1000" max="2000" step="10"
                                       class="slider wide" disabled={controlsDisabled} />
                                <span class="slider-val">{yawPosition_us} µs</span>
                                <button class="small primary" disabled={controlsDisabled}
                                        on:click={setYaw}>Set</button>
                            </div>
                        </div>
                    </section>
                </div>

                <!-- ═══════════  RIGHT COLUMN  ═══════════ -->
                <div class="col">

                    <!-- ── Per-Gear Cards (one per gear) ── -->
                    {#each gearNames as name, id}
                        <section class="card gear-card-wrap"
                                 class:card-error={calibStates[id] === 'error'}
                                 class:card-warn={calibStates[id] === 'uncalibrated'}
                                 class:card-active={gearActions[id] !== 'idle'}
                                 class:card-disabled={!gearEnabled[id]}>

                            <div class="card-header">
                                <div class="gear-header-left">
                                    <span class="status-dot"
                                          class:dot-deploy={gearActions[id] === 'deploying'}
                                          class:dot-retract={gearActions[id] === 'retracting'}
                                          class:dot-idle={gearActions[id] === 'idle' && calibStates[id] === 'calibrated'}
                                          class:dot-error={calibStates[id] === 'error'}
                                          class:dot-uncal={calibStates[id] === 'uncalibrated' || calibStates[id] === 'calibrating'}
                                          class:dot-off={!gearEnabled[id]}></span>
                                    <h3>Gear {id} — {name}</h3>
                                    {#if calibStates[id] === 'error'}
                                        <span class="header-tag error">✕ {calibErrors[id] || 'Error'}</span>
                                    {:else if calibStates[id] === 'calibrated'}
                                        <span class="header-tag ok">✓</span>
                                    {:else if calibStates[id] === 'calibrating'}
                                        <span class="header-tag active"><span class="spin">⚙</span></span>
                                    {:else}
                                        <span class="header-tag warn">⚠</span>
                                    {/if}
                                </div>

                                <!-- Enable / Disable -->
                                <div class="gear-enable">
                                    {#if gearEnabled[id]}
                                        <button class="small" disabled={controlsDisabled}
                                                on:click={() => gearDisable(id)} title="Disable channel">Disable</button>
                                    {:else}
                                        <button class="small primary" disabled={controlsDisabled}
                                                on:click={() => gearEnable(id)} title="Enable channel">Enable</button>
                                    {/if}
                                </div>
                            </div>

                            <!-- Per-gear action buttons -->
                            <div class="gear-actions-row">
                                <button class="small primary" title="Deploy"
                                        disabled={controlsDisabled || calibStates[id] !== 'calibrated' || !gearEnabled[id]}
                                        on:click={() => gearDeploy(id)}>
                                    ▼ Deploy
                                </button>
                                <button class="small" title="Retract"
                                        disabled={controlsDisabled || calibStates[id] !== 'calibrated' || !gearEnabled[id]}
                                        on:click={() => gearRetract(id)}>
                                    ▲ Retract
                                </button>
                                <button class="small danger" title="Stop"
                                        disabled={controlsDisabled || !gearEnabled[id]}
                                        on:click={() => gearStop(id)}>
                                    ■ Stop
                                </button>
                                <button class="small" disabled={controlsDisabled}
                                        on:click={() => gearReset(id)} title="Clear error">↻ Reset</button>
                            </div>

                            <!-- Gear Config -->
                            <div class="subsection-inline">
                                <h4>Configuration</h4>
                                <div class="form-grid cols-3">
                                    <!-- svelte-ignore a11y-label-has-associated-control -->
                                    <label class="toggle form-field">
                                        <input type="checkbox" bind:checked={gearConfigs[id].hasYaw}
                                               disabled={controlsDisabled} />
                                        <span class="toggle-text">Has Yaw</span>
                                    </label>
                                    <div class="form-field">
                                        <span class="field-label">Stall mA</span>
                                        <input type="number" bind:value={gearConfigs[id].stallCurrent_mA}
                                               class="field-input" min="50" max="5000" step="50"
                                               disabled={controlsDisabled} />
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Timeout ms</span>
                                        <input type="number" bind:value={gearConfigs[id].timeout_ms}
                                               class="field-input" min="500" max="65000" step="500"
                                               disabled={controlsDisabled} />
                                    </div>
                                </div>
                                <div class="apply-row">
                                    <button class="small primary" disabled={controlsDisabled}
                                            on:click={() => applyGearConfig(id)}>Apply Config</button>
                                </div>
                            </div>

                            <!-- Door Servo Config -->
                            <div class="subsection-inline">
                                <h4>Door Servos</h4>
                                <div class="form-grid cols-2">
                                    <div class="form-field">
                                        <span class="field-label">Door A Open µs</span>
                                        <input type="number" bind:value={doorConfigs[id].open0_us}
                                               class="field-input" min="500" max="2500" step="10"
                                               disabled={controlsDisabled} />
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Door A Close µs</span>
                                        <input type="number" bind:value={doorConfigs[id].close0_us}
                                               class="field-input" min="500" max="2500" step="10"
                                               disabled={controlsDisabled} />
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Door B Open µs</span>
                                        <input type="number" bind:value={doorConfigs[id].open1_us}
                                               class="field-input" min="500" max="2500" step="10"
                                               disabled={controlsDisabled} />
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Door B Close µs</span>
                                        <input type="number" bind:value={doorConfigs[id].close1_us}
                                               class="field-input" min="500" max="2500" step="10"
                                               disabled={controlsDisabled} />
                                    </div>
                                </div>

                                <!-- Door Mode -->
                                <div class="form-grid cols-3" style="margin-top: 8px;">
                                    <div class="form-field">
                                        <span class="field-label">Pre-Deploy</span>
                                        <select bind:value={doorModes[id].preDeployMode}
                                                class="field-input" disabled={controlsDisabled}>
                                            {#each doorModeNames as modeName, idx}
                                                <option value={idx}>{modeName}</option>
                                            {/each}
                                        </select>
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Post-Deploy</span>
                                        <select bind:value={doorModes[id].postDeployMode}
                                                class="field-input" disabled={controlsDisabled}>
                                            {#each doorModeNames as modeName, idx}
                                                <option value={idx}>{modeName}</option>
                                            {/each}
                                        </select>
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Delay ms</span>
                                        <input type="number" bind:value={doorModes[id].delay_ms}
                                               class="field-input" min="0" max="5000" step="50"
                                               disabled={controlsDisabled} />
                                    </div>
                                </div>

                                <div class="apply-row">
                                    <button class="small" disabled={controlsDisabled}
                                            on:click={() => applyDoorConfig(id)}>Apply Positions</button>
                                    <button class="small" disabled={controlsDisabled}
                                            on:click={() => applyDoorMode(id)}>Apply Mode</button>
                                </div>
                            </div>
                        </section>
                    {/each}

                    <!-- ── Servo Widget (shared) ── -->
                    <ServoWidget servos={gcServos} disabled={controlsDisabled}
                                 label="Door & Yaw Servos" />
                </div>
            </div>
        </div>
    </div>
</div>

<style>
    /* ─── Root layout ─── */
    .tab-root {
        display: flex;
        flex-direction: column;
        height: 100%;
        background: var(--bg-base);
    }

    /* ─── Title bar ─── */
    .tab-title-bar {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 10px 16px;
        background: var(--bg-surface);
        border-bottom: 1px solid var(--border);
        flex-shrink: 0;
    }

    .tab-title-bar h2 {
        font-size: 16px;
        font-weight: 600;
        color: var(--text-bright);
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

    .status-btn {
        font-size: 12px;
        padding: 4px 12px;
    }

    /* ─── Scrollable area ─── */
    .tab-scroll {
        flex: 1;
        overflow-y: auto;
        padding: 14px 16px;
        min-height: 0;
    }

    /* ─── Disabled overlay ─── */
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
        transition: border-color 0.2s;
    }

    .card-warn  { border-color: color-mix(in srgb, var(--warning, #d7ba7d) 60%, transparent); }
    .card-error { border-color: color-mix(in srgb, var(--error) 60%, transparent) !important; }
    .card-active { border-color: color-mix(in srgb, var(--ok, #4ec9b0) 50%, transparent); }

    .card-disabled {
        opacity: 0.55;
    }

    .card-disabled .gear-enable {
        opacity: calc(1 / 0.55);
    }

    .card-disabled .gear-enable button {
        background: color-mix(in srgb, var(--accent) 25%, var(--bg-raised));
        border-color: var(--accent);
        color: var(--accent);
        font-weight: 700;
        box-shadow: 0 0 6px color-mix(in srgb, var(--accent) 30%, transparent);
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

    .gear-header-left {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .header-tag {
        font-size: 10px;
        font-weight: 700;
        padding: 1px 8px;
        border-radius: 3px;
        white-space: nowrap;
    }

    .header-tag.ok    { color: var(--ok, #4ec9b0); background: color-mix(in srgb, var(--ok, #4ec9b0) 10%, transparent); }
    .header-tag.warn  { color: var(--warning, #d7ba7d); background: color-mix(in srgb, var(--warning, #d7ba7d) 10%, transparent); }
    .header-tag.error { color: var(--error); background: color-mix(in srgb, var(--error) 12%, transparent); }
    .header-tag.active { color: var(--accent); background: color-mix(in srgb, var(--accent) 10%, transparent); }

    .header-actions {
        display: flex;
        gap: 6px;
    }

    .gear-enable {
        flex-shrink: 0;
    }

    /* ─── Aggregate buttons ─── */
    .agg-buttons {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 10px;
    }

    .action-btn {
        display: inline-flex;
        align-items: center;
        gap: 5px;
        padding: 6px 16px;
        font-size: 12px;
    }

    .icon { font-size: 11px; }
    .icon.down { color: var(--ok, #4ec9b0); }
    .icon.up { color: var(--accent); }

    /* ─── Gear status strip (mini overview) ─── */
    .gear-status-strip {
        display: flex;
        gap: 8px;
        padding: 8px 0 0;
        border-top: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }

    .gear-pip {
        flex: 1;
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 5px 8px;
        border-radius: 4px;
        background: var(--bg-raised);
        border: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        transition: border-color 0.2s;
    }

    .gear-pip.pip-ok       { border-color: color-mix(in srgb, var(--ok, #4ec9b0) 40%, transparent); }
    .gear-pip.pip-deploying { border-color: color-mix(in srgb, var(--ok, #4ec9b0) 60%, transparent); }
    .gear-pip.pip-retracting { border-color: color-mix(in srgb, var(--accent) 60%, transparent); }
    .gear-pip.pip-error    { border-color: color-mix(in srgb, var(--error) 60%, transparent); background: color-mix(in srgb, var(--error) 5%, var(--bg-raised)); }
    .gear-pip.pip-disabled { opacity: 0.45; }

    .pip-dot {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        flex-shrink: 0;
        background: var(--text-dim);
    }

    .pip-ok .pip-dot       { background: var(--ok, #4ec9b0); }
    .pip-uncal .pip-dot    { background: var(--warning, #d7ba7d); }
    .pip-deploying .pip-dot { background: var(--ok, #4ec9b0); animation: pulse 1s ease-in-out infinite; }
    .pip-retracting .pip-dot { background: var(--accent); animation: pulse 1s ease-in-out infinite; }
    .pip-error .pip-dot    { background: var(--error); animation: pulse 0.6s ease-in-out infinite; }

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
    }

    .pip-badge.disabled {
        color: var(--text-dim);
        background: color-mix(in srgb, var(--border) 50%, transparent);
    }

    .pip-badge.err {
        color: var(--error);
        background: color-mix(in srgb, var(--error) 15%, transparent);
    }

    /* ─── Status dot (per-gear cards) ─── */
    .status-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        flex-shrink: 0;
        background: var(--text-dim);
    }

    .dot-idle    { background: var(--ok, #4ec9b0); }
    .dot-uncal   { background: var(--warning, #d7ba7d); }
    .dot-deploy  { background: var(--ok, #4ec9b0); animation: pulse 1s ease-in-out infinite; }
    .dot-retract { background: var(--accent); animation: pulse 1s ease-in-out infinite; }
    .dot-error   { background: var(--error); animation: pulse 0.6s ease-in-out infinite; }
    .dot-off     { background: var(--text-dim); opacity: 0.4; }

    @keyframes pulse {
        0%, 100% { opacity: 1; }
        50% { opacity: 0.3; }
    }

    /* ─── Per-gear action row ─── */
    .gear-actions-row {
        display: flex;
        gap: 6px;
        align-items: center;
        flex-wrap: wrap;
    }

    /* ─── Gear Picker Chips ─── */
    .gear-picker { display: flex; gap: 4px; }

    .chip {
        font-size: 11px;
        font-weight: 600;
        padding: 2px 10px;
        border-radius: 3px;
        background: var(--bg-input);
        border: 1px solid var(--border);
        color: var(--text-dim);
        cursor: pointer;
        transition: all 0.15s;
    }

    .chip.active {
        background: color-mix(in srgb, var(--accent) 20%, var(--bg-raised));
        color: var(--accent);
        border-color: var(--accent);
    }

    /* ─── Calibration card ─── */
    .calib-settings {
        padding: 10px 12px;
        background: color-mix(in srgb, var(--bg-input) 50%, var(--bg-surface));
        border-radius: 4px;
        margin-bottom: 12px;
    }

    .settings-row {
        display: flex;
        align-items: flex-end;
        gap: 16px;
        flex-wrap: wrap;
    }

    .setting {
        display: flex;
        flex-direction: column;
        gap: 3px;
    }

    .setting-input {
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .save-btn { margin-left: auto; }

    .calib-list {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }

    .calib-row {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 6px 10px;
        border-radius: 4px;
        background: var(--bg-raised);
        border: 1px solid transparent;
        transition: border-color 0.2s, background 0.2s;
    }

    .calib-row.row-uncal {
        border-color: color-mix(in srgb, var(--warning, #d7ba7d) 40%, transparent);
        background: color-mix(in srgb, var(--warning, #d7ba7d) 4%, var(--bg-raised));
    }

    .calib-row.row-active {
        border-color: color-mix(in srgb, var(--accent) 50%, transparent);
        background: color-mix(in srgb, var(--accent) 6%, var(--bg-raised));
    }

    .calib-row.row-done {
        border-color: color-mix(in srgb, var(--ok, #4ec9b0) 30%, transparent);
    }

    .calib-row.row-error {
        border-color: color-mix(in srgb, var(--error) 60%, transparent);
        background: color-mix(in srgb, var(--error) 6%, var(--bg-raised));
    }

    .gear-label {
        font-size: 12px;
        font-weight: 600;
        color: var(--text);
        font-family: var(--font-mono);
        min-width: 110px;
    }

    .calib-state {
        font-size: 11px;
        font-weight: 600;
        min-width: 100px;
    }

    .calib-state.ok     { color: var(--ok, #4ec9b0); }
    .calib-state.warn   { color: var(--warning, #d7ba7d); }
    .calib-state.active { color: var(--accent); }
    .calib-state.error  { color: var(--error); }

    .calib-actions {
        display: flex;
        gap: 4px;
        margin-left: auto;
    }

    .calib-footer {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-top: 10px;
        padding-top: 8px;
        border-top: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }

    /* Spin animation */
    .spin {
        display: inline-block;
        animation: spin 1.5s linear infinite;
    }

    @keyframes spin {
        0%   { transform: rotate(0deg); }
        100% { transform: rotate(360deg); }
    }

    /* ─── Battery display ─── */
    .batt-display {
        margin-bottom: 10px;
    }

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

    .batt-bar-fill.low {
        background: var(--error);
    }

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
        animation: pulse 0.8s ease-in-out infinite;
    }

    /* ─── Forms ─── */
    .form-row {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 8px;
    }

    .form-grid {
        display: grid;
        gap: 8px;
    }

    .form-grid.cols-2 { grid-template-columns: 1fr 1fr; }
    .form-grid.cols-3 { grid-template-columns: 1fr 1fr 1fr; }

    .form-field {
        display: flex;
        flex-direction: column;
        gap: 3px;
    }

    .field-label {
        font-size: 12px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .field-input {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        font-family: var(--font-mono);
        font-size: 13px;
        padding: 4px 8px;
        width: 100%;
    }

    .field-input:focus {
        border-color: var(--border-focus);
        outline: none;
    }

    .field-input.narrow { width: 80px; }

    select.field-input { cursor: pointer; }

    .field-hint {
        font-size: 11px;
        color: var(--text-dim);
        font-style: italic;
    }

    .unit {
        font-size: 11px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .apply-row {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-top: 8px;
    }

    .subsection-inline {
        margin-top: 12px;
        padding-top: 10px;
        border-top: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
    }

    .subsection-inline h4 {
        font-size: 12px;
        font-weight: 600;
        color: var(--text);
        margin-bottom: 8px;
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    /* ─── Slider ─── */
    .slider-row {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .slider { accent-color: var(--accent); }
    .slider.wide { flex: 1; }

    .slider-val {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text-dim);
        min-width: 64px;
        text-align: right;
    }

    /* ─── Toggle ─── */
    .toggle {
        display: flex;
        align-items: center;
        gap: 6px;
        cursor: pointer;
        font-size: 12px;
    }

    .toggle input[type="checkbox"] { accent-color: var(--accent); }

    .toggle-text {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text);
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
