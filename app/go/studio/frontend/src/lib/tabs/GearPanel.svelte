<!-- GearPanel — Gear / Undercarriage editor (Gear tab), regenerated from
     scratch 2026-06-11.

     STRUCTURE (always fully visible — the enable flag gates OPERATION, not
     configuration: an operator authoring a model on the bench sees every
     section from the first open, instead of a bare header):

       Status row     enable toggle · live summary pills · fleet Gear Up/Down
       1 · Input      the one RC up/down channel + signal-loss failsafe (Rule 36)
       2 · Sounds     coordination mode + deploy / retract transit loops (Rule 47)
       3 · Struts     one card per undercarriage leg: motor (port + drive
                      params inline), door servos (ports, positions,
                      calibration + live mirror), door sequencing + close
                      policy, per-strut bench ops, sequenced ordering

     Cross-references:
       - Card / form-row / button-cluster      → Rule 34
       - Typography (.hint/.unit/.help-text)   → Rule 50
       - Validation surfacing                  → Rule 35 (red) / Rule 39 (yellow)
       - Output-port pickers (role-filtered)   → Rule 49 (freePortPool)
       - Enable button                         → Rule 45
       - Action toggles / op-cluster           → Rule 48
       - RC channel cluster                    → Rule 36 (ChannelToggleCluster)
       - Sound rows                            → Rule 47 (SoundRow)
       - Servo calibration + live mirror       → Rule 42/44 (ServoWidget/IoWidget)
       - Motor calibration + live mA readout   → Rule 42/44 (MotorWidget + MotorCalibrationDialog)
       - Modular dirty source                  → Rule 46 (gearConfigSource in gear.ts)
-->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import {
        gearDraft, gearDirty, gearHasErrors, gearPhases,
        loadGearConfig, refreshGearStatus,
        addGearChannel, seedDefaultGearStruts, removeGearChannel, setGearEnabled, setGearCoord, setGearDeployOnConnLoss,
        updateGearChannel, removeGearDoor,
        updateGearInput, updateGearSounds,
        gearItemErrors, installGearPhaseListener,
        gearDeploy, gearRetract, gearStop, gearReset, gearAll, gearEStopAll, gearStep,
        GearAllDeploy, GearAllRetract, GearStepUp, GearStepDown,
        type GearConfigT, type GearChannelT, type CoordMode,
        type PortRefT, type GearPhaseT,
    } from '../gear'
    import {
        deviceModel, liveChannels, liveChannelKey,
        type Port, RoleKind,
    } from '../devicemodel'
    import { effectClaims } from '../effect-claims'
    import { connectionInfo } from '../stores'
    import { pickFile } from '../filepicker'
    import { freePortPool } from '../components/port_pool'
    import { portRefToKey, modelPortKey, parsePortKey as parsePortKeyRaw, refOptLabel } from '../components/port_keys'
    import { collectChannelOptions } from '../channels'
    import { validateSoundFiles } from '../sound_validation'
    import ServoWidget from '../components/ServoWidget.svelte'
    import ServoIoWidget from '../components/ServoIoWidget.svelte'
    import MotorWidget from '../components/MotorWidget.svelte'
    import SoundRow from '../components/SoundRow.svelte'
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'
    import { servoStatus, servoStatusKey, installServoStatusListener, setServoLiveView } from '../servo_status'
    import { makeProfileForPort } from '../servo_calibration'
    import { motorStatus, motorStatusKey, pollMotors } from '../motor_status'
    import { openMotorCalibrationFor } from '../motor_calibration'

    let busy = false
    let error = ''

    let cfg: GearConfigT
    const unsub = gearDraft.subscribe(c => { cfg = c })

    // GEAR_PHASE_EVENT rides the LOSSY async queue (Rule 53) — a dropped
    // event would freeze a pill, so a slow GEAR_STATUS poll self-heals it.
    let statusTimer: ReturnType<typeof setInterval> | undefined
    onMount(() => {
        installGearPhaseListener()
        // Live door-servo positions (Rule 42/53): the generic 20 Hz servo-
        // status stream feeds each door's ServoIoWidget so the operator SEES
        // the door travel during a deploy test (GunFx turret pattern).
        installServoStatusListener()
        setServoLiveView(true).catch(() => {})
        loadGearConfig().catch(e => { error = String(e) })
        refreshGearStatus().catch(() => {})
        pollConfiguredMotors()
        // One timer drives BOTH the phase-status self-heal (Rule 53) AND the
        // gear-motor live readout (current mA / duty / stall) — there's no
        // motor broadcast, so the panel polls each configured motor here.
        statusTimer = setInterval(() => {
            refreshGearStatus().catch(() => {})
            pollConfiguredMotors()
        }, 1500)
    })
    onDestroy(() => {
        unsub()
        if (statusTimer) clearInterval(statusTimer)
        setServoLiveView(false).catch(() => {})
    })

    $: dirty     = $gearDirty
    $: hasErrors = $gearHasErrors
    $: enabled   = !!cfg?.enabled

    // Re-poll config + live phase the instant the device (re)connects, so the
    // strut pills don't sit on a stale "unknown" from a previous session until
    // the 1.5 s timer next fires (the "UI shows Unknown while the board is gear
    // up" report — the panel had cached the pre-connect state).
    let wasConnected = false
    $: if ($connectionInfo.connected && !wasConnected) {
        wasConnected = true
        loadGearConfig().catch(() => {})
        refreshGearStatus().catch(() => {})
    } else if (!$connectionInfo.connected) {
        wasConnected = false
    }

    // ─── Live fleet summary (status row) ─────────────────────────────
    $: strutCount = cfg?.gears.length ?? 0
    $: anyMoving  = (cfg?.gears ?? []).some(g => {
        const p = $gearPhases[g.id]?.phase
        return p === 2 || p === 4          // deploying / retracting
    })
    $: anyErrored = (cfg?.gears ?? []).some(g => $gearPhases[g.id]?.phase === 5)
    $: allDeployed  = strutCount > 0 && (cfg?.gears ?? []).every(g => ($gearPhases[g.id]?.phase ?? 6) === 3)
    $: allRetracted = strutCount > 0 && (cfg?.gears ?? []).every(g => ($gearPhases[g.id]?.phase ?? 6) === 1)
    $: anyHeld    = (cfg?.gears ?? []).some(g => $gearPhases[g.id]?.phase === 7)
    $: fleetMovingDown = (cfg?.gears ?? []).some(g => $gearPhases[g.id]?.phase === 2)
    $: fleetMovingUp   = (cfg?.gears ?? []).some(g => $gearPhases[g.id]?.phase === 4)
    $: allUnknown   = strutCount > 0 && (cfg?.gears ?? []).every(g => {
        const p = $gearPhases[g.id]?.phase ?? 6; return p === 6 || p === 0
    })
    // Master status vocabulary is the gear-up/gear-down language (directional
    // while in transit): error · held · lowering/raising · gear down · gear up
    // · unknown (boot, position unestablished) · mixed (genuinely split).
    $: fleetText = anyErrored ? 'error' : anyHeld ? 'held'
                 : anyMoving ? (fleetMovingDown && !fleetMovingUp ? 'lowering'
                              : fleetMovingUp && !fleetMovingDown ? 'raising' : 'moving')
                 : allDeployed ? 'gear down' : allRetracted ? 'gear up'
                 : allUnknown ? 'unknown' : 'mixed'

    // Port-picker helpers (Rule 34/49) — portRefToKey/modelPortKey/refOptLabel
    // come straight from port_keys.  parsePortKey there returns PortRefLike
    // (board optional); gear's PortRefT requires board, so adapt at this one
    // boundary with a cast (the GunFx pattern).
    const parsePortKey = (key: string, kind: string): PortRefT => parsePortKeyRaw(key, kind) as PortRefT

    // Cross-effect exclusion (Rule 60.4): the MERGED claim list — hard claims
    // + soft claims synthesized from every effect's draft (GunFx turret/smoke,
    // Landing servos/LEDs, LightFx channels, gear itself).  Own-domain claims
    // are dropped here (sibling-strut exclusion is the used-set below; the
    // row's own pick survives via `exempt`).  Passing $deviceModel.claims
    // instead of this was the "every servo shows up" bug (2026-06-13).
    $: xClaims = $effectClaims.filter(c => c.domain !== 'gear')

    /** Refs used across ALL struts so a port picked by a sibling doesn't
     *  linger as "free"; `keep` (the row's own pick) stays in its pool. */
    function usedMotorRefs(gears: GearChannelT[]): Set<string> {
        const out = new Set<string>()
        for (const g of gears) if (g.motor && g.motor.kind) out.add(`${g.motor.guid}#${g.motor.idx}`)
        return out
    }
    function usedDoorRefs(gears: GearChannelT[]): Set<string> {
        const out = new Set<string>()
        for (const g of gears) for (const d of g.doors) if (d.port && d.port.kind)
            out.add(`${d.port.guid}#${d.port.idx}`)
        return out
    }
    function motorPool(ports: Port[], claims: any[], used: Set<string>, keep: PortRefT | null): Port[] {
        const exempt = keep && keep.kind ? [{ guid: keep.guid, kind: keep.kind, idx: keep.idx }] : []
        return freePortPool(ports, claims, 'hbridge', RoleKind.BiDcMotor, exempt)
            .filter(p => {
                if (keep && keep.kind && keep.guid === p.ref.guid && keep.idx === p.ref.index) return true
                return !used.has(`${p.ref.guid}#${p.ref.index}`)
            })
    }
    function doorPool(ports: Port[], claims: any[], used: Set<string>, keep: PortRefT | null): Port[] {
        const exempt = keep && keep.kind ? [{ guid: keep.guid, kind: keep.kind, idx: keep.idx }] : []
        return freePortPool(ports, claims, 'servo', RoleKind.ServoActuator, exempt)
            .filter(p => {
                if (keep && keep.kind && keep.guid === p.ref.guid && keep.idx === p.ref.index) return true
                return !used.has(`${p.ref.guid}#${p.ref.index}`)
            })
    }

    // Servo profile lookup (Rule 44) — shared reactive factory (re-runs on
    // every $deviceModel change; the landing-panel "froze on first render" trap).
    $: profileForPort = makeProfileForPort($deviceModel)
    function labelForPort(port: PortRefT | null | undefined): string {
        if (!port || !port.kind) return ''
        for (const p of $deviceModel.ports) {
            if (p.ref.guid === port.guid && p.kindName === port.kind && p.ref.index === port.idx) {
                return refOptLabel(p)
            }
        }
        return `Servo ${port.idx}`
    }

    // ─── RC up/down channel (Rule 36 + Rule 43 named inputs) ─────────
    $: chanOpts = collectChannelOptions($deviceModel)
    $: chosenChan = chanOpts.find(o => o.fnId === cfg?.input?.name)
    $: liveUs = chosenChan ? $liveChannels[liveChannelKey({ guid: chosenChan.portGuid, kind: 4, index: chosenChan.portIdx }, chosenChan.channel)] : null

    // ─── Transit-sound validation (Rule 47, both OPTIONAL) ───────────
    let soundErrors: { deploy: string; retract: string } = { deploy: '', retract: '' }
    let validateTimer: ReturnType<typeof setTimeout> | null = null
    function scheduleValidate() {
        if (validateTimer) clearTimeout(validateTimer)
        validateTimer = setTimeout(() => { validateTimer = null; void validateSounds() }, 350)
    }
    async function validateSounds() {
        const s = cfg?.sounds
        const errs = await validateSoundFiles([
            { key: 'deploy',  path: s?.deploy  ?? '' },   // both optional
            { key: 'retract', path: s?.retract ?? '' },
        ])
        soundErrors = { deploy: errs.deploy ?? '', retract: errs.retract ?? '' }
    }
    $: void scheduleValidateOn(cfg?.sounds?.deploy, cfg?.sounds?.retract)
    function scheduleValidateOn(..._: unknown[]) { if (cfg) scheduleValidate() }
    $: soundsHaveErrors = !!(soundErrors.deploy || soundErrors.retract)

    // (typed `string` — Svelte's {#each} alias is un-narrowed; callers
    // only pass 'deploy' | 'retract')
    async function browseSound(field: string) {
        const p = await pickFile({ targets: 'sd' })
        if (p != null) { updateGearSounds(s => ({ ...s, [field]: p })); scheduleValidate() }
    }
    function clearSound(field: string) {
        updateGearSounds(s => ({ ...s, [field]: '' }))
        soundErrors = { ...soundErrors, [field]: '' }
    }

    // ─── Coordination ────────────────────────────────────────────────
    // Two modes only (kept deliberately simple): Independent or Full-sync.
    const coordOptions: { id: CoordMode; label: string; hint: string }[] = [
        { id: 'independent', label: 'Independent', hint: 'each strut deploys/retracts on its own — no cross-strut sync.' },
        { id: 'full_sync',   label: 'Full-sync',   hint: 'all struts move in lockstep: doors open together, then struts run together, then doors close together.' },
    ]
    // Legacy configs may still carry door_sync/sequenced — fold them onto the
    // nearest surviving mode so the segmented toggle always has a selection.
    $: if (cfg && (cfg.coord === 'door_sync' || cfg.coord === 'sequenced')) {
        setGearCoord(cfg.coord === 'sequenced' ? 'independent' : 'full_sync')
    }
    $: coordHint = coordOptions.find(o => o.id === cfg?.coord)?.hint ?? ''

    // ─── Live phase pill ─────────────────────────────────────────────
    function phaseClass(p: number | undefined): string {
        switch (p) {
            case 1: return 'phase-retracted'   // gear up
            case 2: return 'phase-deploying'   // lowering
            case 3: return 'phase-deployed'    // gear down
            case 4: return 'phase-retracting'  // raising
            case 5: return 'phase-error'
            case 7: return 'phase-held'        // emergency-held
            default: return 'phase-unknown'    // 0 unconfigured / 6 unknown
        }
    }
    // pillText / lifecycle / doorStatus / strutActions are PURE — they take the
    // phase OBJECT (or its numeric phase), never read $gearPhases inside.  This
    // is load-bearing for reactivity: Svelte tracks template deps syntactically,
    // so a helper that read the store internally (the old pillText(id)/phaseOf)
    // would NOT re-run when $gearPhases changed (the "pill + buttons stale while
    // the lifecycle caption updates" bug).  Always pass $gearPhases[id] in.
    function pillText(phT: GearPhaseT | undefined): string {
        if (!phT) return 'unknown'
        if (phT.phase === 5) {                       // ERROR — surface the reason
            return phT.errReasonTag ? `error · ${phT.errReasonTag}` : 'error'
        }
        const sub = (phT.subPhase && phT.subPhase !== 0) ? ` · ${phT.subPhaseName}` : ''
        return `${phT.phaseName}${sub}`
    }

    // ─── Per-strut phase introspection (items 2/3/5) ─────────────────
    // Phase bytes: 0 unconfig · 1 up · 2 movingDown · 3 down · 4 movingUp
    //              5 error · 6 unknown · 7 held.
    const isMoving  = (p: number) => p === 2 || p === 4
    const isHeld    = (p: number) => p === 7
    const isUnknown = (p: number) => p === 6 || p === 0
    const isDownP   = (p: number) => p === 3

    // ─── Lifecycle strip (item 2): the symmetric transit as 3 macro stages
    //     (Doors → Strut → Doors).  Each segment is done / active / pending
    //     from the live sub-phase, so the operator SEES whether the strut is
    //     in a door action or the strut action.  Sub-phase bytes:
    //       1 doors-opening · 2 doors-open · 3 strut-moving
    //       4 strut-done · 5 doors-closing · 6 doors-closed.
    type StageState = 'done' | 'active' | 'pending' | 'idle'
    interface LifeStage { label: string; state: StageState }
    function lifecycle(ph: GearPhaseT | undefined, hasDoors: boolean): { dir: string; stages: LifeStage[]; caption: string } {
        const sub = ph?.subPhase ?? 0
        const p   = ph?.phase ?? 6
        const dir = p === 2 ? 'lowering' : p === 4 ? 'raising'
                  : p === 3 ? 'gear down' : p === 1 ? 'gear up'
                  : p === 7 ? 'held' : p === 5 ? 'error' : 'unknown'
        // Map the sub-phase position onto [open, strut, close] progress.
        const rank: Record<number, number> = { 0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6 }
        const r = rank[sub] ?? 0
        const seg = (afterDone: number, activeAt: number[]): StageState => {
            if (!isMoving(p)) return 'idle'
            if (activeAt.includes(sub)) return 'active'
            return r > afterDone ? 'done' : 'pending'
        }
        // A doorless strut has no door legs — show just the strut stage so the
        // strip doesn't imply doors that aren't configured.
        const stages: LifeStage[] = hasDoors
            ? [
                { label: 'Doors',  state: seg(2, [1]) },    // doors-opening active; done once open
                { label: 'Strut',  state: seg(4, [3]) },    // strut-moving active; done once strut-done
                { label: 'Doors',  state: seg(6, [5]) },    // doors-closing active; done once closed
              ]
            : [
                { label: 'Strut',  state: seg(4, [3]) },
              ]
        const caption = isMoving(p)
            ? (ph?.subPhaseName ?? '')
            : (p === 5 ? (ph?.errReasonTag ? `error — ${ph.errReasonTag}` : 'error')
              : p === 7 ? 'held — resume up or down'
              : p === 6 || p === 0 ? 'position unknown — command to home'
              : dir)
        return { dir, stages, caption }
    }

    // ─── Door status (request) ───────────────────────────────────────
    // Inferred from the live sub-phase (which directly reflects door state in
    // transit) + the settled phase + the close policy at rest.  Returns null
    // for a strut with no doors configured.
    interface DoorStat { label: string; state: 'open' | 'closed' | 'moving' | 'unknown' }
    function doorStatus(gch: GearChannelT, phT: GearPhaseT | undefined): DoorStat | null {
        if (!gch.doors || gch.doors.length === 0) return null
        const sub = phT?.subPhase ?? 0
        const p   = phT?.phase ?? 6
        switch (sub) {
            case 1: return { label: 'opening', state: 'moving' }
            case 5: return { label: 'closing', state: 'moving' }
            case 2: case 3: case 4: return { label: 'open', state: 'open' }
            case 6: return { label: 'closed', state: 'closed' }
        }
        // Settled (sub === idle).
        if (p === 1) return { label: 'closed', state: 'closed' }          // gear up → stowed
        if (p === 3) {                                                     // gear down → close policy
            if (gch.closePolicy === 'none')  return { label: 'open', state: 'open' }
            if (gch.closePolicy === 'first') return { label: 'part-open', state: 'open' }
            return { label: 'closed', state: 'closed' }
        }
        return { label: '—', state: 'unknown' }                           // unknown / held / error
    }

    // ─── Context-aware strut actions (item 5) ────────────────────────
    // The buttons + labels follow where the strut IS in its cycle:
    //   moving  → Hold + Reverse (the other direction)
    //   held    → Resume Down / Resume Up (both offered — position uncertain)
    //   error   → Retry Down / Retry Up (deploy/retract auto-clear the fault)
    //   settled → the single opposite-direction toggle (Gear Up / Gear Down)
    // `gate` = this action runs a full cycle from the LOADED config, so it's
    // blocked while the draft is dirty / invalid (Rule 35/48).  Emergency-safe
    // actions (hold, reverse, raise, resume/retry) are never gated.
    interface ActBtn { label: string; title: string; danger?: boolean; gate?: boolean; fn: () => void }
    function strutActions(id: number, p: number): ActBtn[] {
        const deploy  = () => safe(() => gearDeploy(id))
        const retract = () => safe(() => gearRetract(id))
        const hold    = () => safe(() => gearStop(id))
        if (isMoving(p)) {
            const reversingDown = p === 4   // currently raising → reverse lowers
            return [
                { label: reversingDown ? '↺ Reverse ▼' : '↺ Reverse ▲',
                  title: 'Reverse mid-cycle — the strut pre-empts and heads the other way (doors/strut reverse cleanly).',
                  fn: reversingDown ? deploy : retract },
                { label: '⏸ Hold', danger: true,
                  title: 'Hold in place — brake the motor + freeze the doors (position uncertain). Resume with Up/Down.',
                  fn: hold },
            ]
        }
        if (isHeld(p)) {
            return [
                { label: '▼ Resume Down', title: 'Resume from the held position toward gear down.', fn: deploy },
                { label: '▲ Resume Up',   title: 'Resume from the held position toward gear up.',   fn: retract },
            ]
        }
        if (p === 5) {   // error — deploy/retract auto-clear the fault + retry
            return [
                { label: '↻ Retry Down', title: 'Clear the fault and drive toward gear down.', fn: deploy },
                { label: '↻ Retry Up',   title: 'Clear the fault and drive toward gear up.',   fn: retract },
            ]
        }
        // Settled (up / down / unknown): one opposite-direction toggle.
        if (isDownP(p)) {
            return [{ label: '▲ Gear Up', danger: true, title: 'Raise this strut (always available).', fn: retract }]
        }
        return [{ label: '▼ Gear Down', gate: true,
                 title: 'Lower this strut: open doors → run motor down → close doors. Apply edits first (runs the loaded config).',
                 fn: deploy }]
    }
    // Step target: advance toward the OPPOSITE of the settled state (or
    // continue toward the in-flight direction).
    function stepTarget(p: number): number {
        if (p === 2) return GearStepDown   // mid-lowering → keep stepping down
        if (p === 4) return GearStepUp     // mid-raising → keep stepping up
        return isDownP(p) ? GearStepUp : GearStepDown
    }
    function stepLabel(p: number): string {
        return stepTarget(p) === GearStepDown ? 'Step ▾' : 'Step ▴'
    }

    // ─── Field setters ───────────────────────────────────────────────
    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    function resetSelect(e: Event) { (e.target as HTMLSelectElement).value = '' }

    function setField<K extends keyof GearChannelT>(id: number, key: K, val: GearChannelT[K]) {
        updateGearChannel(id, g => ({ ...g, [key]: val }))
    }
    function setMotorPort(id: number, port: PortRefT) {
        updateGearChannel(id, g => ({ ...g, motor: port }))
    }
    // Deploy DIRECTION — which way the H-bridge drives to LOWER the gear.
    // The firmware contract stays signed deploy/retract duties; the toggle
    // only flips their SIGN, preserving each magnitude (the working duty is
    // tuned in Calibrate).  forward = deploy positive / retract negative.
    function motorForward(g: GearChannelT): boolean { return (g.deployDuty ?? 0) >= 0 }
    function setMotorDirection(id: number, forward: boolean) {
        updateGearChannel(id, g => {
            const dMag = Math.abs(g.deployDuty) || 20000
            const rMag = Math.abs(g.retractDuty) || dMag
            return { ...g, deployDuty: forward ? dMag : -dMag, retractDuty: forward ? -rMag : rMag }
        })
    }
    function setDoorPort(id: number, idx: number, port: PortRefT) {
        updateGearChannel(id, g => ({ ...g, doors: g.doors.map((d, i) => i === idx ? { ...d, port } : d) }))
    }
    function onPickMotor(id: number, e: Event) {
        // No `if (k)` guard: picking the blank "— pick a gear motor —" option
        // (value="") MUST clear the motor.  parsePortKey('', 'hbridge') returns
        // the same unselected sentinel a fresh strut uses (emptyPort('hbridge')
        // = {guid:'', kind:'hbridge', idx:0}), so the previously-claimed port is
        // released and reappears in every sibling picker (Rule 49).  Matches the
        // door picker, which already clears this way.
        setMotorPort(id, parsePortKey(selValue(e), 'hbridge'))
    }
    function onPickDoor(id: number, e: Event) {
        const k = selValue(e); if (k) { addGearDoorWithPort(id, parsePortKey(k, 'servo')); resetSelect(e) }
    }
    function addGearDoorWithPort(id: number, port: PortRefT) {
        updateGearChannel(id, g => g.doors.length >= 2 ? g
            : ({ ...g, doors: [...g.doors, { port }] }))
    }

    async function safe(fn: () => Promise<void>) {
        busy = true; error = ''
        try { await fn() } catch (e) { error = String(e) } finally { busy = false }
        refreshGearStatus().catch(() => {})
    }

    // ─── Gear-motor live status + calibration (Rule 42/44) ───────────
    // Poll every configured motor's BiDcMotor status (current mA / duty /
    // position / stall) into the shared motorStatus store; the per-strut
    // MotorWidget reads its sample by guid|idx.
    function pollConfiguredMotors() {
        const refs = (cfg?.gears ?? [])
            .map(g => g.motor)
            .filter(m => m && m.kind === 'hbridge')
            .map(m => ({ guid: m.guid, idx: m.idx, kind: m.kind }))
        if (refs.length) pollMotors(refs).catch(() => {})
    }

    /** Open the motor calibration popup for a strut; Save there commits the
     *  working duty + suggested travel timeout back into the channel draft. */
    function openMotorCal(gch: GearChannelT) {
        if (!gch.motor || !gch.motor.kind) return
        openMotorCalibrationFor({
            guid: gch.motor.guid,
            portIdx: gch.motor.idx,
            label: labelForPort(gch.motor) || `Motor idx ${gch.motor.idx}`,
            deployDuty: gch.deployDuty,
            retractDuty: gch.retractDuty,
            timeoutMs: gch.timeoutMs,
            guard: gch.guard,    // seed the dialog from the strut's saved guard
            onCommit: ({ deployDuty, retractDuty, timeoutMs, guard }) => {
                updateGearChannel(gch.id, g => ({ ...g, deployDuty, retractDuty, timeoutMs, guard }))
            },
        }).catch(e => { error = String(e) })
    }
</script>

<div class="card gear-card">
    <div class="card-header">
        <h3>Gear / Undercarriage</h3>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}

    <!-- Master enable (GunFx/Engine pattern): the toggle gates the WHOLE
         subsystem — when off the tab body is hidden and nothing deploys (the
         radio is ignored too); when on, control + the RC channel are live. -->
    <div class="status-row">
        <div class="status">
            {#if enabled}
                <span class="status-label">Undercarriage Set</span>
                <span class="state-pill phase {anyErrored ? 'phase-error' : anyMoving ? 'phase-deploying' : allDeployed ? 'phase-deployed' : 'phase-retracted'}">
                    {strutCount === 0 ? 'no struts' : fleetText}
                </span>
                {#if strutCount > 0}<span class="hint compact">{strutCount} strut{strutCount === 1 ? '' : 's'} · {cfg?.coord?.replace('_', '-')}</span>{/if}
            {/if}
        </div>
        <div class="controls">
            <button class="small state-toggle" class:state-on={enabled}
                    on:click={() => setGearEnabled(!enabled)} disabled={busy}
                    title={enabled ? 'Gear subsystem ENABLED — Apply to push. Disable to turn the whole undercarriage off (radio + control).'
                                   : 'Gear subsystem DISABLED — nothing deploys and the radio is ignored. Click to enable, then Apply.'}>
                {enabled ? '✓ Enabled' : '▶ Disabled'}
            </button>
            {#if enabled}
                <span class="ctrl-sep" aria-hidden="true"></span>
                <button class="small" on:click={() => addGearChannel()} disabled={busy}
                        title="Add a landing strut (nose, main left, main right, …) — each binds one gear motor + up to two door servos.">
                    + Add landing strut
                </button>
            {/if}
        </div>
    </div>

    {#if !enabled}
        <div class="empty-state">
            <p><strong>Gear / Undercarriage is disabled.</strong></p>
            <p>Toggle <b>▶ Disabled → ✓ Enabled</b> above (then Apply) to configure and control the
               undercarriage — deploy/retract, doors, and the RC up/down channel. While disabled the
               whole subsystem is off and the radio is ignored.</p>
        </div>
    {/if}

  {#if enabled}
    <!-- ═══ CONTROL — the command surface: a fleet row + ONE row per strut,
         each showing live state (lifecycle strip) and that strut's controls.
         Runs the LOADED config (Apply edits first); the config cards below are
         editing-only. ═══ -->
    {#if strutCount > 0}
        <div class="section-head">
            Undercarriage Control
            <span class="hint">live state + manual control — fleet and per-strut. Runs the LOADED config, so Apply edits first. The RC channel below drives the same up/down.</span>
        </div>
        <div class="ctl-frame">
            <!-- Fleet row -->
            <div class="sctl-row fleet">
                <div class="sctl-id">
                    <span class="sctl-name">All struts</span>
                    <span class="state-pill phase {anyErrored ? 'phase-error' : anyMoving ? 'phase-deploying' : allDeployed ? 'phase-deployed' : allRetracted ? 'phase-retracted' : 'phase-unknown'}">{fleetText}</span>
                </div>
                <div class="sctl-acts">
                    <button class="small state-toggle" on:click={() => safe(() => gearAll(GearAllDeploy))}
                            disabled={busy || dirty || hasErrors}
                            title={dirty ? 'Apply your edits first — fleet deploy runs the LOADED config'
                                 : hasErrors ? 'Resolve validation errors first'
                                 : 'Lower every strut (GEAR_ALL deploy).'}>▼ Gear Down</button>
                    <button class="small state-toggle danger" on:click={() => safe(() => gearAll(GearAllRetract))}
                            disabled={busy}
                            title="Raise the whole undercarriage — always available (emergency cutoff).">▲ Gear Up</button>
                    <button class="small danger estop" on:click={() => safe(() => gearEStopAll())}
                            disabled={busy}
                            title="EMERGENCY STOP — brake every motor + freeze the doors. Resume with Gear Up/Down.">⏹ E-Stop</button>
                </div>
            </div>
            <!-- Per-strut control rows -->
            {#each cfg.gears as gch (gch.id)}
                {@const issues     = gearItemErrors(cfg.gears, cfg.gears.findIndex(g => g.id === gch.id), $deviceModel.ports)}
                {@const chanErrors = issues.length > 0}
                {@const phT        = $gearPhases[gch.id]}
                {@const ph         = phT?.phase ?? 6}
                {@const errored    = ph === 5}
                {@const gated      = busy || dirty || chanErrors}
                {@const acts       = strutActions(gch.id, ph)}
                {@const life       = lifecycle(phT, gch.doors.length > 0)}
                {@const order      = cfg.gears.findIndex(g => g.id === gch.id)}
                {@const doors      = doorStatus(gch, phT)}
                <div class="sctl-row" class:invalid={chanErrors}>
                    <div class="sctl-id">
                        <span class="sctl-name">{gch.name || `Strut ${order + 1}`}</span>
                        <span class="state-pill phase {phaseClass(ph)}">{pillText(phT)}</span>
                        {#if doors}
                            <span class="door-chip door-{doors.state}" title="Door state: {doors.label}">⌂ {doors.label}</span>
                        {/if}
                    </div>
                    <div class="sctl-life lifecycle" class:held={isHeld(ph)} class:errored={errored} class:unknown={isUnknown(ph)}>
                        <div class="life-stages">
                            {#each life.stages as st, si}
                                {#if si > 0}<span class="life-arrow">▸</span>{/if}
                                <span class="life-stage {st.state}">{st.label}</span>
                            {/each}
                        </div>
                        {#if life.caption}<span class="life-caption">{life.caption}</span>{/if}
                    </div>
                    <div class="sctl-acts">
                        {#if chanErrors}
                            <span class="hint err compact" title={issues.join(' ')}>⚠ fix config below</span>
                        {:else}
                            {#each acts as a}
                                <button class="small state-toggle" class:danger={a.danger}
                                        on:click={a.fn} disabled={busy || (a.gate && gated)}
                                        title={a.gate && dirty ? 'Apply your edits first — runs the LOADED config' : a.title}>{a.label}</button>
                            {/each}
                            <!-- Single-step: advance ONE leg then park (do one item in the sequence). -->
                            <button class="small btn-step" on:click={() => safe(() => gearStep(gch.id, stepTarget(ph)))}
                                    disabled={busy || isMoving(ph) || (stepTarget(ph) === GearStepDown && gated)}
                                    title="Advance this strut ONE leg toward {stepTarget(ph) === GearStepDown ? 'down' : 'up'} (open doors → run strut → close doors), then park. Manual single-step.">{stepLabel(ph)}</button>
                            {#if errored}
                                <button class="small" on:click={() => safe(() => gearReset(gch.id))} disabled={busy}
                                        title="Clear the error without moving (ERROR → unknown). Deploy/Retract also clear it automatically.">⟳ Reset</button>
                            {/if}
                        {/if}
                    </div>
                </div>
            {/each}
        </div>
    {/if}

    <!-- ═══ Top split: LEFT = Input group (RC up/down channel + signal-loss
         failsafe) · RIGHT = strut sync + transit sounds.  The struts span the
         FULL width below (Rule 60.1 two-column; collapses when narrow). ═══ -->
    <div class="two-col gear-top-cols">
      <div class="col">
        <!-- ═══ Input (LEFT) — framed group: (2) channel on top,
             (3) signal-loss below. ═══ -->
        <div class="section-head">
            Input
            <span class="hint">how the set is commanded from the radio — the RC up/down channel and the signal-loss failsafe. Pick a channel + Apply to drive the gear from the stick.</span>
        </div>
        <div class="sub-frame">
            <!-- (2) Channel selection — TOP.  RC up/down (Rule 36 cluster +
                 Rule 43 named inputs): one switch drives the WHOLE set;
                 firmware failsafe always deploys on RC loss. -->
            <ChannelToggleCluster
                channelLabel="Up/down channel"
                emptyOption="— manual only —"
                options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
                inputId={cfg?.input?.name ?? ''}
                thresholdUs={cfg?.input?.thresholdUs ?? 1500}
                hysteresisUs={cfg?.input?.hysteresisUs ?? 50}
                liveUs={liveUs?.us ?? null}
                liveValid={liveUs?.valid ?? false}
                busy={busy}
                actionVerb="Lowers"
                onChange={(n) => updateGearInput(i => ({
                    ...i, name: n.inputId,
                    thresholdUs: n.thresholdUs, hysteresisUs: n.hysteresisUs,
                }))} />
            {#if cfg?.input?.name}
                <div class="form-row">
                    <span class="hint">switch ON (above threshold) lowers the gear, OFF (below) raises it — flip the channel ON THE RADIO if it's reversed. RC-loss always lowers the gear.</span>
                </div>
            {:else if chanOpts.length === 0}
                <div class="form-row"><span class="field-label"></span>
                    <span class="hint warn">No named input channels — name one in /hubfx.yaml's inputs (IO tab) to drive the gear from the radio.</span>
                </div>
            {/if}

            <!-- (3) Signal-loss failsafe — BELOW the channel. -->
            <div class="form-row">
                <span class="field-label">On signal loss</span>
                <button class="small state-toggle" class:state-on={cfg?.deployOnConnectionLoss}
                        on:click={() => setGearDeployOnConnLoss(!cfg?.deployOnConnectionLoss)} disabled={busy}
                        title={cfg?.deployOnConnectionLoss
                            ? 'Enabled: if an input LINK drops (e.g. the Jeti UART dies, not just the gear channel), the gear emergency-deploys. Click to disable.'
                            : 'Disabled: input link loss does not auto-deploy. Click to enable emergency deploy on connection loss.'}>
                    {cfg?.deployOnConnectionLoss ? '✓ Emergency deploy' : '○ Ignore link loss'}
                </button>
                <span class="hint">deploys the whole set if the input link (UART) drops — distinct from the per-channel RC-loss failsafe.</span>
            </div>
        </div>
      </div><!-- /col — Input (left) -->

      <div class="col">
        <!-- ═══ (4) Transition sounds (RIGHT) — framed group ═══ -->
        <!-- Strut Synchronization — top of the right column. -->
        <div class="section-head">Strut Synchronization</div>
        <div class="sub-frame">
            <!-- Mode choice ≤4 options → segmented toggle (Rule 60.3). -->
            <div class="form-row">
                <div class="seg-select">
                    {#each coordOptions as o}
                        <button class="seg" class:on={cfg?.coord === o.id}
                                on:click={() => setGearCoord(o.id)} disabled={busy}
                                title={o.hint}>{o.label}</button>
                    {/each}
                </div>
            </div>
            <div class="form-row"><span class="hint">{coordHint}</span></div>
        </div>

        <!-- (4) Transition sounds — framed group, below Strut Sync. -->
        <div class="section-head" class:section-error={soundsHaveErrors}>
            Transition sounds
            <span class="hint">optional — the matching WAV loops on the dedicated Gear audio channel while any strut is moving, and stops when the set settles.</span>
            {#if soundsHaveErrors}<span class="section-err-tag">missing files</span>{/if}
        </div>
        <div class="sub-frame">
            {#each ['deploy', 'retract'] as f}
                <SoundRow
                    label={f}
                    placeholder={'/sounds/…  (optional)'}
                    value={cfg?.sounds?.[f] ?? ''}
                    outputMask={cfg?.sounds?.outputMask ?? 3}
                    busy={busy}
                    required={false}
                    error={soundErrors[f]}
                    onPathChange={(v) => { updateGearSounds(s => ({ ...s, [f]: v })); scheduleValidate() }}
                    onMaskChange={(m) => updateGearSounds(s => ({ ...s, outputMask: m }))}
                    onBrowse={() => browseSound(f)}
                    onClear={() => clearSound(f)} />
            {/each}
        </div>
      </div><!-- /col — Strut sync + Transition sounds (right) -->
    </div><!-- /two-col gear-top-cols -->

    <!-- ═══ Landing struts — FULL width, spanning both columns above ═══ -->
    <div class="section-head">
        Landing struts
        <span class="hint">one card per undercarriage leg — gear motor, door servos and their sequencing.</span>
    </div>

    {#if cfg && cfg.gears.length === 0}
        <div class="empty-state">
            <p><strong>No struts yet.</strong></p>
            <p>
                Each strut binds a <strong>gear motor</strong> (an H-bridge port with a BiDcMotor role —
                attach one on the IO tab) and up to two <strong>door servos</strong>.  A deploy opens the
                doors, runs the motor to the endstop, then closes the doors per your policy; typical
                models add three: nose, main left, main right.
            </p>
            <div class="empty-actions">
                <button class="small primary" on:click={() => seedDefaultGearStruts()} disabled={busy}
                        title="Seed the canonical undercarriage: Main Left, Main Right, Front/Back.">
                    + Add default struts
                </button>
                <button class="small" on:click={() => addGearChannel()} disabled={busy}
                        title="Add a single blank strut instead.">+ Add one strut</button>
            </div>
        </div>
    {/if}

    {#each (cfg?.gears ?? []) as gch (gch.id)}
        {@const issues       = gearItemErrors(cfg.gears, cfg.gears.findIndex(g => g.id === gch.id), $deviceModel.ports)}
        {@const chanErrors   = issues.length > 0}
        {@const usedMotors   = usedMotorRefs(cfg.gears)}
        {@const usedDoors    = usedDoorRefs(cfg.gears)}
        {@const motorOpts    = motorPool($deviceModel.ports, xClaims, usedMotors, gch.motor)}
        {@const motorPoolEmpty= motorOpts.length === 0 && (!gch.motor || !gch.motor.kind)}
        {@const doorAddPool  = doorPool($deviceModel.ports, xClaims, usedDoors, null)}
        {@const doorPoolEmpty= doorAddPool.length === 0 && gch.doors.length === 0}
        {@const order        = cfg.gears.findIndex(g => g.id === gch.id)}
        <div class="card group-card" class:invalid={chanErrors}>
            <div class="card-header inner">
                <h4>{gch.name || `Landing Strut ${order + 1}`}</h4>
                <div class="header-actions">
                    <!-- Config card = editing only.  Live state + deploy/retract/
                         step/hold/reset live in the Control section at the top. -->
                    <span class="state-pill phase {phaseClass($gearPhases[gch.id]?.phase ?? 6)} pill-compact">{pillText($gearPhases[gch.id])}</span>
                    <button class="small danger" on:click={() => removeGearChannel(gch.id)} disabled={busy}>× Remove</button>
                </div>
            </div>

            <!-- Name -->
            <div class="form-row">
                <span class="field-label">Name</span>
                <input class="field-input wide" type="text" maxlength="15"
                       value={gch.name}
                       on:input={(e) => setField(gch.id, 'name', inputValue(e))}
                       disabled={busy} placeholder="e.g. nose, main-left, main-right" />
            </div>

            <!-- Two-column subsystem split (Rule 60.1, GunFx pattern):
                 LEFT = the strut drive (motor), RIGHT = the doors subsystem
                 (servo selection + sequencing).  Each column is ONE
                 .sub-frame (Rule 60.8 background hierarchy).  Live widgets
                 stay OUT of the columns (Rule 60.2). -->
            <div class="two-col strut-cols">
                <div class="col">
                    <div class="sub-frame" class:frame-warn={motorPoolEmpty}>
                        <div class="frame-head">
                            Gear motor
                            {#if motorPoolEmpty}<span class="section-warn-tag">no BiDcMotor port</span>{/if}
                        </div>
                        <div class="form-row">
                            <span class="field-label">Port</span>
                            <select class="field-input wide" value={portRefToKey(gch.motor)}
                                    on:change={(e) => onPickMotor(gch.id, e)}
                                    disabled={busy || motorPoolEmpty}>
                                <option value="">{motorPoolEmpty ? '— no free BiDcMotor port —' : '— pick a gear motor —'}</option>
                                {#each motorOpts as p}
                                    <option value={modelPortKey(p)}>{refOptLabel(p)}</option>
                                {/each}
                            </select>
                        </div>
                        {#if motorPoolEmpty}
                            <div class="form-row">
                                <span class="hint warn">No free H-bridge with a BiDcMotor role — attach one on the IO tab, then pick it here.</span>
                            </div>
                        {/if}
                        {#if gch.motor.kind}
                            <!-- Deploy DIRECTION (Rule 60.3 segmented toggle):
                                 which way the H-bridge drives to LOWER the gear.
                                 Flip if deploy/retract come out swapped.  The
                                 working-duty MAGNITUDE is tuned in Calibrate. -->
                            <div class="form-row">
                                <span class="field-label">Deploy drives</span>
                                <div class="seg-select">
                                    <button class="seg" class:on={motorForward(gch)}
                                            on:click={() => setMotorDirection(gch.id, true)} disabled={busy}
                                            title="Deploy runs the motor FORWARD (positive duty); retract reverses.">Forward</button>
                                    <button class="seg" class:on={!motorForward(gch)}
                                            on:click={() => setMotorDirection(gch.id, false)} disabled={busy}
                                            title="Deploy runs the motor in REVERSE (negative duty); retract goes forward.">Reverse</button>
                                </div>
                            </div>
                            <div class="form-row">
                                <span class="field-label">Travel timeout</span>
                                <input class="field-input narrow" type="number" min="0" max="60000" step="500"
                                       value={gch.timeoutMs}
                                       on:change={(e) => setField(gch.id, 'timeoutMs', Math.max(0, Math.round(numValue(e))))}
                                       disabled={busy} title="Full-travel watchdog (ms) — the endstop seek aborts to ERROR after this. 0 = seek until stall." />
                                <span class="unit">ms</span>
                                <span class="hint compact">flip direction if deploy/retract are swapped; tune the duty in Calibrate.</span>
                            </div>
                            <!-- Motor summary + Calibrate popup + LIVE readout
                                 (current mA / duty / position / stall) — Rule 44
                                 popup pattern, Rule 60.2 per-element live strip. -->
                            {@const mLive = $motorStatus[motorStatusKey(gch.motor.guid, gch.motor.idx)] ?? null}
                            <div class="form-row motor-widget-row">
                                <MotorWidget
                                    hasMotor={true}
                                    deployDuty={gch.deployDuty}
                                    retractDuty={gch.retractDuty}
                                    timeoutMs={gch.timeoutMs}
                                    live={mLive}
                                    busy={busy}
                                    onCalibrate={() => openMotorCal(gch)} />
                            </div>
                        {/if}
                    </div>
                </div>

                <div class="col">
                    <div class="sub-frame" class:frame-warn={doorPoolEmpty}>
                        <div class="frame-head">
                            Doors (≤2)
                            {#if doorPoolEmpty}<span class="section-warn-tag">no ServoActuator port</span>{/if}
                        </div>
                        {#each gch.doors as d, i (i)}
                            {@const dPool = doorPool($deviceModel.ports, xClaims, usedDoors, d.port)}
                            <div class="form-row">
                                <span class="field-label">Door {i + 1}</span>
                                <select class="field-input wide" value={portRefToKey(d.port)}
                                        on:change={(e) => setDoorPort(gch.id, i, parsePortKey(selValue(e), 'servo'))}
                                        disabled={busy}>
                                    <option value="">— pick a free servo port —</option>
                                    {#each dPool as p}
                                        <option value={modelPortKey(p)}>{refOptLabel(p)}</option>
                                    {/each}
                                </select>
                                <button class="small danger btn-slot" on:click={() => removeGearDoor(gch.id, i)} disabled={busy}>× Remove</button>
                            </div>
                            <!-- Open/close ARE the servo's calibrated ends (same
                                 parametrisation as a landing-light servo): open →
                                 MAX-µs end, close → MIN-µs end; the REV flag in
                                 Calibrate flips the physical direction.  No
                                 redundant per-door position inputs. -->
                            <div class="form-row sub">
                                <span class="hint">open = calibrated MAX end · close = MIN end — set the travel (and reverse) in <b>⚙ Calibrate</b> below.</span>
                            </div>
                            <!-- Calibration/setup (Rule 44). -->
                            <div class="form-row servo-widget-row">
                                <ServoWidget
                                    port={d.port}
                                    portLabel={labelForPort(d.port)}
                                    profile={profileForPort(d.port)}
                                    busy={busy} />
                            </div>
                            <!-- Live signal mirror — IN the door column, with the
                                 door it belongs to (Rule 60.2: a PER-ELEMENT live
                                 widget stays with its element; full-width is for
                                 panel/unit-level bars).  Grows to the column. -->
                            {#if d.port && d.port.kind}
                                {@const dProf = profileForPort(d.port) ?? ({ minUs: 1000, maxUs: 2000, centerUs: 1500, reversed: false, maxSpeedUsPerSec: 0, maxAccelUsPerSec2: 0, maxJerkUsPerSec3: 0 })}
                                {@const dSv = $servoStatus[servoStatusKey(d.port.guid, d.port.idx)]}
                                <div class="form-row live-row">
                                    <span class="field-label">Live</span>
                                    <div class="live-widget">
                                        <ServoIoWidget
                                            hasInput={false}
                                            inputUs={null}
                                            inputValid={false}
                                            neutralUs={1500}
                                            hasServo={true}
                                            minUs={dProf.minUs} maxUs={dProf.maxUs} centerUs={dProf.centerUs} reversed={dProf.reversed}
                                            servo={dSv ?? null} />
                                    </div>
                                </div>
                            {/if}
                        {/each}
                        {#if gch.doors.length < 2}
                            <div class="form-row">
                                <select class="field-input wide" on:change={(e) => onPickDoor(gch.id, e)}
                                        disabled={busy || doorAddPool.length === 0}>
                                    <option value="">{doorAddPool.length === 0 ? '— no free servo ports —' : '+ Add door…'}</option>
                                    {#each doorAddPool as p}
                                        <option value={modelPortKey(p)}>{refOptLabel(p)}</option>
                                    {/each}
                                </select>
                            </div>
                            {#if doorAddPool.length === 0 && gch.doors.length === 0}
                                <div class="form-row">
                                    <span class="hint">no doors is fine — the leg runs bare; attach ServoActuators on the IO tab to add doors.</span>
                                </div>
                            {/if}
                        {/if}

                        {#if gch.doors.length >= 1}
                            <!-- Sequencing (Rule 60.3 segmented toggles). -->
                            <div class="form-row">
                                <span class="field-label">Opening</span>
                                <div class="seg-select">
                                    <button class="seg" class:on={gch.doorMode === 'sync'}
                                            on:click={() => setField(gch.id, 'doorMode', 'sync')} disabled={busy}
                                            title="Both doors start opening together.">Together</button>
                                    <button class="seg" class:on={gch.doorMode === 'delay'}
                                            on:click={() => setField(gch.id, 'doorMode', 'delay')} disabled={busy}
                                            title="Door 1 opens, door 2 follows after a fixed delay.">Staggered</button>
                                    <button class="seg" class:on={gch.doorMode === 'sequence'}
                                            on:click={() => setField(gch.id, 'doorMode', 'sequence')} disabled={busy}
                                            title="Door 1 opens fully (motion-done monitored), then door 2 starts.">One, then other</button>
                                </div>
                            </div>
                            {#if gch.doorMode === 'delay'}
                                <div class="form-row">
                                    <span class="field-label">Stagger</span>
                                    <input class="field-input narrow" type="number" min="0" max="10000" step="50"
                                           value={gch.doorDelayMs}
                                           on:change={(e) => setField(gch.id, 'doorDelayMs', Math.max(0, Math.round(numValue(e))))}
                                           disabled={busy} title="Delay before door 2 starts opening (ms)." />
                                    <span class="unit">ms</span>
                                </div>
                                {#if !gch.doorDelayMs || gch.doorDelayMs <= 0}
                                    <div class="form-row">
                                        <span class="hint err">Staggered mode needs a positive delay.</span>
                                    </div>
                                {/if}
                            {/if}
                            <div class="form-row">
                                <span class="field-label">After deploy</span>
                                <div class="seg-select">
                                    <button class="seg" class:on={gch.closePolicy === 'both'}
                                            on:click={() => setField(gch.id, 'closePolicy', 'both')} disabled={busy}
                                            title="Both doors close again once the gear is down.">Both close</button>
                                    <button class="seg" class:on={gch.closePolicy === 'first'}
                                            on:click={() => setField(gch.id, 'closePolicy', 'first')} disabled={busy}
                                            title="Door 1 closes; door 2 stays open around the leg.">One closes</button>
                                    <button class="seg" class:on={gch.closePolicy === 'none'}
                                            on:click={() => setField(gch.id, 'closePolicy', 'none')} disabled={busy}
                                            title="Both doors stay open while the gear is down.">None close</button>
                                </div>
                            </div>
                            <div class="form-row">
                                <span class="hint">the motor waits for the doors to finish opening; retract re-stows whatever was opened.</span>
                            </div>
                            {#if gch.closePolicy === 'first' && gch.doors.length < 2}
                                <div class="form-row">
                                    <span class="hint err">"One closes" needs 2 doors — add a second door or pick a different policy.</span>
                                </div>
                            {/if}
                        {/if}
                    </div>
                </div>
            </div>

            <!-- Per-strut issues (Rule 35 red list) -->
            {#if issues.length > 0}
                <ul class="grp-issues">
                    {#each issues as msg}<li class="grp-issue err">⚠ {msg}</li>{/each}
                </ul>
            {/if}
        </div>
    {/each}
  {/if}<!-- /enabled -->
</div>

<style>
    .gear-card { margin-bottom: 14px; --ctl-h: 28px; }
    .header-actions { display: flex; align-items: center; gap: 8px; }

    /* Rule 63 — uniform row height: every control sharing a horizontal row
       (buttons, inputs/selects, pills, chips, segmented toggles) is the SAME
       height (--ctl-h) and vertically centred, so a row never has a tall input
       next to a short button.  Applied to the control rows AND the config
       form-rows below. */
    .header-actions button,
    .header-actions .state-pill,
    .status-row .controls button,
    .sctl-acts button,
    .sctl-id .state-pill,
    .sctl-id .door-chip,
    .form-row .field-input,
    .form-row button,
    .form-row .seg-select .seg {
        height: var(--ctl-h);
        box-sizing: border-box;
    }
    .header-actions .state-pill,
    .sctl-id .state-pill,
    .sctl-id .door-chip {
        display: inline-flex;
        align-items: center;
    }

    /* Status row: base .status-row / .status / .status-label / .state-pill
       styles are SHARED via style.css (Rule 34/50) — same as EnginePanel.
       Keep ONLY the gear-specific bits: button height + the phase-colour
       pill classes below.  (The old local overrides made this row a
       borderless strip instead of the shared raised card — the "formatting
       looks wrong" report.) */
    .status-row .controls button { height: 28px; box-sizing: border-box; }

    /* Rule 60.8 background hierarchy: the strut card keeps the STANDARD
       nested-.card chrome (--bg-surface, like GunFx's gun/smoke cards);
       only the .sub-frame subsystem boxes inside it go --bg-raised. */
    .group-card { margin: 6px 0 12px; }
    .group-card.invalid { border-color: var(--error); background: rgba(255,80,80,0.05); }
    .card-header.inner { padding: 4px 0 8px; border-bottom: 1px dashed var(--border); margin-bottom: 8px; }
    .card-header.inner h4 { font-size: 13px; font-weight: 600; color: var(--text-bright); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; flex-wrap: wrap; }
    .section-head.sub { font-size: 10px; margin-top: 12px; border-bottom-style: dashed; }
    .section-head .hint { font-weight: 400; text-transform: none; letter-spacing: 0; }
    .section-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
    .section-head.section-error { color: var(--error); border-bottom-color: var(--error); }
    .section-warn-tag { font-size: 9px; font-weight: 700; color: var(--warning); padding: 1px 6px; border: 1px solid var(--warning); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }
    .section-err-tag { font-size: 9px; font-weight: 700; color: var(--error); padding: 1px 6px; border: 1px solid var(--error); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }

    .state-pill.phase { height: 28px; display: inline-flex; align-items: center; box-sizing: border-box; font-family: var(--font-mono); font-size: 10px; padding: 0 8px; border-radius: 3px; background: var(--bg-input); border: 1px solid var(--border); text-transform: uppercase; letter-spacing: 0.4px; }
    .state-pill.phase.phase-retracted  { color: var(--text-dim); }
    .state-pill.phase.phase-deploying  { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-deployed   { color: var(--success); border-color: var(--success); background: rgba(100,200,120,0.12); }
    .state-pill.phase.phase-retracting { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-error      { color: var(--error); border-color: var(--error); background: rgba(255,80,80,0.12); }
    .state-pill.phase.phase-held       { color: var(--warning); border-color: var(--warning); background: rgba(255,180,0,0.12); }
    .state-pill.phase.phase-unknown    { color: var(--text-dim); }

    /* Emergency-stop button — strong outline so it stands apart from Gear
       Up/Down in the status row. */
    .controls .estop { border-color: var(--error); color: var(--error); font-weight: 600; }

    /* Single-step button — quiet accent so it reads as a secondary, manual
       control next to the primary direction action. */
    .btn-step { font-family: var(--font-mono); }

    /* ── Lifecycle strip (item 2) — the door→strut→door transit, live ── */
    .lifecycle { display: flex; align-items: center; gap: 10px; flex-wrap: wrap;
                 margin: 0 0 8px; padding: 5px 8px; border: 1px solid var(--border);
                 border-radius: 4px; background: var(--bg-input); }
    .lifecycle.held    { border-color: var(--warning); }
    .lifecycle.errored { border-color: var(--error); }
    .lifecycle.unknown { border-style: dashed; }
    .life-stages { display: flex; align-items: center; gap: 6px; }
    .life-arrow { color: var(--text-dim); font-size: 10px; }
    .life-stage { font-family: var(--font-mono); font-size: 10px; padding: 1px 8px;
                  border-radius: 3px; border: 1px solid var(--border); color: var(--text-dim);
                  background: var(--bg-surface); text-transform: uppercase; letter-spacing: 0.4px; }
    .life-stage.done    { color: var(--success); border-color: var(--success);
                          background: rgba(100,200,120,0.10); }
    .life-stage.active  { color: var(--warning); border-color: var(--warning);
                          background: rgba(255,180,0,0.14); font-weight: 700; }
    .life-stage.pending { color: var(--text-dim); opacity: 0.6; }
    .life-stage.idle    { color: var(--text-dim); opacity: 0.45; }
    .life-caption { font-size: 10px; color: var(--text-dim); font-style: italic; margin-left: auto; }
    .lifecycle.errored .life-caption { color: var(--error); font-style: normal; font-weight: 600; }
    .lifecycle.held .life-caption    { color: var(--warning); font-style: normal; }

    /* ── Control section (top): fleet row + one row per strut ─────────── */
    .ctl-frame { border: 1px solid var(--border); border-radius: 5px;
                 background: var(--bg-raised); padding: 4px; margin: 0 0 8px;
                 display: flex; flex-direction: column; gap: 4px; }
    .sctl-row { display: grid; grid-template-columns: minmax(150px, 1.1fr) minmax(180px, 1.6fr) auto;
                align-items: center; gap: 10px; padding: 5px 8px; border-radius: 4px;
                background: var(--bg-surface); }
    .sctl-row.fleet { background: var(--bg-input); border: 1px solid var(--border); }
    .sctl-row.invalid { border: 1px solid var(--error); background: rgba(255,80,80,0.05); }
    .sctl-id { display: flex; align-items: center; gap: 8px; min-width: 0; }
    .sctl-name { font-size: 12px; font-weight: 600; color: var(--text-bright);
                 white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .sctl-row.fleet .sctl-name { text-transform: uppercase; font-size: 11px; letter-spacing: 0.5px; color: var(--text-dim); }
    .sctl-acts { display: flex; align-items: center; gap: 6px; justify-content: flex-end; flex-wrap: wrap; }
    /* The lifecycle strip embedded in a control row: no outer chrome/margin
       (the row provides the frame). */
    .sctl-life { margin: 0; padding: 0; border: none; background: none; min-width: 0; }
    .sctl-life.unknown { border: none; }
    @media (max-width: 760px) {
        .sctl-row { grid-template-columns: 1fr; gap: 6px; }
        .sctl-acts { justify-content: flex-start; }
    }
    .pill-compact { font-size: 9px; }

    /* Door status chip in the control row (only when doors are configured). */
    .door-chip { font-family: var(--font-mono); font-size: 9px; padding: 1px 6px;
                 border-radius: 3px; border: 1px solid var(--border); color: var(--text-dim);
                 text-transform: uppercase; letter-spacing: 0.4px; white-space: nowrap; }
    .door-chip.door-open   { color: var(--success); border-color: var(--success); background: rgba(100,200,120,0.10); }
    .door-chip.door-closed { color: var(--text-dim); }
    .door-chip.door-moving { color: var(--warning); border-color: var(--warning); background: rgba(255,180,0,0.12); }
    .door-chip.door-unknown{ color: var(--text-dim); border-style: dashed; }

    .form-row.sub { margin-left: 0; }

    .servo-widget-row { margin-top: 2px; }
    /* Motor summary + Calibrate button + live readout — the widget grows to
       the column so the live strip isn't squeezed (Rule 60.2). */
    .motor-widget-row { margin-top: 6px; }
    .motor-widget-row :global(.motor-widget) { flex: 1 1 auto; min-width: 0; }

    /* Rule 60.2 — full-width live-telemetry row: the widget container
       GROWS to the card width (a plain form-row child only takes content
       width, which squeezed the servo bars). */
    .live-row { display: flex; align-items: flex-start; gap: 10px; margin: 6px 0; }
    .live-row .field-label { padding-top: 2px; }
    .live-row .live-widget { flex: 1 1 auto; min-width: 0; }

    /* Rule 60.1 — the strut's port/behaviour split rides the global
       .two-col grid; collapse to one column when the panel gets narrow. */
    .strut-cols { gap: 4px 18px; margin-top: 2px; }
    @media (max-width: 900px) {
        .strut-cols { grid-template-columns: 1fr; }
    }

    /* Top split — Input (left) | Strut sync + Transit sounds (right).  align-items:start
       so the shorter sounds column doesn't stretch to the taller control
       column; collapses to one column when narrow. */
    .gear-top-cols { gap: 6px 24px; margin: 4px 0 8px; align-items: start; }
    @media (max-width: 900px) {
        .gear-top-cols { grid-template-columns: 1fr; }
    }

    .form-field { display: flex; flex-direction: column; gap: 3px; }
    .form-field .field-label { font-size: 11px; color: var(--text-dim); }

    .hint.compact { font-size: 10px; }
    .hint.warn { color: var(--warning); }
    .hint.err { color: var(--error); }

    .empty-state { padding: 14px; border: 1px dashed var(--border); border-radius: 5px; color: var(--text-dim); font-size: 12px; margin: 8px 0; }
    .empty-state p { margin: 0 0 8px; }
    .empty-actions { display: flex; gap: 8px; flex-wrap: wrap; }

    .grp-issues { margin: 10px 0 2px; padding-left: 4px; list-style: none; }
    .grp-issue.err { color: var(--error); font-size: 11px; padding: 2px 0; }

    .btn-slot { min-width: 28px; }
</style>
