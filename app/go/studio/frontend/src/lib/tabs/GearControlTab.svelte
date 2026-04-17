<!-- ScaleFX Studio — GearControl Tab -->
<!-- Left: global settings (controls, battery, yaw).  Right: per-channel setup with doors & servos. -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { SendCommand } from '../../../wailsjs/go/main/App'
    import { EventsOn, EventsOff } from '../../../wailsjs/runtime/runtime'
    import { connectionInfo } from '../stores'
    import ServoWidget from '../components/ServoWidget.svelte'
    import SaveConfigDialog from '../dialogs/SaveConfigDialog.svelte'
    import { GearControlConfigVerifier, type GearControlConfig } from '../config/gearcontrol-verifier'
    import { EMPTY_RESULT, type VerifyResult } from '../config/config-verifier'

    export let boardLabel: string = 'GearControl'

    // ─── Connection state ───
    $: isHubFX = $connectionInfo.controllerType === 'hubfx'
    $: isDirect = $connectionInfo.connected && !isHubFX
    $: controlsDisabled = !$connectionInfo.connected || !$connectionInfo.initialized

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

    // Live current readings (updated during calibration via status polling)
    let liveCurrent_mA: number[] = [0, 0, 0]
    type CalibPhase = 'none' | 'clear' | 'deploy' | 'settle'
    let calibPhases: CalibPhase[] = ['none', 'none', 'none']

    let calibTimeout_s = 60

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
        liveCurrent_mA = [0, 0, 0]
        calibPhases = ['clear', 'clear', 'clear']
    }
    function calibCancelAll() {
        SendCommand('calibrate.cancel all')
        calibStates = calibStates.map(s => s === 'calibrating' ? 'uncalibrated' as CalibState : s)
    }
    function calibrate(id: number) {
        SendCommand(`calibrate ${id} ${calibTimeout_s}`)
        calibStates[id] = 'calibrating'
        calibErrors[id] = ''
        liveCurrent_mA[id] = 0
        calibPhases[id] = 'clear'
        calibStates = calibStates
        liveCurrent_mA = liveCurrent_mA
        calibPhases = calibPhases
    }
    function calibCancel(id: number) {
        SendCommand(`calibrate.cancel ${id}`)
        calibStates[id] = 'uncalibrated'
        calibStates = calibStates
    }
    function markCalibrated(id: number) {
        calibStates[id] = 'calibrated'
        calibStates = calibStates
    }
    function markAllCalibrated() {
        calibStates = calibStates.map(() => 'calibrated' as CalibState)
    }
    function resetGearState() {
        SendCommand('reset all')
        calibStates = calibStates.map(() => 'uncalibrated' as CalibState)
        calibErrors = ['', '', '']
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

    // ─── Gear Config (per gear) ───
    interface GearConfig {
        stallCurrent_mA: number   // Established during calibration (read-only)
        timeout_ms: number
    }

    let gearConfigs: GearConfig[] = [
        { stallCurrent_mA: 0, timeout_ms: 60000 },
        { stallCurrent_mA: 0, timeout_ms: 60000 },
        { stallCurrent_mA: 0, timeout_ms: 60000 },
    ]

    function applyGearConfig(id: number) {
        const gc = gearConfigs[id]
        const flags = 0x00  // hasYaw is now set via yaw config
        SendCommand(`gear.config ${id} ${flags} ${gc.stallCurrent_mA} ${gc.timeout_ms}`)
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

    // ─── Yaw Config ───
    interface YawConfig {
        neutral_us: number; min_us: number; max_us: number
        speed: number; reversed: boolean
    }

    let yawGearId = 0
    let yawConfig: YawConfig = { neutral_us: 1500, min_us: 1000, max_us: 2000, speed: 4000, reversed: false }
    let yawPosition_us = 1500

    function applyYawConfig() {
        SendCommand(`yaw.config ${yawGearId} ${yawConfig.neutral_us} ${yawConfig.min_us} ${yawConfig.max_us}`)
    }
    function setYaw() { SendCommand(`yaw ${yawPosition_us}`) }
    function resetYawPosition() {
        yawPosition_us = yawConfig.neutral_us
        SendCommand(`yaw ${yawConfig.neutral_us}`)
    }

    // ─── Battery ───
    // Monitor is always on. Defaults match firmware (LiPo, auto-detect cells).
    let batteryAutoDeploy = false
    let batteryChemistry = 'lipo'
    let batteryCellCount = 0  // 0 = auto-detect
    let batteryVoltage_mV = 0
    const batteryChemistries = ['lipo', 'liion', 'nimh']
    const chemistryLabels: Record<string, string> = { lipo: 'LiPo', liion: 'Li-Ion', nimh: 'NiMH' }
    // Cell voltage bounds (mV) — must match BatteryProfiles in battery_monitor.h
    const cellVoltageBounds: Record<string, { min: number; max: number }> = {
        lipo:  { min: 3000, max: 4200 },
        liion: { min: 2800, max: 4200 },
        nimh:  { min:  900, max: 1400 },
    }
    $: effectiveCellCount = batteryCellCount > 0 ? batteryCellCount : 0
    $: battMinV = (cellVoltageBounds[batteryChemistry]?.min ?? 3000) * effectiveCellCount
    $: battMaxV = (cellVoltageBounds[batteryChemistry]?.max ?? 4200) * effectiveCellCount
    $: batteryVolts = (batteryVoltage_mV / 1000).toFixed(2)
    $: batteryPct = (battMaxV > battMinV && batteryVoltage_mV > 0)
        ? Math.min(100, Math.max(0, Math.round((batteryVoltage_mV - battMinV) / (battMaxV - battMinV) * 100)))
        : 0
    $: batteryLow = batteryVoltage_mV > 0 && effectiveCellCount > 0 && batteryPct < 15

    function applyBattery() {
        const auto = batteryAutoDeploy ? 'autodeploy' : ''
        const cells = batteryCellCount > 0 ? `cells:${batteryCellCount}` : 'auto'
        SendCommand(`battery ${auto} ${batteryChemistry} ${cells}`.replace(/\s+/g, ' ').trim())
    }

    // ─── Pin Mapping ───
    type PinRole = 'door' | 'gear_input' | 'yaw_input' | 'yaw_output' | 'unused'
    const pinRoleOptions: PinRole[] = ['door', 'gear_input', 'yaw_input', 'yaw_output', 'unused']
    const pinRoleLabels: Record<PinRole, string> = {
        door: 'Door Servo', gear_input: 'Gear Input', yaw_input: 'Yaw Input',
        yaw_output: 'Yaw Output', unused: 'Unused'
    }
    const pinSlots = ['pin1', 'pin2', 'pin3', 'pin4', 'pin5', 'pin6', 'pin7']
    const pinGPIOs = ['GP1', 'GP2', 'GP3', 'GP6', 'GP7', 'GP8', 'GP9']

    interface PinConfig {
        role: PinRole; channel: number; min_us: number; max_us: number
        speed: number; reversed: boolean; threshold_us: number
        gear_id: number; neutral_us: number
    }

    function mkPin(role: PinRole, overrides: Partial<PinConfig> = {}): PinConfig {
        return { role, channel: 0, min_us: 500, max_us: 2500, speed: 4000,
                 reversed: false, threshold_us: 1500, gear_id: 0, neutral_us: 1500, ...overrides }
    }

    // Direct mode: standalone — gear_input + yaw_input on first two pins
    const directPinPreset: PinConfig[] = [
        mkPin('gear_input'),                     // pin1: Gear Input
        mkPin('yaw_input'),                      // pin2: Yaw Input
        mkPin('door', { channel: 0 }),           // pin3: Nose Door A
        mkPin('door', { channel: 0 }),           // pin4: Nose Door B
        mkPin('door', { channel: 1 }),           // pin5: Left Door
        mkPin('door', { channel: 2 }),           // pin6: Right Door
        mkPin('yaw_output'),                     // pin7: Yaw Output
    ]

    // Slave mode: HubFX sends commands — no inputs needed, all pins for door servos
    const slavePinPreset: PinConfig[] = [
        mkPin('door', { channel: 0 }),           // pin1: Nose Door A
        mkPin('door', { channel: 0 }),           // pin2: Nose Door B
        mkPin('door', { channel: 1 }),           // pin3: Left Door A
        mkPin('door', { channel: 1 }),           // pin4: Left Door B
        mkPin('door', { channel: 2 }),           // pin5: Right Door A
        mkPin('door', { channel: 2 }),           // pin6: Right Door B
        mkPin('yaw_output'),                     // pin7: Yaw Output
    ]

    let pinConfigs: PinConfig[] = directPinPreset.map(p => ({ ...p }))

    $: yawEnabled = pinConfigs.some(p => p.role === 'yaw_output')
    $: hasGearInput = pinConfigs.some(p => p.role === 'gear_input')
    $: doorPinsPerGear = [0, 1, 2].map(ch => pinConfigs.filter(p => p.role === 'door' && p.channel === ch).length)
    $: gearHasDoors = doorPinsPerGear.map(n => n > 0)

    // Live per-gear door positions (µs), filled by status broadcast.
    // Used by Pin Mapping rows for "live value" readouts and by per-gear
    // door subsections to show actual servo position vs. configured target.
    let liveDoor_us: number[][] = [[0, 0], [0, 0], [0, 0]]

    // Gear ID list for dropdowns — filters out disabled gears so a door /
    // yaw_output cannot be assigned to a channel that's been turned off.
    // We always include the currently-selected gearId so the option doesn't
    // disappear from a control while the user is mid-edit.
    function enabledGearOptions(currentId: number): { id: number; name: string }[] {
        return gearNames
            .map((name, id) => ({ id, name }))
            .filter(g => gearEnabled[g.id] || g.id === currentId)
    }

    // For a door-role pin at table index `pinIdx` belonging to channel `ch`,
    // return 0 (Door A) for the first such pin in pinConfigs, 1 (Door B) for
    // the second. Lets a Pin Mapping row show which leg's live µs it owns.
    function doorLegIndex(pinIdx: number, ch: number): 0 | 1 {
        let n = 0
        for (let i = 0; i < pinIdx; i++) {
            if (pinConfigs[i]?.role === 'door' && pinConfigs[i].channel === ch) n++
        }
        return n === 0 ? 0 : 1
    }

    // Compose a single label that proxies the gear input — what the operator
    // commanded most recently. Fed by aggregated gearActions; "—" when idle.
    $: aggregateGearActionLabel = (() => {
        if (gearActions.some(a => a === 'deploying')) return '▼ Deploying'
        if (gearActions.some(a => a === 'retracting')) return '▲ Retracting'
        return 'Idle'
    })()

    // Map a live PWM µs reading to a 0–100% bar position within [openUs, closeUs].
    // open and close may be inverted (close < open) — we normalize, so the bar
    // grows as the servo moves *toward* the open endpoint.
    function servoPct(value: number, openUs: number, closeUs: number): number {
        if (!value) return 0
        const lo = Math.min(openUs, closeUs)
        const hi = Math.max(openUs, closeUs)
        if (hi <= lo) return 0
        const pct = ((value - lo) / (hi - lo)) * 100
        return Math.max(0, Math.min(100, pct))
    }

    // ─── Warning dismiss (persisted) ───
    // "Don't show again" survives reloads via localStorage.
    const WARN_KEY = 'gearcontrol.directWarningDismissed'
    let warningDismissed = (typeof localStorage !== 'undefined') && localStorage.getItem(WARN_KEY) === '1'
    function dismissWarning(persist: boolean) {
        warningDismissed = true
        if (persist && typeof localStorage !== 'undefined') localStorage.setItem(WARN_KEY, '1')
    }

    // ─── Auto-pick pin preset by connection ───
    // Slave preset when connected via HubFX (no inputs needed); Direct preset
    // for standalone boards. User can still override per-pin in the table.
    let presetApplied = false
    $: if (!presetApplied && $connectionInfo.connected) {
        pinConfigs = (isHubFX ? slavePinPreset : directPinPreset).map(p => ({ ...p }))
        presetApplied = true
    }

    // ─── Live status broadcast listener ───
    interface GearStatusInfo {
        state: number; current_mA: number; door0_us: number; door1_us: number
        stallThreshold_mA: number; shuntVoltage_10uV: number
        errorReason: number; doorPreMode: number; doorPostMode: number
        configFlags: number; doorState: number
    }
    interface BatteryBroadcastInfo {
        voltage_mV: number; enabled: boolean; autoDeploy: boolean; lowVoltage: boolean
    }
    interface GearControlBroadcast {
        gears: GearStatusInfo[]; yaw_us: number; battery: BatteryBroadcastInfo
    }

    // Live values from periodic STATUS_BROADCAST
    let liveYaw_us = 0

    function handleStatusBroadcast(data: GearControlBroadcast) {
        for (let i = 0; i < 3; i++) {
            const g = data.gears[i]
            liveCurrent_mA[i] = g.current_mA
            liveDoor_us[i][0] = g.door0_us
            liveDoor_us[i][1] = g.door1_us

            // Update calibrated stall threshold when firmware reports it
            if (g.stallThreshold_mA > 0) {
                gearConfigs[i].stallCurrent_mA = g.stallThreshold_mA
            }

            // Map firmware state to calibration/action state
            const st = g.state
            if (st === 5) { // ERROR
                calibStates[i] = 'error'
                gearActions[i] = 'idle'
            } else if (st === 6) { // CALIBRATING
                calibStates[i] = 'calibrating'
                gearActions[i] = 'idle'
            } else if (st === 3) { // DEPLOYING
                gearActions[i] = 'deploying'
                if (calibStates[i] === 'calibrating') calibStates[i] = 'calibrating'
                else if (calibStates[i] !== 'error') calibStates[i] = 'calibrated'
            } else if (st === 4) { // RETRACTING
                gearActions[i] = 'retracting'
                if (calibStates[i] !== 'calibrating' && calibStates[i] !== 'error') calibStates[i] = 'calibrated'
            } else if (st === 1 || st === 2) { // DEPLOYED / RETRACTED
                gearActions[i] = 'idle'
                if (calibStates[i] !== 'error') calibStates[i] = 'calibrated'
            } else { // UNKNOWN
                gearActions[i] = 'idle'
                if (calibStates[i] === 'calibrating') calibStates[i] = 'uncalibrated'
            }

            // Enabled flag from config flags bit 7
            gearEnabled[i] = (g.configFlags & 0x80) !== 0

            // Error reason string
            if (g.errorReason !== 0 && st === 5) {
                calibErrors[i] = gearErrorReasonName(g.errorReason)
            }
        }

        // Battery — monitor is always on
        batteryVoltage_mV = data.battery.voltage_mV

        // Yaw
        liveYaw_us = data.yaw_us

        // Trigger reactivity
        liveCurrent_mA = liveCurrent_mA
        liveDoor_us = liveDoor_us
        calibStates = calibStates
        gearActions = gearActions
        gearEnabled = gearEnabled
        calibErrors = calibErrors
        gearConfigs = gearConfigs
    }

    // Must match GearErrorReasonName in app/go/protocol/gearcontrol/gearcontrol.go.
    // See CLAUDE.md Rule 8 (error codes must match across layers) and Rule 1
    // (Protocol Constant Sync).
    function gearErrorReasonName(code: number): string {
        const names: Record<number, string> = {
            0x00: 'None',
            0x01: 'INA226 init failed',
            0x02: 'Motor stall',
            0x03: 'Motor timeout',
            0x04: 'Sequence error',
            0x05: 'Motor disconnected',
            0x06: 'Calibration timeout',
            0x07: 'No stall detected',
        }
        return names[code] || `Error 0x${code.toString(16).padStart(2, '0')}`
    }

    // ─── Calibration status (async, ~50ms cadence) ───
    // Wire format / field list: gearcontrol.CalibStatus in
    // app/go/engine/handlers/gearcontrol/types.go. The Go side already maps
    // errorReason→errorReasonName using the authoritative protocol table, so we
    // just use evt.errorReasonName (fallback to local map if missing).
    type GearCalibStatus = {
        gearId: number
        phase: number
        phaseName: string
        current_mA: number
        peak_mA: number
        stall_mA: number
        finished: boolean
        errorReason: number
        errorReasonName: string
    }

    function handleCalibStatus(evt: GearCalibStatus) {
        const i = evt.gearId
        if (i < 0 || i >= gearCount) return

        liveCurrent_mA[i] = evt.current_mA

        // Map firmware phase → UI bucket (see calibPhaseName in Go types.go).
        // 1 clear-run, 2 clear-settle → 'clear'
        // 3 deploy-run, 5 retract-run → 'deploy' (motor running either direction)
        // 4 mid-settle → 'settle'
        // 9 opening-doors, 10 closing-doors → 'deploy' (show as moving)
        const phaseMap: Record<number, CalibPhase> = {
            1: 'clear', 2: 'clear',
            3: 'deploy', 4: 'settle', 5: 'deploy',
            9: 'deploy', 10: 'deploy',
        }
        if (phaseMap[evt.phase]) calibPhases[i] = phaseMap[evt.phase]

        if (evt.finished) {
            if (evt.phase === 7) { // ERROR
                calibStates[i] = 'error'
                calibErrors[i] = evt.errorReasonName || gearErrorReasonName(evt.errorReason)
            } else if (evt.phase === 6) { // COMPLETE
                calibStates[i] = 'calibrated'
                calibErrors[i] = ''
            } else if (evt.phase === 8) { // CANCELLED
                calibStates[i] = 'uncalibrated'
                calibErrors[i] = ''
            }
            calibPhases[i] = 'none'
        }

        // Trigger reactivity
        calibStates = calibStates
        calibErrors = calibErrors
        calibPhases = calibPhases
        liveCurrent_mA = liveCurrent_mA
    }

    onMount(() => {
        EventsOn('gearcontrol:status', handleStatusBroadcast)
        EventsOn('gearcontrol:calib', handleCalibStatus)
    })

    onDestroy(() => {
        EventsOff('gearcontrol:status')
        EventsOff('gearcontrol:calib')
    })

    // ─── Config ───
    function configReload() { SendCommand('config.reload') }
    function configStatus() { SendCommand('config.status') }
    function refreshStatus() { SendCommand('status') }

    // ─── Verification ─────────────────────────────────────────────────
    // Mirrors the LightFX pattern (Rule 24): a board-specific verifier
    // produces a path-indexed map of issues; the template binds verify-error
    // / verify-warn classes via sev(path) so any field with a problem renders
    // with a red/yellow border.
    const gcVerifier = new GearControlConfigVerifier()
    let saveDialogOpen = false
    let saveVerifyResult: VerifyResult = EMPTY_RESULT
    let saveConfigText = ''

    function buildGearControlConfig(): GearControlConfig {
        return {
            pins: pinConfigs.map(p => ({
                role: p.role, channel: p.channel, gear_id: p.gear_id, threshold_us: p.threshold_us,
            })),
            gears: gearConfigs.map((gc, gi) => ({
                enabled: gearEnabled[gi],
                calibrated: calibStates[gi] === 'calibrated',
                timeout_ms: gc.timeout_ms,
                door: { ...doorConfigs[gi] },
                doorPinCount: doorPinsPerGear[gi],
            })),
            yaw: {
                enabled: yawEnabled,
                gearId: yawGearId,
                neutral_us: yawConfig.neutral_us,
                min_us: yawConfig.min_us,
                max_us: yawConfig.max_us,
            },
            isSlave: isHubFX,
        }
    }

    let liveResult: VerifyResult = EMPTY_RESULT
    $: {
        // Touch every reactive dep so Svelte re-runs verification on any edit.
        void pinConfigs; void gearConfigs; void doorConfigs; void gearEnabled
        void calibStates; void yawConfig; void yawGearId; void isHubFX
        liveResult = gcVerifier.verify(buildGearControlConfig())
    }

    let sev: (path: string) => string | null
    $: sev = (() => {
        void liveResult
        return (path: string): string | null => gcVerifier.severityForPath(path)
    })()

    function openSaveDialog() {
        saveVerifyResult = gcVerifier.verify(buildGearControlConfig())
        // GearControl config text is plumbed server-side via config.save —
        // we don't currently round-trip the YAML through the GUI, so leave
        // the dialog's text panel empty. The verify panel still gates save.
        saveConfigText = ''
        saveDialogOpen = true
    }

    // ─── Servo Widget defs ───
    const gcServos = [
        { id: 0, name: 'Nose Door A' },
        { id: 1, name: 'Nose Door B' },
        { id: 2, name: 'Left Door A' },
        { id: 3, name: 'Left Door B' },
        { id: 4, name: 'Right Door A' },
        { id: 5, name: 'Right Door B' },
        { id: 6, name: 'Yaw' },
        { id: 7, name: 'Spare' },
    ]
</script>

<div class="tab-root">
    <!-- ═══ Title Bar ═══ -->
    <div class="tab-title-bar">
        <h2>{boardLabel}</h2>
        <div class="title-actions">
            <button class="small" on:click={configReload} disabled={controlsDisabled} title="Reload config from flash">↻ Reload</button>
            <button class="small" on:click={openSaveDialog} disabled={controlsDisabled} title="Verify and save config to flash">💾 Save…</button>
            {#if liveResult.counts.error > 0}
                <span class="verify-badge error" title="{liveResult.counts.error} error(s) — see Save dialog">{liveResult.counts.error} ✕</span>
            {:else if liveResult.counts.warning > 0}
                <span class="verify-badge warning" title="{liveResult.counts.warning} warning(s)">{liveResult.counts.warning} ⚠</span>
            {/if}
            <button class="small" on:click={configStatus} disabled={controlsDisabled} title="Config load status">Config</button>
            <button class="small" on:click={refreshStatus} disabled={controlsDisabled} title="Refresh board status">Status</button>
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
        <div class="content-wrap">
            <div class="two-col">
                <!-- ═══════════  LEFT COLUMN  ═══════════ -->
                <div class="col">

                    <!-- ── Channel Toggles ── -->
                    <!-- Per-channel enable/disable lives here so it gates everything below it: -->
                    <!-- the per-gear cards on the right collapse, and door / yaw_output dropdowns -->
                    <!-- in Pin Mapping exclude disabled gears. Disabling is non-persistent: -->
                    <!-- it sends `disable <id>` over the wire and trusts the broadcast bit -->
                    <!-- (configFlags & 0x80) to confirm. -->
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="6" width="20" height="12" rx="2"/><circle cx="8" cy="12" r="2"/><circle cx="16" cy="12" r="2"/></svg> Channel Toggles</h3>
                            <span class="header-tag {gearEnabled.every(e => e) ? 'ok' : 'warn'}">
                                {gearEnabled.filter(e => e).length}/{gearCount} active
                            </span>
                        </div>
                        <div class="channel-toggles">
                            {#each gearNames as name, id}
                                <button class="channel-toggle"
                                        class:on={gearEnabled[id]}
                                        class:off={!gearEnabled[id]}
                                        disabled={controlsDisabled}
                                        title={gearEnabled[id] ? `Disable Gear ${id}` : `Enable Gear ${id}`}
                                        on:click={() => gearEnabled[id] ? gearDisable(id) : gearEnable(id)}>
                                    <span class="channel-toggle-dot"
                                          class:dot-on={gearEnabled[id]}
                                          class:dot-off={!gearEnabled[id]}></span>
                                    <span class="channel-toggle-name">{id} — {name}</span>
                                    <span class="channel-toggle-state">{gearEnabled[id] ? 'ON' : 'OFF'}</span>
                                </button>
                            {/each}
                        </div>
                    </section>

                    <!-- ── Pin Mapping ── -->
                    <!-- Mode (Direct vs Slave) is inferred from connection — no manual toggle. -->
                    <section class="card">
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><path d="M3 9h18"/><path d="M3 15h18"/><path d="M9 3v18"/></svg> Pin Mapping</h3>
                            <span class="header-tag {isHubFX ? 'active' : 'ok'}">
                                {isHubFX ? 'Slave (via HubFX)' : 'Direct'}
                            </span>
                        </div>

                        <div class="pin-map-list">
                            {#each pinConfigs as pin, idx}
                                <div class="pin-row"
                                     class:pin-unused={pin.role === 'unused'}
                                     class:verify-error={sev(`pins[${idx}]`) === 'error'}
                                     class:verify-warn={sev(`pins[${idx}]`) === 'warning'}>
                                    <div class="pin-id">
                                        <span class="pin-slot">{pinSlots[idx]}</span>
                                        <span class="pin-gpio">{pinGPIOs[idx]}</span>
                                    </div>
                                    <select bind:value={pin.role} class="field-input pin-role-select"
                                            disabled={controlsDisabled}>
                                        {#each pinRoleOptions as role}
                                            <option value={role}>{pinRoleLabels[role]}</option>
                                        {/each}
                                    </select>
                                    <div class="pin-params">
                                        {#if pin.role === 'door'}
                                            <div class="pin-param">
                                                <span class="pin-param-label">Ch</span>
                                                <select bind:value={pin.channel}
                                                        class="field-input pin-param-input"
                                                        disabled={controlsDisabled}>
                                                    {#each enabledGearOptions(pin.channel) as opt}
                                                        <option value={opt.id}>{opt.id + 1} — {opt.name}</option>
                                                    {/each}
                                                </select>
                                            </div>
                                            <!-- Live servo position. Door A = idx 0, Door B = idx 1; -->
                                            <!-- pick whichever leg this pin owns. We approximate by counting -->
                                            <!-- door pins for this channel before this pin index. -->
                                            {#if gearEnabled[pin.channel]}
                                                <span class="pin-live"
                                                      title="Live servo position"
                                                      class:pin-live-warn={!gearEnabled[pin.channel]}>
                                                    {liveDoor_us[pin.channel]?.[doorLegIndex(idx, pin.channel)] || '—'} µs
                                                </span>
                                            {:else}
                                                <span class="pin-live pin-live-warn">channel off</span>
                                            {/if}
                                        {:else if pin.role === 'gear_input'}
                                            <div class="pin-param">
                                                <span class="pin-param-label">Threshold</span>
                                                <input type="number" bind:value={pin.threshold_us}
                                                       class="field-input pin-param-input" min="800" max="2200" step="50"
                                                       disabled={controlsDisabled} />
                                                <span class="unit">µs</span>
                                            </div>
                                            <!-- Live: aggregate gear action (deploy/retract/idle) — proxies the input. -->
                                            <span class="pin-live">{aggregateGearActionLabel}</span>
                                        {:else if pin.role === 'yaw_output'}
                                            <div class="pin-param">
                                                <span class="pin-param-label">Gear</span>
                                                <select bind:value={pin.gear_id}
                                                        class="field-input pin-param-input"
                                                        disabled={controlsDisabled}>
                                                    {#each enabledGearOptions(pin.gear_id) as opt}
                                                        <option value={opt.id}>{opt.id + 1} — {opt.name}</option>
                                                    {/each}
                                                </select>
                                            </div>
                                            <span class="pin-live">{liveYaw_us || '—'} µs</span>
                                        {:else if pin.role === 'yaw_input'}
                                            <span class="pin-param-hint dim">PWM input (standalone only)</span>
                                            <!-- Live yaw mirrors input → output, so show the same value. -->
                                            <span class="pin-live">{liveYaw_us || '—'} µs</span>
                                        {:else}
                                            <span class="pin-param-hint dim">—</span>
                                        {/if}
                                    </div>
                                </div>
                            {/each}
                        </div>

                        {#if !hasGearInput && !isHubFX}
                            <div class="pin-map-hint warn">
                                ⚠ No gear input pin assigned — standalone operation requires a gear input
                            </div>
                        {/if}
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

                    <!-- ── Yaw Steering ── -->
                    {#if yawEnabled}
                    <section class="card"
                             class:verify-error={sev('yaw') === 'error'}
                             class:verify-warn={sev('yaw') === 'warning'}>
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><polygon points="16.24 7.76 14.12 14.12 7.76 16.24 9.88 9.88 16.24 7.76"/></svg> Yaw Steering</h3>
                            <span class="header-tag ok">Gear {yawGearId} — {gearNames[yawGearId]}</span>
                        </div>

                        <div class="form-grid cols-3">
                            <div class="form-field">
                                <span class="field-label">Gear ID</span>
                                <input type="number" bind:value={yawGearId}
                                       class="field-input" min="0" max="2"
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Neutral µs</span>
                                <input type="number" bind:value={yawConfig.neutral_us}
                                       class="field-input" min="500" max="2500" step="10"
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Min µs</span>
                                <input type="number" bind:value={yawConfig.min_us}
                                       class="field-input" min="500" max="2500" step="10"
                                       disabled={controlsDisabled} />
                            </div>
                        </div>
                        <div class="form-grid cols-3" style="margin-top: 8px;">
                            <div class="form-field">
                                <span class="field-label">Max µs</span>
                                <input type="number" bind:value={yawConfig.max_us}
                                       class="field-input" min="500" max="2500" step="10"
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Speed</span>
                                <input type="number" bind:value={yawConfig.speed}
                                       class="field-input" min="100" max="10000" step="100"
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <!-- svelte-ignore a11y-label-has-associated-control -->
                                <label class="toggle" style="margin-top: 18px">
                                    <input type="checkbox" bind:checked={yawConfig.reversed}
                                           disabled={controlsDisabled} />
                                    <span class="toggle-text">Reversed</span>
                                </label>
                            </div>
                        </div>
                        <div class="apply-row">
                            <button class="small primary" disabled={controlsDisabled}
                                    on:click={applyYawConfig}>Apply Yaw Config</button>
                        </div>

                        <div class="subsection-inline">
                            <h4>Yaw Input</h4>
                            <div class="slider-row">
                                <input type="range" bind:value={yawPosition_us} min="1000" max="2000" step="10"
                                       class="slider wide" disabled={controlsDisabled} />
                                <span class="slider-val">{yawPosition_us} µs</span>
                                <button class="small primary" disabled={controlsDisabled}
                                        on:click={setYaw}>Set</button>
                                <button class="small" disabled={controlsDisabled}
                                        on:click={resetYawPosition}>Reset</button>
                            </div>
                        </div>
                    </section>
                    {/if}

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
                                <span class="field-label">Chemistry</span>
                                <select bind:value={batteryChemistry} class="field-input"
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
                                           class="field-input narrow" min="0" max="6" step="1"
                                           title="0 = auto-detect"
                                           disabled={controlsDisabled} />
                                    <span class="unit">{batteryCellCount === 0 ? 'auto' : 'S'}</span>
                                </div>
                            </div>
                            {#if batteryCellCount === 0}
                                <span class="field-hint" style="margin-left: 16px; align-self: flex-end;">
                                    Auto-detect on connect
                                </span>
                            {/if}
                        </div>
                    </section>
                </div>

                <!-- ═══════════  RIGHT COLUMN  ═══════════ -->
                <div class="col">

                    <!-- ── Per-Gear Cards (one per gear) ── -->
                    <!-- Disabled channels are hidden entirely; toggles live in the -->
                    <!-- "Channel Toggles" frame in the left column. -->
                    {#each gearNames as name, id}
                        {#if gearEnabled[id]}
                        <section class="card gear-card-wrap"
                                 class:card-error={calibStates[id] === 'error'}
                                 class:card-warn={calibStates[id] === 'uncalibrated'}
                                 class:card-active={gearActions[id] !== 'idle'}
                                 class:verify-error={sev(`gears[${id}]`) === 'error'}
                                 class:verify-warn={sev(`gears[${id}]`) === 'warning'}>

                            <div class="card-header">
                                <div class="gear-header-left">
                                    <span class="status-dot"
                                          class:dot-deploy={gearActions[id] === 'deploying'}
                                          class:dot-retract={gearActions[id] === 'retracting'}
                                          class:dot-idle={gearActions[id] === 'idle' && calibStates[id] === 'calibrated'}
                                          class:dot-error={calibStates[id] === 'error'}
                                          class:dot-uncal={calibStates[id] === 'uncalibrated' || calibStates[id] === 'calibrating'}></span>
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
                            </div>

                            <!-- Per-gear action buttons -->
                            <div class="gear-actions-row">
                                <button class="small primary" title="Deploy"
                                        disabled={controlsDisabled || calibStates[id] !== 'calibrated'}
                                        on:click={() => gearDeploy(id)}>
                                    ▼ Deploy
                                </button>
                                <button class="small" title="Retract"
                                        disabled={controlsDisabled || calibStates[id] !== 'calibrated'}
                                        on:click={() => gearRetract(id)}>
                                    ▲ Retract
                                </button>
                                <button class="small danger" title="Stop"
                                        disabled={controlsDisabled}
                                        on:click={() => gearStop(id)}>
                                    ■ Stop
                                </button>
                                <button class="small" disabled={controlsDisabled}
                                        on:click={() => gearReset(id)} title="Clear error">↻ Reset</button>
                            </div>

                            <!-- Retract Motor -->
                            <div class="subsection-inline">
                                <h4>Retract Motor</h4>

                                <!-- Live current + calibrated stall readout -->
                                <div class="motor-readout">
                                    <div class="readout-item">
                                        <span class="readout-label">Current</span>
                                        <span class="readout-value" class:readout-active={calibStates[id] === 'calibrating'}>
                                            {liveCurrent_mA[id]} <span class="readout-unit">mA</span>
                                        </span>
                                    </div>
                                    <div class="readout-item">
                                        <span class="readout-label">Stall Threshold</span>
                                        <span class="readout-value" class:readout-ok={gearConfigs[id].stallCurrent_mA > 0}
                                              class:readout-dim={gearConfigs[id].stallCurrent_mA === 0}>
                                            {gearConfigs[id].stallCurrent_mA > 0 ? gearConfigs[id].stallCurrent_mA : '—'}
                                            {#if gearConfigs[id].stallCurrent_mA > 0}<span class="readout-unit">mA</span>{/if}
                                        </span>
                                    </div>
                                    {#if calibStates[id] === 'calibrating'}
                                        <div class="readout-item">
                                            <span class="readout-label">Phase</span>
                                            <span class="readout-value readout-active">
                                                {calibPhases[id] === 'clear' ? '⟳ Clearing' : calibPhases[id] === 'deploy' ? '▼ Deploying' : calibPhases[id] === 'settle' ? '◇ Settling' : '—'}
                                            </span>
                                        </div>
                                    {/if}
                                    {#if calibStates[id] === 'error'}
                                        <div class="readout-item readout-error">
                                            <span class="readout-label">Error</span>
                                            <span class="readout-value">{calibErrors[id] || 'Calibration failed'}</span>
                                        </div>
                                    {/if}
                                </div>

                                <!-- Current bar (visual gauge) -->
                                {#if calibStates[id] === 'calibrating' || liveCurrent_mA[id] > 0}
                                <div class="current-bar-track">
                                    <div class="current-bar-fill"
                                         class:current-stall={gearConfigs[id].stallCurrent_mA > 0 && liveCurrent_mA[id] >= gearConfigs[id].stallCurrent_mA}
                                         style="width: {Math.min(100, (liveCurrent_mA[id] / Math.max(gearConfigs[id].stallCurrent_mA || 2000, 500)) * 100)}%">
                                    </div>
                                    {#if gearConfigs[id].stallCurrent_mA > 0}
                                    <div class="current-bar-threshold"
                                         style="left: {Math.min(100, (gearConfigs[id].stallCurrent_mA / Math.max(gearConfigs[id].stallCurrent_mA || 2000, 500)) * 100)}%">
                                    </div>
                                    {/if}
                                </div>
                                {/if}

                                <div class="form-grid cols-2" style="margin-top: 8px">
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

                            <!-- Door Positions (shown only when door servos assigned) -->
                            {#if gearHasDoors[id]}
                            <div class="subsection-inline"
                                 class:verify-error={sev(`gears[${id}].door.0`) === 'error' || sev(`gears[${id}].door.1`) === 'error'}
                                 class:verify-warn={sev(`gears[${id}].door.0`) === 'warning' || sev(`gears[${id}].door.1`) === 'warning'}>
                                <h4>Door Positions <span class="door-count">{doorPinsPerGear[id]} servo{doorPinsPerGear[id] > 1 ? 's' : ''}</span></h4>

                                <!-- Live servo positions (from STATUS broadcast). -->
                                <!-- Bar fill is the value's position within [min,max] of -->
                                <!-- the configured open/close range; clamps when outside. -->
                                <div class="door-live">
                                    <div class="door-live-row">
                                        <span class="door-live-label">Door A live</span>
                                        <div class="door-live-bar-track">
                                            <div class="door-live-bar-fill"
                                                 style="width: {servoPct(liveDoor_us[id][0], doorConfigs[id].open0_us, doorConfigs[id].close0_us)}%"></div>
                                        </div>
                                        <span class="door-live-value">{liveDoor_us[id][0] || '—'} µs</span>
                                    </div>
                                    {#if doorPinsPerGear[id] > 1}
                                    <div class="door-live-row">
                                        <span class="door-live-label">Door B live</span>
                                        <div class="door-live-bar-track">
                                            <div class="door-live-bar-fill"
                                                 style="width: {servoPct(liveDoor_us[id][1], doorConfigs[id].open1_us, doorConfigs[id].close1_us)}%"></div>
                                        </div>
                                        <span class="door-live-value">{liveDoor_us[id][1] || '—'} µs</span>
                                    </div>
                                    {/if}
                                </div>

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
                                    {#if doorPinsPerGear[id] > 1}
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
                                    {/if}
                                </div>
                                <div class="apply-row">
                                    <button class="small" disabled={controlsDisabled}
                                            on:click={() => applyDoorConfig(id)}>Apply Positions</button>
                                </div>
                            </div>

                            <!-- Door Sequencing -->
                            <div class="subsection-inline">
                                <h4>Door Sequencing</h4>
                                <div class="form-grid cols-3">
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
                                            on:click={() => applyDoorMode(id)}>Apply Mode</button>
                                </div>
                            </div>
                            {:else}
                            <div class="no-doors-hint">No door servos assigned to this gear in Pin Mapping</div>
                            {/if}


                        </section>
                        {/if}
                    {/each}

                    <!-- ── Servo Widget (shared) ── -->
                    <ServoWidget servos={gcServos} disabled={controlsDisabled}
                                 label="Door & Yaw Servos" />
                </div>
            </div>
        </div>
    </div>
</div>

<SaveConfigDialog
    boardType="gearcontrol"
    boardLabel={boardLabel}
    verifyResult={saveVerifyResult}
    configText={saveConfigText}
    open={saveDialogOpen}
    onSave={() => { SendCommand('config.save'); saveDialogOpen = false }}
    onClose={() => { saveDialogOpen = false }}
/>

<style>
    /* GearControlTab-specific — shared styles in style.css */

    /* ─── Card Variants ─── */
    .card { transition: border-color 0.2s; }
    .card-warn  { border-color: color-mix(in srgb, var(--warning, #d7ba7d) 60%, transparent); }
    .card-error { border-color: color-mix(in srgb, var(--error) 60%, transparent) !important; }
    .card-active { border-color: color-mix(in srgb, var(--ok, #4ec9b0) 50%, transparent); }

    /* ─── Verification highlights (Rule 24) ─── */
    /* Mirrors LightFXTab — any UI block bound with class:verify-error / verify-warn
       picks up the border + glow set here, regardless of card/row/section class. */
    .verify-error {
        border-color: var(--error) !important;
        background: color-mix(in srgb, var(--error) 8%, var(--bg-surface)) !important;
        box-shadow: 0 0 0 1px color-mix(in srgb, var(--error) 35%, transparent);
    }
    .verify-warn {
        border-color: var(--warning, #d7ba7d) !important;
        box-shadow: 0 0 0 1px color-mix(in srgb, var(--warning, #d7ba7d) 35%, transparent);
    }
    .verify-badge {
        font-size: 11px;
        font-weight: 700;
        padding: 1px 7px;
        border-radius: 8px;
        line-height: 1.4;
        font-family: var(--font-mono);
    }
    .verify-badge.error   { background: color-mix(in srgb, var(--error) 20%, transparent); color: var(--error); }
    .verify-badge.warning { background: color-mix(in srgb, var(--warning, #d7ba7d) 20%, transparent); color: var(--warning, #d7ba7d); }

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

    /* ─── Channel Toggles ─── */
    .channel-toggles {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }
    .channel-toggle {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 8px 12px;
        border-radius: 4px;
        background: var(--bg-raised);
        border: 1px solid var(--border);
        cursor: pointer;
        transition: background 0.15s, border-color 0.15s, opacity 0.15s;
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text);
    }
    .channel-toggle.on {
        border-color: color-mix(in srgb, var(--ok, #4ec9b0) 60%, transparent);
        background: color-mix(in srgb, var(--ok, #4ec9b0) 6%, var(--bg-raised));
    }
    .channel-toggle.off {
        border-color: color-mix(in srgb, var(--text-dim) 30%, transparent);
        background: var(--bg-raised);
        opacity: 0.7;
    }
    .channel-toggle:hover:not(:disabled) { border-color: var(--accent); }
    .channel-toggle-dot {
        width: 9px; height: 9px; border-radius: 50%;
        flex-shrink: 0;
        background: var(--text-dim);
    }
    .channel-toggle-dot.dot-on  { background: var(--ok, #4ec9b0); box-shadow: 0 0 6px color-mix(in srgb, var(--ok, #4ec9b0) 50%, transparent); }
    .channel-toggle-dot.dot-off { background: var(--text-dim); }
    .channel-toggle-name { flex: 1; font-weight: 600; }
    .channel-toggle-state {
        font-size: 10px;
        font-weight: 700;
        padding: 1px 8px;
        border-radius: 3px;
        background: color-mix(in srgb, var(--border) 40%, transparent);
        color: var(--text-dim);
    }
    .channel-toggle.on .channel-toggle-state {
        background: color-mix(in srgb, var(--ok, #4ec9b0) 18%, transparent);
        color: var(--ok, #4ec9b0);
    }

    /* ─── Pin Mapping live readouts ─── */
    .pin-live {
        font-family: var(--font-mono);
        font-size: 11px;
        font-weight: 600;
        color: var(--text);
        padding: 1px 8px;
        border-radius: 3px;
        background: color-mix(in srgb, var(--accent) 8%, transparent);
        margin-left: auto;
        white-space: nowrap;
    }
    .pin-live.pin-live-warn {
        color: var(--warning, #d7ba7d);
        background: color-mix(in srgb, var(--warning, #d7ba7d) 10%, transparent);
    }

    /* ─── Live door servo bars ─── */
    .door-live {
        display: flex;
        flex-direction: column;
        gap: 4px;
        margin-bottom: 8px;
        padding: 6px 10px;
        background: var(--bg-raised);
        border-radius: 4px;
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }
    .door-live-row {
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .door-live-label {
        font-size: 10px;
        font-weight: 600;
        color: var(--text-dim);
        font-family: var(--font-mono);
        min-width: 80px;
        text-transform: uppercase;
        letter-spacing: 0.4px;
    }
    .door-live-bar-track {
        flex: 1;
        height: 5px;
        background: var(--bg-input);
        border-radius: 3px;
        overflow: hidden;
    }
    .door-live-bar-fill {
        height: 100%;
        background: var(--accent);
        border-radius: 3px;
        transition: width 0.2s;
    }
    .door-live-value {
        font-family: var(--font-mono);
        font-size: 11px;
        font-weight: 700;
        color: var(--text);
        min-width: 64px;
        text-align: right;
    }

    /* ─── Aggregate Buttons ─── */
    .agg-buttons {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        margin-bottom: 10px;
    }

    .action-btn { font-size: 12px; }
    .icon { font-size: 11px; }
    .icon.down { color: var(--ok, #4ec9b0); }
    .icon.up { color: var(--accent); }

    /* ─── Gear Status Strip ─── */
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

    /* ─── Status Dot ─── */
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

    /* ─── Per-Gear Actions ─── */
    .gear-actions-row {
        display: flex;
        gap: 6px;
        align-items: center;
        flex-wrap: wrap;
    }

    /* ─── Calibration ─── */
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

    .spin {
        display: inline-block;
        animation: spin 1.5s linear infinite;
    }

    @keyframes spin {
        0%   { transform: rotate(0deg); }
        100% { transform: rotate(360deg); }
    }

    /* ─── Battery ─── */
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
        animation: pulse 0.8s ease-in-out infinite;
    }

    /* ─── Pin Mapping ─── */
    .pin-map-list {
        display: flex;
        flex-direction: column;
        gap: 4px;
    }

    .pin-row {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 5px 8px;
        border-radius: 4px;
        background: var(--bg-raised);
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
        transition: opacity 0.2s;
    }

    .pin-row.pin-unused { opacity: 0.45; }

    .pin-id {
        display: flex;
        flex-direction: column;
        align-items: center;
        min-width: 40px;
        flex-shrink: 0;
    }

    .pin-slot {
        font-size: 11px;
        font-weight: 700;
        color: var(--text);
        font-family: var(--font-mono);
    }

    .pin-gpio {
        font-size: 9px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .pin-role-select {
        width: 120px;
        flex-shrink: 0;
    }

    .pin-params {
        display: flex;
        align-items: center;
        gap: 8px;
        flex: 1;
        min-width: 0;
    }

    .pin-param {
        display: flex;
        align-items: center;
        gap: 4px;
    }

    .pin-param-label {
        font-size: 10px;
        font-weight: 600;
        color: var(--text-dim);
        font-family: var(--font-mono);
        white-space: nowrap;
    }

    .pin-param-input {
        width: 120px !important;
        min-width: 60px;
    }

    .pin-param-hint {
        font-size: 10px;
        font-weight: 600;
        color: var(--text);
        font-family: var(--font-mono);
    }

    .pin-param-hint.dim {
        color: var(--text-dim);
    }

    .pin-map-hint {
        margin-top: 8px;
        padding: 6px 10px;
        border-radius: 4px;
        font-size: 11px;
        font-weight: 600;
    }

    .pin-map-hint.warn {
        color: var(--warning, #d7ba7d);
        background: color-mix(in srgb, var(--warning, #d7ba7d) 8%, var(--bg-surface));
        border: 1px solid color-mix(in srgb, var(--warning, #d7ba7d) 25%, transparent);
    }

    /* ─── Door count badge / no-doors hint ─── */
    .door-count {
        font-size: 10px;
        font-weight: 600;
        color: var(--text-dim);
        margin-left: 4px;
    }

    .no-doors-hint {
        padding: 8px 12px;
        font-size: 11px;
        color: var(--text-dim);
        font-style: italic;
        text-align: center;
        border: 1px dashed color-mix(in srgb, var(--border) 50%, transparent);
        border-radius: 4px;
        margin-top: 4px;
    }

    /* ─── Form Overrides ─── */
    .form-grid { grid-template-columns: 1fr 1fr; }

    /* ─── Motor Readout (live current + stall threshold) ─── */
    .motor-readout {
        display: flex;
        gap: 16px;
        flex-wrap: wrap;
        padding: 8px 10px;
        background: var(--bg-raised);
        border-radius: 4px;
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
        margin-bottom: 8px;
    }

    .readout-item {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .readout-item.readout-error {
        color: var(--error);
    }

    .readout-label {
        font-size: 9px;
        font-weight: 600;
        text-transform: uppercase;
        letter-spacing: 0.5px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .readout-value {
        font-size: 16px;
        font-weight: 700;
        font-family: var(--font-mono);
        color: var(--text);
    }

    .readout-value.readout-active {
        color: var(--accent);
    }

    .readout-value.readout-ok {
        color: var(--ok, #4ec9b0);
    }

    .readout-value.readout-dim {
        color: var(--text-dim);
        font-size: 14px;
    }

    .readout-unit {
        font-size: 11px;
        font-weight: 600;
        color: var(--text-dim);
    }

    /* ─── Current bar gauge ─── */
    .current-bar-track {
        position: relative;
        height: 4px;
        background: var(--bg-input);
        border-radius: 2px;
        overflow: visible;
        margin-bottom: 4px;
    }

    .current-bar-fill {
        height: 100%;
        background: var(--accent);
        border-radius: 2px;
        transition: width 0.3s;
        min-width: 1px;
    }

    .current-bar-fill.current-stall {
        background: var(--error);
    }

    .current-bar-threshold {
        position: absolute;
        top: -2px;
        width: 2px;
        height: 8px;
        background: var(--ok, #4ec9b0);
        border-radius: 1px;
        transform: translateX(-1px);
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

    /* ─── Toggle ─── */
    .toggle-text {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text);
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

    /* ─── Title Actions ─── */
    .title-actions {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-left: auto;
    }

    /* ─── Direct Mode Warning ─── */
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
    .direct-warning-text {
        flex: 1;
    }
    .direct-warning button {
        flex-shrink: 0;
    }
</style>
