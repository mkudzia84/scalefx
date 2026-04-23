<!-- ScaleFX Studio — GearControl Tab -->
<!-- Left: global settings (controls, battery, yaw).  Right: per-channel setup with doors & servos. -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { SendCommand, UploadConfig } from '../../../wailsjs/go/main/App'
    import { EventsOn, EventsOff } from '../../../wailsjs/runtime/runtime'
    import { connectionInfo } from '../stores'
    import SaveConfigDialog from '../dialogs/SaveConfigDialog.svelte'
    import ServoCalibrationDialog from '../dialogs/ServoCalibrationDialog.svelte'
    import GearControlBoardDialog from '../dialogs/GearControlBoardDialog.svelte'
    import { GearControlConfigVerifier, type GearControlConfig } from '../config/gearcontrol-verifier'
    import {
        generateGearControlYaml, parseGearControlYaml,
        type GearControlFullConfig,
    } from '../config/config-yaml-gen'
    import { EMPTY_RESULT, type VerifyResult } from '../config/config-verifier'
    import type { BoardConfigDriver } from '../config/board-driver'
    import { createLivePusher, pushBadgeText } from '../live-push'
    import { autoLoadOnConnect, loadConfigFromDevice } from '../config/config-loader'

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

    // Helpers ignore disabled channels — a disabled gear can't be calibrated and
    // shouldn't trigger "Calibration required" warnings or block aggregate Deploy/Retract.
    $: anyUncalibrated = calibStates.some((s, i) => gearEnabled[i] && s !== 'calibrated')
    $: allCalibrated = calibStates.every((s, i) => !gearEnabled[i] || s === 'calibrated')
    $: hasErrors = calibStates.some((s, i) => gearEnabled[i] && s === 'error')

    // ─── Aggregate actions ───
    function gearAllDeploy()  { SendCommand('gear:deploy all'); gearActions = gearActions.map(() => 'deploying' as GearAction) }
    function gearAllRetract() { SendCommand('gear:retract all'); gearActions = gearActions.map(() => 'retracting' as GearAction) }
    function gearAllStop()    { SendCommand('gear:stop all'); gearActions = gearActions.map(() => 'idle' as GearAction) }
    function gearResetAll()   {
        SendCommand('gear:reset all')
        calibStates = calibStates.map(s => s === 'error' ? 'uncalibrated' as CalibState : s)
        calibErrors = ['', '', '']
    }

    // ─── Calibration ───
    function calibrateAll() {
        SendCommand(`gear:calibrate all ${calibTimeout_s}`)
        calibStates = calibStates.map(() => 'calibrating' as CalibState)
        calibErrors = ['', '', '']
        liveCurrent_mA = [0, 0, 0]
        calibPhases = ['clear', 'clear', 'clear']
    }
    function calibCancelAll() {
        SendCommand('gear:calibrate.cancel all')
        calibStates = calibStates.map(s => s === 'calibrating' ? 'uncalibrated' as CalibState : s)
    }
    function calibrate(id: number) {
        SendCommand(`gear:calibrate ${id} ${calibTimeout_s}`)
        calibStates[id] = 'calibrating'
        calibErrors[id] = ''
        liveCurrent_mA[id] = 0
        calibPhases[id] = 'clear'
        calibStates = calibStates
        liveCurrent_mA = liveCurrent_mA
        calibPhases = calibPhases
    }
    function calibCancel(id: number) {
        SendCommand(`gear:calibrate.cancel ${id}`)
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
        SendCommand('gear:reset all')
        calibStates = calibStates.map(() => 'uncalibrated' as CalibState)
        calibErrors = ['', '', '']
    }

    // ─── Per-gear actions ───
    function gearDeploy(id: number)  { SendCommand(`gear:deploy ${id}`); gearActions[id] = 'deploying'; gearActions = gearActions }
    function gearRetract(id: number) { SendCommand(`gear:retract ${id}`); gearActions[id] = 'retracting'; gearActions = gearActions }
    function gearStop(id: number)    { SendCommand(`gear:stop ${id}`); gearActions[id] = 'idle'; gearActions = gearActions }
    function gearEnable(id: number)  { SendCommand(`gear:enable ${id}`); gearEnabled[id] = true; gearEnabled = gearEnabled }
    function gearDisable(id: number) { SendCommand(`gear:disable ${id}`); gearEnabled[id] = false; gearEnabled = gearEnabled }
    function gearReset(id: number) {
        SendCommand(`gear:reset ${id}`)
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
        SendCommand(`gear:gear.config ${id} ${flags} ${gc.stallCurrent_mA} ${gc.timeout_ms}`)
    }

    // ─── Door Mode (per gear) ───
    const doorModeNames = ['None', 'Single', 'Dual Sync', 'Dual Delay', 'Dual Seq']
    const doorModeValues = ['none', 'single', 'dual-sync', 'dual-delay', 'dual-seq']

    // Short descriptions for each door mode — surfaced in the per-gear card and as
    // option/select tooltips. Phase = "pre-deploy" (doors open before gear extends)
    // vs "post-deploy" (doors close after gear extends/retracts).
    const doorModeDescPre: string[] = [
        'No door movement before gear extends.',
        'Open one door (Door A only) before the gear extends.',
        'Open both doors simultaneously, then extend the gear.',
        'Open Door A, wait Delay ms, then open Door B; then extend the gear.',
        'Open Door A fully, wait until it reaches target, then open Door B; then extend the gear.',
    ]
    const doorModeDescPost: string[] = [
        'Leave doors as-is after the gear has finished moving.',
        'Close one door (Door A only) after the gear has finished moving.',
        'Close both doors simultaneously after the gear has finished moving.',
        'Close Door A, wait Delay ms, then close Door B after the gear has finished moving.',
        'Close Door A fully, wait until it reaches target, then close Door B after the gear has finished moving.',
    ]
    function doorModePreDesc(idx: number): string  { return doorModeDescPre[idx]  ?? '' }
    function doorModePostDesc(idx: number): string { return doorModeDescPost[idx] ?? '' }
    function usesDelay(preIdx: number, postIdx: number): boolean {
        return preIdx === 3 || postIdx === 3
    }

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
        SendCommand(`gear:door.mode ${id} ${doorModeValues[dm.preDeployMode]} ${doorModeValues[dm.postDeployMode]} ${dm.delay_ms}`)
    }

    // ─── Yaw Config ───
    // Yaw state lives on the yaw_output pin in pinConfigs (gear_id / neutral_us /
    // min_us / max_us / speed / reversed). The yaw frame binds directly to that
    // pin — no duplicated state. SERVO_ID_YAW is the hardware servo id for the
    // yaw output (matches gcServos[6] and firmware ServoChannel::YAW).
    const SERVO_ID_YAW = 6
    let yawPosition_us = 1500

    function applyYawConfig() {
        const pin = pinConfigs.find(p => p.role === 'yaw_output')
        if (!pin) return
        SendCommand(`gear:yaw.config ${pin.gear_id} ${pin.neutral_us} ${pin.min_us} ${pin.max_us}`)
        SendCommand(`gear:servo.config ${SERVO_ID_YAW} ${pin.min_us} ${pin.max_us} ${pin.speed} 0 0 ${pin.reversed ? 1 : 0}`)
    }
    function setYaw() { SendCommand(`gear:yaw ${yawPosition_us}`) }
    function resetYawPosition() {
        const pin = pinConfigs.find(p => p.role === 'yaw_output')
        const neutral = pin?.neutral_us ?? 1500
        yawPosition_us = neutral
        SendCommand(`gear:yaw ${neutral}`)
    }

    // Apply door servo settings (min/max/speed/reversed) for a given pin.
    // servoId = gear channel * 2 + doorIdx (0=Door A, 1=Door B).
    function applyDoorServoConfig(pinIdx: number) {
        const pin = pinConfigs[pinIdx]
        if (!pin || pin.role !== 'door') return
        const doorIdx = doorLegIndex(pinIdx, pin.channel)
        const servoId = pin.channel * 2 + doorIdx
        SendCommand(`gear:servo.config ${servoId} ${pin.min_us} ${pin.max_us} ${pin.speed} 0 0 ${pin.reversed ? 1 : 0}`)
    }

    // ─── Battery ───
    // Monitor is always on. Defaults match firmware (LiPo, auto-detect cells).
    let batteryAutoDeploy = false
    let batteryChemistry = 'lipo'
    let batteryCellCount = 0  // 0 = auto-detect
    let batteryVoltage_mV = 0
    const batteryChemistries = ['lipo', 'liion', 'nimh']
    const chemistryLabels: Record<string, string> = { lipo: 'LiPo', liion: 'Li-Ion', nimh: 'NiMH' }
    // Cell voltage bounds (mV) — must match BatteryProfiles in battery_types.h
    // (fullCharge_mV, nominal_mV, low_mV, critical_mV). We use `min = critical`
    // for the empty-end of the SOC bar and `max = fullCharge` for the full-end;
    // `nominal` mirrors the firmware's detectCellCount() divisor.
    const cellVoltageBounds: Record<string, { min: number; max: number; nominal: number }> = {
        lipo:  { min: 3000, max: 4200, nominal: 3700 },
        liion: { min: 2800, max: 4200, nominal: 3600 },
        nimh:  { min:  900, max: 1400, nominal: 1200 },
    }
    // Mirror of battery_state_machine.cpp::detectCellCount() — round(v/nominal),
    // clamped to [1,6], 0 when voltage is below the MIN_DETECT_mV threshold.
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
        const auto = batteryAutoDeploy ? 'autodeploy' : ''
        const cells = batteryCellCount > 0 ? `cells:${batteryCellCount}` : 'auto'
        SendCommand(`gear:battery ${auto} ${batteryChemistry} ${cells}`.replace(/\s+/g, ' ').trim())
    }

    // ─── Pin Mapping ───
    // gear_input is FIXED on GP0 — it lives in its own section (gearInputConfig
    // below) and is not selectable as a per-pin role.
    type PinRole = 'door' | 'yaw_input' | 'yaw_output' | 'unused'
    const pinRoleOptions: PinRole[] = ['door', 'yaw_input', 'yaw_output', 'unused']
    const pinRoleLabels: Record<PinRole, string> = {
        door: 'Door Servo', yaw_input: 'Yaw Input',
        yaw_output: 'Yaw Output', unused: 'Unused'
    }
    const pinSlots = ['SRV1', 'SRV2', 'SRV3', 'SRV4', 'SRV5', 'SRV6', 'SRV7']
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

    // Direct mode: standalone — yaw_input on pin2 (gear_input is fixed on GP0)
    const directPinPreset: PinConfig[] = [
        mkPin('yaw_input'),                      // pin1: Yaw Input
        mkPin('door', { channel: 0 }),           // pin2: Nose Door A
        mkPin('door', { channel: 0 }),           // pin3: Nose Door B
        mkPin('door', { channel: 1 }),           // pin4: Left Door
        mkPin('door', { channel: 2 }),           // pin5: Right Door
        mkPin('unused'),                         // pin6: Unused
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

    // Fixed Gear Input (GP0) — RC PWM in for deploy/retract command.
    let gearInputEnabled = true
    let gearInputThreshold_us = 1500

    $: yawPinIdx = pinConfigs.findIndex(p => p.role === 'yaw_output')
    $: yawEnabled = yawPinIdx >= 0
    $: doorPinsPerGear = [0, 1, 2].map(ch => pinConfigs.filter(p => p.role === 'door' && p.channel === ch).length)
    $: gearHasDoors = doorPinsPerGear.map(n => n > 0)

    // Per-gear door pin indices (in pinConfigs order); indexed 0=Door A, 1=Door B.
    // Used by the gear card to render the inline servo settings for each door pin.
    $: doorPinIndicesPerGear = [0, 1, 2].map(ch =>
        pinConfigs.map((p, i) => (p.role === 'door' && p.channel === ch) ? i : -1).filter(i => i >= 0)
    )

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

    // Map a pin index to its firmware servo ID (matches gearcontrol_pico.ino:
    // door IDs 0-5 = gear*2 + doorLeg, yaw = 6). Returns null for non-servo roles.
    function pinServoId(pinIdx: number): number | null {
        const p = pinConfigs[pinIdx]
        if (!p) return null
        if (p.role === 'yaw_output') return 6
        if (p.role === 'door') return p.channel * 2 + doorLegIndex(pinIdx, p.channel)
        return null
    }

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
    interface ServoLiveConfigInfo {
        min_us: number; max_us: number; speed: number; reversed: boolean
    }
    interface GearControlBroadcast {
        gears: GearStatusInfo[]; yaw_us: number; battery: BatteryBroadcastInfo
        servos?: ServoLiveConfigInfo[]
        gearInput_us?: number; gearInputThreshold_us?: number
        gearInputEnabled?: boolean; gearInputCommandDeploy?: boolean
    }

    // Live values from periodic STATUS_BROADCAST
    let liveYaw_us = 0
    let liveGearInput_us = 0
    let liveGearInputCommandDeploy = false

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

        // Gear input (fixed GP0) — live PWM pulse + decoded command
        if (data.gearInput_us !== undefined) liveGearInput_us = data.gearInput_us
        if (data.gearInputCommandDeploy !== undefined) liveGearInputCommandDeploy = data.gearInputCommandDeploy

        // Reconcile pinConfigs against the firmware's live servo configs
        // (v0.15.0+). Replaces the earlier ACK-echo carrier: the STATUS broadcast
        // is already the single source of truth, so Save → firmware → next
        // broadcast self-heals the tab. Skip the pin being calibrated (its
        // widened limits are temporary — ServoCalibrationDialog restores them).
        if (data.servos && data.servos.length >= 7) {
            let dirty = false
            for (let i = 0; i < pinConfigs.length; i++) {
                if (calibDialogOpen && i === calibTargetPin) continue
                // Don't fight a user mid-edit: skip pins whose debounced push
                // is still pending — the next broadcast after the push lands
                // will carry the new firmware-side truth.
                const p = pinConfigs[i]
                const pushKey = p.role === 'yaw_output' ? 'yaw.cfg' : `servo:pin${i}`
                if (live.isPending(pushKey)) continue
                const sid = pinServoId(i)
                if (sid === null) continue
                const s = data.servos[sid]
                if (p.min_us !== s.min_us)   { p.min_us = s.min_us; dirty = true }
                if (p.max_us !== s.max_us)   { p.max_us = s.max_us; dirty = true }
                if (p.speed !== s.speed)     { p.speed = s.speed; dirty = true }
                if (p.reversed !== s.reversed) { p.reversed = s.reversed; dirty = true }
            }
            if (dirty) pinConfigs = pinConfigs
        }

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
        const unsubAutoLoad = autoLoadOnConnect(gcDriver, ['gearcontrol'])
        return () => { unsubAutoLoad() }
    })

    onDestroy(() => {
        EventsOff('gearcontrol:status')
        EventsOff('gearcontrol:calib')
    })

    // ─── Config ───
    function configReload() { SendCommand('config.reload') }

    // ─── Live Push (Rule 24) ─────────────────────────────────────────
    // Each editable settings group (per-gear, per-door-pin servo, door
    // sequencing, yaw, battery) has a debounced auto-push: edits validate
    // locally on change, the resulting command is deduped against the last
    // value sent, and pushed ~350ms after the user stops editing. Apply
    // buttons remain as a "force resend" + sync-now affordance.
    const live = createLivePusher(SendCommand)
    const pushStatus = live.status
    const scheduleLivePush = live.schedule

    onDestroy(() => live.destroy())

    // ─── Per-group command builders + schedulers ──────────────────────

    function buildGearCmd(id: number): string | null {
        const gc = gearConfigs[id]
        if (gc.timeout_ms < 500 || gc.timeout_ms > 65000) return null
        return `gear.config ${id} 0 ${gc.stallCurrent_mA} ${gc.timeout_ms}`
    }
    function scheduleGearPush(id: number) {
        scheduleLivePush(`gear:${id}`, () => buildGearCmd(id))
    }

    function buildDoorModeCmd(id: number): string | null {
        const dm = doorModes[id]
        if (dm.delay_ms < 0 || dm.delay_ms > 5000) return null
        return `door.mode ${id} ${doorModeValues[dm.preDeployMode]} ${doorModeValues[dm.postDeployMode]} ${dm.delay_ms}`
    }
    function scheduleDoorModePush(id: number) {
        scheduleLivePush(`doormode:${id}`, () => buildDoorModeCmd(id))
    }

    function buildDoorServoCmd(pinIdx: number): string | null {
        const pin = pinConfigs[pinIdx]
        if (!pin || pin.role !== 'door') return null
        if (pin.min_us < 500 || pin.max_us > 2500 || pin.min_us >= pin.max_us) return null
        if (pin.speed < 100 || pin.speed > 10000) return null
        const doorIdx = doorLegIndex(pinIdx, pin.channel)
        const servoId = pin.channel * 2 + doorIdx
        return `servo.config ${servoId} ${pin.min_us} ${pin.max_us} ${pin.speed} 0 0 ${pin.reversed ? 1 : 0}`
    }
    function scheduleDoorServoPush(pinIdx: number) {
        scheduleLivePush(`servo:pin${pinIdx}`, () => buildDoorServoCmd(pinIdx))
    }

    function buildYawCfgCmd(): string | null {
        const pin = pinConfigs.find(p => p.role === 'yaw_output')
        if (!pin) return ''
        if (pin.min_us < 500 || pin.max_us > 2500 || pin.min_us >= pin.max_us) return null
        if (pin.neutral_us < pin.min_us || pin.neutral_us > pin.max_us) return null
        return `yaw.config ${pin.gear_id} ${pin.neutral_us} ${pin.min_us} ${pin.max_us}`
    }
    function buildYawServoCmd(): string | null {
        const pin = pinConfigs.find(p => p.role === 'yaw_output')
        if (!pin) return ''
        if (pin.min_us < 500 || pin.max_us > 2500 || pin.min_us >= pin.max_us) return null
        if (pin.speed < 100 || pin.speed > 10000) return null
        return `servo.config ${SERVO_ID_YAW} ${pin.min_us} ${pin.max_us} ${pin.speed} 0 0 ${pin.reversed ? 1 : 0}`
    }
    function scheduleYawPush() {
        scheduleLivePush('yaw.cfg', () => buildYawCfgCmd())
        scheduleLivePush('yaw.servo', () => buildYawServoCmd())
    }

    function buildBatteryCmd(): string {
        const auto = batteryAutoDeploy ? 'autodeploy' : ''
        const cells = batteryCellCount > 0 ? `cells:${batteryCellCount}` : 'auto'
        return `battery ${auto} ${batteryChemistry} ${cells}`.replace(/\s+/g, ' ').trim()
    }
    function scheduleBatteryPush() {
        scheduleLivePush('battery', () => buildBatteryCmd())
    }

    const resetPushBaseline = live.resetBaseline

    // ─── Verification ─────────────────────────────────────────────────
    // Mirrors the LightFX pattern (Rule 23): a board-specific verifier
    // produces a path-indexed map of issues; the template binds verify-error
    // / verify-warn classes via sev(path) so any field with a problem renders
    // with a red/yellow border.
    const gcVerifier = new GearControlConfigVerifier()
    let saveDialogOpen = false
    let boardDialogOpen = false

    function buildGearControlConfig(): GearControlConfig {
        const yawPin = pinConfigs.find(p => p.role === 'yaw_output')
        return {
            pins: pinConfigs.map(p => ({
                role: p.role, channel: p.channel, gear_id: p.gear_id, threshold_us: p.threshold_us,
                min_us: p.min_us, max_us: p.max_us, speed: p.speed, reversed: p.reversed,
                neutral_us: p.neutral_us,
            })),
            gears: gearConfigs.map((gc, gi) => ({
                enabled: gearEnabled[gi],
                calibrated: calibStates[gi] === 'calibrated',
                timeout_ms: gc.timeout_ms,
                doorPinCount: doorPinsPerGear[gi],
            })),
            yaw: {
                enabled: yawEnabled,
                gearId: yawPin?.gear_id ?? 0,
                neutral_us: yawPin?.neutral_us ?? 1500,
                min_us: yawPin?.min_us ?? 1000,
                max_us: yawPin?.max_us ?? 2000,
            },
            gearInput: {
                enabled: gearInputEnabled,
                threshold_us: gearInputThreshold_us,
            },
            isSlave: isHubFX,
        }
    }

    // Driver-facing snapshot: includes retracts / door_modes / battery for
    // YAML round-trip (the verifier ignores these, but the generator needs them).
    function buildGearControlFullConfig(): GearControlFullConfig {
        return {
            ...buildGearControlConfig(),
            retracts: [0, 1, 2].map(ch => ({
                channel: ch,
                enabled: gearEnabled[ch],
                stallCurrent_mA: gearConfigs[ch].stallCurrent_mA,
                timeout_ms: gearConfigs[ch].timeout_ms,
            })),
            doorModes: [0, 1, 2].map(ch => ({
                channel: ch,
                preDeploy: doorModeValues[doorModes[ch].preDeployMode],
                postDeploy: doorModeValues[doorModes[ch].postDeployMode],
                delay_ms: doorModes[ch].delay_ms,
            })),
            battery: {
                autoDeploy: batteryAutoDeploy,
                chemistry: batteryChemistry,
                cellCount: batteryCellCount,
            },
        }
    }

    function applyGearControlConfig(cfg: GearControlFullConfig) {
        const applied: string[] = []

        // Retracts → gearEnabled + gearConfigs (force new object refs so
        // `bind:value` picks up the change even when the field ends up equal).
        if (cfg.retracts && cfg.retracts.length > 0) {
            for (const r of cfg.retracts) {
                if (r.channel >= 0 && r.channel < 3) {
                    gearEnabled[r.channel] = r.enabled
                    gearConfigs[r.channel] = {
                        ...gearConfigs[r.channel],
                        stallCurrent_mA: r.stallCurrent_mA,
                        timeout_ms: r.timeout_ms,
                    }
                }
            }
            gearEnabled = [...gearEnabled]
            gearConfigs = [...gearConfigs]
            applied.push(`retracts×${cfg.retracts.length}`)
        }

        // Pins — replace the full list (preset isn't re-applied so manual edits survive)
        if (cfg.pins.length > 0) {
            pinConfigs = cfg.pins.map(p => ({
                role: p.role,
                channel: p.channel,
                min_us: p.min_us,
                max_us: p.max_us,
                speed: p.speed,
                reversed: p.reversed,
                threshold_us: p.threshold_us,
                // Yaw steering is hard-wired to the nose gear (gear 0).
                gear_id: p.role === 'yaw_output' ? 0 : p.gear_id,
                neutral_us: p.neutral_us,
            }))
            presetApplied = true  // don't clobber with the slave/direct preset
            applied.push(`pins×${cfg.pins.length}`)
        }

        // Door modes — YAML values are strings; reverse-map to index
        if (cfg.doorModes && cfg.doorModes.length > 0) {
            for (const dm of cfg.doorModes) {
                if (dm.channel < 0 || dm.channel >= 3) continue
                const preIdx  = doorModeValues.indexOf(dm.preDeploy)
                const postIdx = doorModeValues.indexOf(dm.postDeploy)
                doorModes[dm.channel] = {
                    preDeployMode:  preIdx  >= 0 ? preIdx  : doorModes[dm.channel].preDeployMode,
                    postDeployMode: postIdx >= 0 ? postIdx : doorModes[dm.channel].postDeployMode,
                    delay_ms: dm.delay_ms,
                }
            }
            doorModes = [...doorModes]
            applied.push(`doorModes×${cfg.doorModes.length}`)
        }

        // Battery
        if (cfg.battery) {
            batteryAutoDeploy = cfg.battery.autoDeploy
            if (batteryChemistries.includes(cfg.battery.chemistry)) {
                batteryChemistry = cfg.battery.chemistry
            }
            batteryCellCount = cfg.battery.cellCount
            applied.push('battery')
        }

        // Fixed gear input (GP0)
        if (cfg.gearInput) {
            gearInputEnabled = cfg.gearInput.enabled
            gearInputThreshold_us = cfg.gearInput.threshold_us
            applied.push('gearInput')
        }

        // Diagnostic — visible in Studio console alongside the config-loader trace
        console.log('[GearControl] applyState →', applied.length ? applied.join(', ') : '(nothing to apply)')
    }

    const gcDriver: BoardConfigDriver<GearControlFullConfig> = {
        boardType: 'gearcontrol',
        boardLabel,
        buildState: buildGearControlFullConfig,
        generateYaml: (s) => generateGearControlYaml(s, false),
        parseYaml:    (t) => parseGearControlYaml(t, false),
        applyState:   applyGearControlConfig,
        verify:       (s) => gcVerifier.verify(s),
    }

    let liveResult: VerifyResult = EMPTY_RESULT
    $: {
        // Touch every reactive dep so Svelte re-runs verification on any edit.
        // Svelte 4's dep tracking is shallow — variables read only inside
        // buildGearControlConfig() aren't tracked unless we void them here.
        void pinConfigs; void gearConfigs; void gearEnabled
        void calibStates; void isHubFX
        void gearInputEnabled; void gearInputThreshold_us
        liveResult = gcVerifier.verify(buildGearControlConfig())
    }

    let sev: (path: string) => string | null
    $: sev = (() => {
        void liveResult
        return (path: string): string | null => gcVerifier.severityForPath(path)
    })()

    function openSaveDialog() { saveDialogOpen = true }

    // ─── Interactive servo calibration ────────────────────────────────
    // One dialog instance, parameterized by which servo invoked it.
    let calibDialogOpen = false
    let calibServoId = 0
    let calibServoName = ''
    let calibTargetPin = -1            // pin index whose config gets updated on Save; -1 = none
    let calibInit = { min_us: 500, max_us: 2500, speed: 4000, accel: 0, decel: 0, reversed: false }

    function openDoorServoSetLimits(pinIdx: number, doorIdx: number) {
        const p = pinConfigs[pinIdx]
        if (!p || p.role !== 'door') return
        calibServoId = p.channel * 2 + doorIdx
        calibServoName = `${gearNames[p.channel]} Door ${doorIdx === 0 ? 'A' : 'B'}`
        calibInit = { min_us: p.min_us, max_us: p.max_us, speed: p.speed, accel: 0, decel: 0, reversed: p.reversed }
        calibTargetPin = pinIdx
        calibDialogOpen = true
    }

    function openYawServoSetLimits() {
        if (yawPinIdx < 0) return
        const p = pinConfigs[yawPinIdx]
        calibServoId = SERVO_ID_YAW
        calibServoName = 'Yaw'
        calibInit = { min_us: p.min_us, max_us: p.max_us, speed: p.speed, accel: 0, decel: 0, reversed: p.reversed }
        calibTargetPin = yawPinIdx
        calibDialogOpen = true
    }

    function onCalibApply(cfg: { min_us: number; max_us: number; speed: number; reversed: boolean }) {
        if (calibTargetPin < 0 || calibTargetPin >= pinConfigs.length) return
        pinConfigs[calibTargetPin].min_us = cfg.min_us
        pinConfigs[calibTargetPin].max_us = cfg.max_us
        pinConfigs[calibTargetPin].speed  = cfg.speed
        pinConfigs[calibTargetPin].reversed = cfg.reversed
        pinConfigs = pinConfigs
        // Rebaseline the live-push dedup so the tab's row matches what the dialog sent.
        const isYaw = calibTargetPin === yawPinIdx
        if (isYaw) { resetPushBaseline('yaw.cfg'); resetPushBaseline('yaw.servo') }
        else       { resetPushBaseline(`servo:pin${calibTargetPin}`) }
    }

</script>

<div class="tab-root">
    <!-- ═══ Title Bar ═══ -->
    <div class="tab-title-bar">
        <h2>{boardLabel}</h2>
        <div class="title-actions">
            <button class="small" on:click={configReload} disabled={controlsDisabled} title="Reload config from flash">↻ Reload</button>
            <button class="small" on:click={openSaveDialog} disabled={controlsDisabled} title="Verify and save config to flash">💾 Save…</button>
            <button class="small" on:click={() => boardDialogOpen = true} title="Show annotated board diagram with live port assignments">🗺 View Diagram</button>
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
        <div class="content-wrap">

            <!-- ═══ Gear Input + Aggregate Deploy / Retract (TOP, spans both columns) ═══ -->
            <!-- Gear input is the FIXED GP0 PWM channel (standalone) or the HubFX-relayed -->
            <!-- command (slave). Aggregate Deploy / Retract / Stop drive all gears at once. -->
            <section class="card top-span">
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

                <div class="top-grid">
                    <!-- Left half: Gear Input live readout + threshold -->
                    <div class="gear-input-block"
                         class:verify-error={sev('gearInput') === 'error'}
                         class:verify-warn={sev('gearInput') === 'warning'}>
                        <div class="gear-input-header">
                            <span class="gear-input-label">
                                Gear Input
                                <span class="gear-input-pin dim">{isHubFX ? 'via HubFX' : 'GP0 · RC PWM'}</span>
                            </span>
                            {#if isHubFX}
                                <span class="gear-input-state {liveGearInputCommandDeploy ? 'state-deploy' : 'state-retract'}">
                                    {liveGearInputCommandDeploy ? '▼ DEPLOY' : '▲ RETRACT'}
                                </span>
                            {:else if liveGearInput_us > 0}
                                <span class="gear-input-state {liveGearInput_us > gearInputThreshold_us ? 'state-deploy' : 'state-retract'}">
                                    {liveGearInput_us > gearInputThreshold_us ? '▼ DEPLOY' : '▲ RETRACT'}
                                </span>
                            {:else}
                                <span class="gear-input-state state-idle">— No signal</span>
                            {/if}
                        </div>

                        {#if !isHubFX}
                            <!-- Live PWM bar over [800, 2200] µs window -->
                            <div class="gear-input-bar-track">
                                {#if liveGearInput_us > 0}
                                    <div class="gear-input-bar-fill"
                                         class:bar-deploy={liveGearInput_us > gearInputThreshold_us}
                                         style="width: {Math.max(0, Math.min(100, ((liveGearInput_us - 800) / 1400) * 100))}%"></div>
                                {/if}
                                <div class="gear-input-bar-threshold"
                                     style="left: {Math.max(0, Math.min(100, ((gearInputThreshold_us - 800) / 1400) * 100))}%"
                                     title="Threshold {gearInputThreshold_us}µs"></div>
                            </div>
                            <div class="gear-input-readout">
                                <span class="gear-input-pulse">{liveGearInput_us > 0 ? `${liveGearInput_us} µs` : '— µs'}</span>
                                <span class="gear-input-thr-label">threshold</span>
                                <input type="number" bind:value={gearInputThreshold_us}
                                       class="field-input narrow" min="800" max="2200" step="50"
                                       title="PWM threshold (µs): pulse > threshold → deploy. Persisted via Save."
                                       disabled={controlsDisabled} />
                                <span class="unit">µs</span>
                                <!-- svelte-ignore a11y-label-has-associated-control -->
                                <label class="toggle">
                                    <input type="checkbox" bind:checked={gearInputEnabled}
                                           disabled={controlsDisabled}
                                           title="Arm GP0 PWM reader (standalone mode). Persisted via Save." />
                                    <span class="toggle-text">Enabled</span>
                                </label>
                                <span class="push-hint" title="Gear input has no runtime command — persisted on Save.">⏷ save-only</span>
                            </div>
                        {:else}
                            <div class="gear-input-readout">
                                <span class="dim">Pin GP0 disarmed — board takes commands from HubFX.</span>
                            </div>
                        {/if}
                    </div>

                    <!-- Right half: Deploy / Retract / Stop (whole undercarriage) -->
                    <div class="agg-block">
                        <div class="agg-buttons">
                            <button class="action-btn primary" disabled={controlsDisabled || !allCalibrated}
                                    on:click={gearAllDeploy} title="Deploy all enabled gears">
                                <span class="icon down">▼</span> Deploy All
                            </button>
                            <button class="action-btn" disabled={controlsDisabled || !allCalibrated}
                                    on:click={gearAllRetract} title="Retract all enabled gears">
                                <span class="icon up">▲</span> Retract All
                            </button>
                            <button class="action-btn danger" disabled={controlsDisabled}
                                    on:click={gearAllStop} title="Stop all motors immediately">
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
                    </div>
                </div>
            </section>

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
                            <div class="pin-map-header-right">
                                <button class="pin-map-board-btn"
                                        on:click={() => boardDialogOpen = true}
                                        title="Show board layout with port assignments">
                                    View Board
                                </button>
                                <span class="header-tag {isHubFX ? 'active' : 'ok'}">
                                    {isHubFX ? 'Slave (via HubFX)' : 'Direct'}
                                </span>
                            </div>
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
                                            on:change={() => resetPushBaseline(`servo:pin${idx}`)}
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
                                {#if gearEnabled[id]}
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
                                {/if}
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

                    <!-- ── Yaw Servo ── -->
                    <!-- Only rendered when a yaw_output pin is assigned. Binds -->
                    <!-- directly to the yaw pin's PinConfig (gear_id / neutral_us / -->
                    <!-- min_us / max_us / speed / reversed) — no duplicated state. -->
                    {#if yawEnabled}
                    <section class="card"
                             class:verify-error={sev('yaw') === 'error' || sev(`pins[${yawPinIdx}]`) === 'error'}
                             class:verify-warn={sev('yaw') === 'warning' || sev(`pins[${yawPinIdx}]`) === 'warning'}>
                        <div class="card-header">
                            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><polygon points="16.24 7.76 14.12 14.12 7.76 16.24 9.88 9.88 16.24 7.76"/></svg> Yaw Servo</h3>
                            <span class="header-tag ok">Gear {pinConfigs[yawPinIdx].gear_id} — {gearNames[pinConfigs[yawPinIdx].gear_id]}</span>
                        </div>

                        <div class="form-grid cols-3">
                            <div class="form-field">
                                <span class="field-label">Associated Gear</span>
                                <input class="field-input" type="text" readonly
                                       value={`0 — ${gearNames[0]}`}
                                       title="Yaw steering is hard-wired to the nose gear (gear 0)" />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Neutral µs</span>
                                <input type="number" bind:value={pinConfigs[yawPinIdx].neutral_us}
                                       on:input={scheduleYawPush}
                                       class="field-input" min="500" max="2500" step="10"
                                       title="Yaw position when gear is retracted. Live-pushed (~350ms)."
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Speed µs/s</span>
                                <input type="number" bind:value={pinConfigs[yawPinIdx].speed}
                                       on:input={scheduleYawPush}
                                       class="field-input" min="100" max="10000" step="100"
                                       title="Cruise speed in µs per second. Live-pushed (~350ms)."
                                       disabled={controlsDisabled} />
                            </div>
                        </div>
                        <div class="form-grid cols-3" style="margin-top: 8px;">
                            <div class="form-field">
                                <span class="field-label">Min µs</span>
                                <input type="number" bind:value={pinConfigs[yawPinIdx].min_us}
                                       on:input={scheduleYawPush}
                                       class="field-input" min="500" max="2500" step="10"
                                       title="Lower PWM bound. Live-pushed (~350ms)."
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <span class="field-label">Max µs</span>
                                <input type="number" bind:value={pinConfigs[yawPinIdx].max_us}
                                       on:input={scheduleYawPush}
                                       class="field-input" min="500" max="2500" step="10"
                                       title="Upper PWM bound. Must be > min. Live-pushed (~350ms)."
                                       disabled={controlsDisabled} />
                            </div>
                            <div class="form-field">
                                <!-- svelte-ignore a11y-label-has-associated-control -->
                                <label class="toggle" style="margin-top: 18px">
                                    <input type="checkbox" bind:checked={pinConfigs[yawPinIdx].reversed}
                                           on:change={scheduleYawPush}
                                           disabled={controlsDisabled} />
                                    <span class="toggle-text">Reversed</span>
                                </label>
                            </div>
                        </div>
                        <div class="apply-row">
                            <button class="small primary" disabled={controlsDisabled}
                                    title="Force resend `yaw.config` + `servo.config` now"
                                    on:click={applyYawConfig}>Apply Yaw Config</button>
                            <button class="small" disabled={controlsDisabled}
                                    title="Interactive limit setter — jog the servo and capture min/max"
                                    on:click={openYawServoSetLimits}>🎯 Set Limits…</button>
                            <span class="push-badge push-{$pushStatus['yaw.cfg'] || ''}" title="Live-push status (~350ms)">{pushBadgeText($pushStatus['yaw.cfg'])}</span>
                        </div>

                        <div class="subsection-inline">
                            <h4>Yaw Input</h4>
                            <div class="slider-row">
                                <input type="range" bind:value={yawPosition_us}
                                       min={pinConfigs[yawPinIdx].min_us}
                                       max={pinConfigs[yawPinIdx].max_us}
                                       step="10"
                                       class="slider wide" disabled={controlsDisabled} />
                                <span class="slider-val">{yawPosition_us} µs</span>
                                <button class="small primary" disabled={controlsDisabled}
                                        on:click={setYaw}>Set</button>
                                <button class="small" disabled={controlsDisabled}
                                        on:click={resetYawPosition}>Reset</button>
                            </div>
                            <div class="field-hint">Live: {liveYaw_us || '—'} µs</div>
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
                                       on:change={scheduleBatteryPush}
                                       disabled={controlsDisabled} />
                                <span class="toggle-text">Auto-Deploy on Low Voltage</span>
                            </label>
                            <span class="push-badge push-{$pushStatus['battery'] || ''}" style="margin-left: auto" title="Live-push status (~350ms)">{pushBadgeText($pushStatus['battery'])}</span>
                            <button class="small primary"
                                    disabled={controlsDisabled}
                                    title="Force resend `battery` now"
                                    on:click={applyBattery}>Apply</button>
                        </div>

                        <div class="form-row">
                            <div class="setting">
                                <span class="field-label">Chemistry</span>
                                <select bind:value={batteryChemistry} class="field-input"
                                        on:change={scheduleBatteryPush}
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
                                           title="0 = auto-detect. Live-pushed (~350ms)."
                                           disabled={controlsDisabled} />
                                    <span class="unit">{batteryCellCount === 0 ? 'auto' : 'S'}</span>
                                </div>
                            </div>
                            {#if batteryCellCount === 0}
                                <span class="field-hint" style="margin-left: 16px; align-self: flex-end;">
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
                                               on:input={() => scheduleGearPush(id)}
                                               class="field-input" min="500" max="65000" step="500"
                                               title="Max motor run time in ms. Live-pushed (~350ms)."
                                               disabled={controlsDisabled} />
                                    </div>
                                </div>
                                <div class="apply-row">
                                    <button class="small primary" disabled={controlsDisabled}
                                            title="Force resend `gear.config` now"
                                            on:click={() => applyGearConfig(id)}>Apply Config</button>
                                    <span class="push-badge push-{$pushStatus[`gear:${id}`] || ''}" title="Live-push status (~350ms)">{pushBadgeText($pushStatus[`gear:${id}`])}</span>
                                </div>
                            </div>

                            <!-- Door Servos (one block per assigned door pin) -->
                            {#if gearHasDoors[id]}
                            <div class="subsection-inline">
                                <h4>Door Servos <span class="door-count">{doorPinsPerGear[id]} servo{doorPinsPerGear[id] > 1 ? 's' : ''}</span></h4>

                                {#each doorPinIndicesPerGear[id] as pinIdx, doorIdx}
                                    <div class="door-servo-block"
                                         class:verify-error={sev(`pins[${pinIdx}]`) === 'error'}
                                         class:verify-warn={sev(`pins[${pinIdx}]`) === 'warning'}>
                                        <div class="door-servo-header">
                                            <span class="door-servo-label">Door {doorIdx === 0 ? 'A' : 'B'}</span>
                                            <span class="door-servo-pin dim">{pinSlots[pinIdx]} · {pinGPIOs[pinIdx]}</span>
                                            <span class="door-live-value">{liveDoor_us[id][doorIdx] || '—'} µs</span>
                                        </div>
                                        <!-- Live bar: position within the configured [min,max] range. -->
                                        <div class="door-live-bar-track">
                                            <div class="door-live-bar-fill"
                                                 style="width: {servoPct(liveDoor_us[id][doorIdx], pinConfigs[pinIdx].min_us, pinConfigs[pinIdx].max_us)}%"></div>
                                        </div>
                                        <div class="form-grid cols-3" style="margin-top: 6px">
                                            <div class="form-field">
                                                <span class="field-label">Min µs</span>
                                                <input type="number" bind:value={pinConfigs[pinIdx].min_us}
                                                       on:input={() => scheduleDoorServoPush(pinIdx)}
                                                       class="field-input" min="500" max="2500" step="10"
                                                       title="Pulse width at one end-stop. Live-pushed (~350ms)."
                                                       disabled={controlsDisabled} />
                                            </div>
                                            <div class="form-field">
                                                <span class="field-label">Max µs</span>
                                                <input type="number" bind:value={pinConfigs[pinIdx].max_us}
                                                       on:input={() => scheduleDoorServoPush(pinIdx)}
                                                       class="field-input" min="500" max="2500" step="10"
                                                       title="Pulse width at the other end-stop. Must be > Min. Live-pushed (~350ms)."
                                                       disabled={controlsDisabled} />
                                            </div>
                                            <div class="form-field">
                                                <span class="field-label">Speed µs/s</span>
                                                <input type="number" bind:value={pinConfigs[pinIdx].speed}
                                                       on:input={() => scheduleDoorServoPush(pinIdx)}
                                                       class="field-input" min="100" max="10000" step="100"
                                                       title="Cruise speed in µs per second. Live-pushed (~350ms)."
                                                       disabled={controlsDisabled} />
                                            </div>
                                        </div>
                                        <div class="door-servo-footer">
                                            <!-- svelte-ignore a11y-label-has-associated-control -->
                                            <label class="toggle">
                                                <input type="checkbox" bind:checked={pinConfigs[pinIdx].reversed}
                                                       on:change={() => scheduleDoorServoPush(pinIdx)}
                                                       disabled={controlsDisabled} />
                                                <span class="toggle-text">Reversed (open @ min)</span>
                                            </label>
                                            <span class="push-badge push-{$pushStatus[`servo:pin${pinIdx}`] || ''}" title="Live-push (~350ms)">{pushBadgeText($pushStatus[`servo:pin${pinIdx}`])}</span>
                                            <button class="small" style="margin-left: auto"
                                                    disabled={controlsDisabled}
                                                    title="Interactive limit setter — jog the servo and capture min/max"
                                                    on:click={() => openDoorServoSetLimits(pinIdx, doorIdx)}>🎯 Set Limits…</button>
                                            <button class="small"
                                                    disabled={controlsDisabled}
                                                    title="Force resend `servo.config` now"
                                                    on:click={() => applyDoorServoConfig(pinIdx)}>Apply</button>
                                        </div>
                                    </div>
                                {/each}
                            </div>

                            <!-- Door Sequencing -->
                            <div class="subsection-inline">
                                <h4>Door Sequencing</h4>
                                <div class="form-grid cols-3">
                                    <div class="form-field">
                                        <span class="field-label">Pre-Deploy</span>
                                        <select bind:value={doorModes[id].preDeployMode}
                                                on:change={() => scheduleDoorModePush(id)}
                                                class="field-input" disabled={controlsDisabled}
                                                title={doorModePreDesc(doorModes[id].preDeployMode)}>
                                            {#each doorModeNames as modeName, idx}
                                                <option value={idx} title={doorModePreDesc(idx)}>{modeName}</option>
                                            {/each}
                                        </select>
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Post-Deploy</span>
                                        <select bind:value={doorModes[id].postDeployMode}
                                                on:change={() => scheduleDoorModePush(id)}
                                                class="field-input" disabled={controlsDisabled}
                                                title={doorModePostDesc(doorModes[id].postDeployMode)}>
                                            {#each doorModeNames as modeName, idx}
                                                <option value={idx} title={doorModePostDesc(idx)}>{modeName}</option>
                                            {/each}
                                        </select>
                                    </div>
                                    <div class="form-field">
                                        <span class="field-label">Delay ms</span>
                                        <input type="number" bind:value={doorModes[id].delay_ms}
                                               on:input={() => scheduleDoorModePush(id)}
                                               class="field-input" min="0" max="5000" step="50"
                                               disabled={controlsDisabled || !usesDelay(doorModes[id].preDeployMode, doorModes[id].postDeployMode)}
                                               title="Used by Dual Delay only — gap between Door A and Door B (ms). Live-pushed (~350ms)." />
                                    </div>
                                </div>

                                <!-- Per-gear visible description: explains what the currently-selected -->
                                <!-- pre/post modes do for THIS gear channel. Replaces hover-only tooltips. -->
                                <div class="door-mode-desc">
                                    <div class="door-mode-desc-row">
                                        <span class="door-mode-desc-tag pre">PRE</span>
                                        <span class="door-mode-desc-mode">{doorModeNames[doorModes[id].preDeployMode]}</span>
                                        <span class="door-mode-desc-text">{doorModePreDesc(doorModes[id].preDeployMode)}</span>
                                    </div>
                                    <div class="door-mode-desc-row">
                                        <span class="door-mode-desc-tag post">POST</span>
                                        <span class="door-mode-desc-mode">{doorModeNames[doorModes[id].postDeployMode]}</span>
                                        <span class="door-mode-desc-text">{doorModePostDesc(doorModes[id].postDeployMode)}</span>
                                    </div>
                                    {#if usesDelay(doorModes[id].preDeployMode, doorModes[id].postDeployMode)}
                                        <div class="door-mode-desc-row">
                                            <span class="door-mode-desc-tag delay">DELAY</span>
                                            <span class="door-mode-desc-mode">{doorModes[id].delay_ms} ms</span>
                                            <span class="door-mode-desc-text">Gap between Door A and Door B in Dual Delay phases.</span>
                                        </div>
                                    {/if}
                                </div>

                                <div class="apply-row">
                                    <button class="small" disabled={controlsDisabled}
                                            title="Force resend `door.mode` now"
                                            on:click={() => applyDoorMode(id)}>Apply Mode</button>
                                    <span class="push-badge push-{$pushStatus[`doormode:${id}`] || ''}" title="Live-push status (~350ms)">{pushBadgeText($pushStatus[`doormode:${id}`])}</span>
                                </div>
                            </div>
                            {:else}
                            <div class="no-doors-hint">No door servos assigned to this gear in Pin Mapping</div>
                            {/if}


                        </section>
                        {/if}
                    {/each}

                </div>
            </div>
        </div>
    </div>
</div>

<SaveConfigDialog
    driver={gcDriver}
    bind:open={saveDialogOpen}
    initialResult={liveResult}
    onSave={async (yaml) => {
        await UploadConfig(yaml)
        // Round-trip: re-download + re-apply so the tab reflects on-device state.
        await loadConfigFromDevice(gcDriver)
    }}
    onClose={() => { saveDialogOpen = false }}
/>

<GearControlBoardDialog
    bind:open={boardDialogOpen}
    pinConfigs={pinConfigs}
    {pinSlots}
    gearNames={gearNames}
    gearEnabled={gearEnabled}
    gearInputEnabled={gearInputEnabled}
    batteryChemistry={batteryChemistry}
    batteryCellCount={batteryCellCount}
    batteryVoltage_mV={batteryVoltage_mV}
    isHubFX={isHubFX}
    onClose={() => { boardDialogOpen = false }}
/>

<ServoCalibrationDialog
    bind:open={calibDialogOpen}
    prefix="gear"
    servoId={calibServoId}
    servoName={calibServoName}
    min_us={calibInit.min_us}
    max_us={calibInit.max_us}
    speed={calibInit.speed}
    accel={calibInit.accel}
    decel={calibInit.decel}
    reversed={calibInit.reversed}
    supportsAccelDecel={false}
    onApply={onCalibApply}
    onClose={() => { calibDialogOpen = false }}
/>

<style>
    /* GearControlTab-specific — shared styles in style.css */

    /* ─── Card Variants ─── */
    .card { transition: border-color 0.2s; }
    .card-warn  { border-color: color-mix(in srgb, var(--warning, #d7ba7d) 60%, transparent); }
    .card-error { border-color: color-mix(in srgb, var(--error) 60%, transparent) !important; }
    .card-active { border-color: color-mix(in srgb, var(--ok, #4ec9b0) 50%, transparent); }

    /* ─── Verification highlights (Rule 23) ─── */
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

    /* ─── Door Servo blocks (inside each gear card) ─── */
    .door-servo-block {
        padding: 8px 10px;
        margin-top: 8px;
        background: var(--bg-raised);
        border-radius: 4px;
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
    }
    .door-servo-header {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-bottom: 6px;
    }
    .door-servo-label {
        font-size: 11px;
        font-weight: 700;
        color: var(--text);
        font-family: var(--font-mono);
        text-transform: uppercase;
        letter-spacing: 0.5px;
    }
    .door-servo-pin {
        font-family: var(--font-mono);
        font-size: 10px;
    }
    .door-servo-footer {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-top: 6px;
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

    /* ─── Top spanning section (Gear Input + Aggregate) ─── */
    .top-span {
        margin-bottom: 12px;
    }
    .top-grid {
        display: grid;
        grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
        gap: 14px;
        align-items: start;
    }
    @media (max-width: 900px) {
        .top-grid { grid-template-columns: 1fr; }
    }

    .gear-input-block, .agg-block {
        padding: 10px 12px;
        background: var(--bg-raised);
        border: 1px solid color-mix(in srgb, var(--border) 40%, transparent);
        border-radius: 4px;
    }
    .gear-input-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 8px;
    }
    .gear-input-label {
        font-family: var(--font-mono);
        font-size: 12px;
        font-weight: 700;
        color: var(--text);
        display: inline-flex;
        gap: 8px;
        align-items: center;
    }
    .gear-input-pin {
        font-size: 10px;
        font-weight: 500;
    }
    .gear-input-state {
        font-family: var(--font-mono);
        font-size: 11px;
        font-weight: 700;
        padding: 2px 8px;
        border-radius: 3px;
    }
    .gear-input-state.state-deploy   { color: var(--ok, #4ec9b0); background: color-mix(in srgb, var(--ok, #4ec9b0) 12%, transparent); }
    .gear-input-state.state-retract  { color: var(--accent); background: color-mix(in srgb, var(--accent) 12%, transparent); }
    .gear-input-state.state-idle     { color: var(--text-dim); background: color-mix(in srgb, var(--text-dim) 10%, transparent); }

    .gear-input-bar-track {
        position: relative;
        height: 8px;
        background: var(--bg-input);
        border-radius: 4px;
        overflow: hidden;
        margin-bottom: 6px;
    }
    .gear-input-bar-fill {
        position: absolute;
        top: 0; left: 0; bottom: 0;
        background: var(--accent);
        transition: width 0.2s;
    }
    .gear-input-bar-fill.bar-deploy {
        background: var(--ok, #4ec9b0);
    }
    .gear-input-bar-threshold {
        position: absolute;
        top: -2px; bottom: -2px;
        width: 2px;
        background: var(--warning, #d7ba7d);
        z-index: 2;
    }
    .gear-input-readout {
        display: flex;
        align-items: center;
        gap: 8px;
        flex-wrap: wrap;
        font-family: var(--font-mono);
        font-size: 11px;
    }
    .gear-input-pulse {
        font-weight: 700;
        color: var(--text);
        min-width: 70px;
    }
    .gear-input-thr-label {
        color: var(--text-dim);
        font-size: 10px;
        margin-left: auto;
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
    .pin-map-header-right {
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .pin-map-board-btn {
        font-size: 11px;
        font-weight: 500;
        padding: 3px 10px;
        background: var(--bg-raised);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        cursor: pointer;
    }
    .pin-map-board-btn:hover {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
        border-color: var(--accent);
        color: var(--text-bright);
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

    /* ─── Door Mode Description (per gear) ─── */
    .door-mode-desc {
        margin-top: 8px;
        padding: 8px 10px;
        background: color-mix(in srgb, var(--accent) 5%, var(--bg-raised));
        border: 1px solid color-mix(in srgb, var(--accent) 18%, transparent);
        border-radius: 4px;
        display: flex;
        flex-direction: column;
        gap: 4px;
    }
    .door-mode-desc-row {
        display: flex;
        align-items: baseline;
        gap: 8px;
        font-size: 11px;
        line-height: 1.35;
    }
    .door-mode-desc-tag {
        font-family: var(--font-mono);
        font-size: 9px;
        font-weight: 700;
        padding: 1px 6px;
        border-radius: 2px;
        letter-spacing: 0.5px;
        flex-shrink: 0;
        min-width: 42px;
        text-align: center;
    }
    .door-mode-desc-tag.pre   { color: var(--ok, #4ec9b0); background: color-mix(in srgb, var(--ok, #4ec9b0) 14%, transparent); }
    .door-mode-desc-tag.post  { color: var(--accent); background: color-mix(in srgb, var(--accent) 14%, transparent); }
    .door-mode-desc-tag.delay { color: var(--warning, #d7ba7d); background: color-mix(in srgb, var(--warning, #d7ba7d) 14%, transparent); }
    .door-mode-desc-mode {
        font-family: var(--font-mono);
        font-weight: 700;
        color: var(--text);
        flex-shrink: 0;
        min-width: 80px;
    }
    .door-mode-desc-text {
        color: var(--text-dim);
        flex: 1;
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
    .push-hint {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        font-style: italic;
        margin-left: auto;
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
