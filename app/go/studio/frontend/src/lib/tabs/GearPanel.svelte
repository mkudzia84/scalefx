<!-- GearPanel — Gear / Undercarriage editor (Gear tab), regenerated from
     scratch 2026-06-11.

     STRUCTURE (always fully visible — the enable flag gates OPERATION, not
     configuration: an operator authoring a model on the bench sees every
     section from the first open, instead of a bare header):

       Status row     enable toggle · live summary pills · fleet Deploy/Retract
       1 · Control    coordination mode + the one RC up/down channel (Rule 36)
       2 · Sounds     deploy / retract transit loops (Rule 47)
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
       - Modular dirty source                  → Rule 46 (gearConfigSource in gear.ts)
-->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import {
        gearDraft, gearDirty, gearHasErrors, gearPhases,
        loadGearConfig, refreshGearStatus,
        addGearChannel, removeGearChannel, moveGearChannel, setGearEnabled, setGearCoord,
        updateGearChannel, removeGearDoor,
        updateGearInput, updateGearSounds,
        gearItemErrors, installGearPhaseListener,
        gearDeploy, gearRetract, gearStop, gearReset, gearAll,
        GearAllDeploy, GearAllRetract,
        type GearConfigT, type GearChannelT, type CoordMode,
        type PortRefT,
    } from '../gear'
    import {
        deviceModel, liveChannels, liveChannelKey,
        type Port, RoleKind,
    } from '../devicemodel'
    import { effectClaims } from '../effect-claims'
    import { pickFile } from '../filepicker'
    import { freePortPool } from '../components/port_pool'
    import { portRefToKey, modelPortKey, parsePortKey as parsePortKeyRaw, refOptLabel } from '../components/port_keys'
    import { collectChannelOptions } from '../channels'
    import { validateSoundFiles } from '../sound_validation'
    import ServoWidget from '../components/ServoWidget.svelte'
    import ServoIoWidget from '../components/ServoIoWidget.svelte'
    import SoundRow from '../components/SoundRow.svelte'
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'
    import { servoStatus, servoStatusKey, installServoStatusListener, setServoLiveView } from '../servo_status'
    import { makeProfileForPort } from '../servo_calibration'

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
        statusTimer = setInterval(() => { refreshGearStatus().catch(() => {}) }, 1500)
    })
    onDestroy(() => {
        unsub()
        if (statusTimer) clearInterval(statusTimer)
        setServoLiveView(false).catch(() => {})
    })

    $: dirty     = $gearDirty
    $: hasErrors = $gearHasErrors
    $: enabled   = !!cfg?.enabled

    // ─── Live fleet summary (status row) ─────────────────────────────
    $: strutCount = cfg?.gears.length ?? 0
    $: anyMoving  = (cfg?.gears ?? []).some(g => {
        const p = $gearPhases[g.id]?.phase
        return p === 2 || p === 4          // deploying / retracting
    })
    $: anyErrored = (cfg?.gears ?? []).some(g => $gearPhases[g.id]?.phase === 5)
    $: allDeployed  = strutCount > 0 && (cfg?.gears ?? []).every(g => ($gearPhases[g.id]?.phase ?? 1) === 3)
    $: allRetracted = strutCount > 0 && (cfg?.gears ?? []).every(g => ($gearPhases[g.id]?.phase ?? 1) === 1)
    $: fleetText = anyErrored ? 'ERROR' : anyMoving ? 'moving' :
                   allDeployed ? 'deployed' : allRetracted ? 'retracted' : 'mixed'

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
    const coordOptions: { id: CoordMode; label: string; hint: string }[] = [
        { id: 'independent', label: 'Independent', hint: 'each strut deploys/retracts on its own — no cross-strut sync.' },
        { id: 'door_sync',   label: 'Door-sync',   hint: 'all struts open their doors together, then run motors independently.' },
        { id: 'full_sync',   label: 'Full-sync',   hint: 'all struts move in lockstep: doors open together, motors run together, doors close together.' },
        { id: 'sequenced',   label: 'Sequenced',   hint: 'one strut runs its full cycle, then the next — card order, top to bottom (reorder with ↑/↓).' },
    ]
    $: coordHint = coordOptions.find(o => o.id === cfg?.coord)?.hint ?? ''

    // ─── Live phase pill ─────────────────────────────────────────────
    function phaseClass(p: number | undefined): string {
        switch (p) {
            case 1: return 'phase-retracted'
            case 2: return 'phase-deploying'
            case 3: return 'phase-deployed'
            case 4: return 'phase-retracting'
            case 5: return 'phase-error'
            default: return 'phase-unknown'
        }
    }
    function pillText(id: number): string {
        const ph = $gearPhases[id]
        if (!ph) return 'Retracted'
        const sub = (ph.subPhase && ph.subPhase !== 0) ? ` · ${ph.subPhaseName}` : ''
        return `${ph.phaseName}${sub}`
    }
    function isDeployed(id: number): boolean {
        const ph = $gearPhases[id]?.phase
        return ph === 2 || ph === 3
    }
    function isErrored(id: number): boolean {
        return $gearPhases[id]?.phase === 5
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
    function setDoorPort(id: number, idx: number, port: PortRefT) {
        updateGearChannel(id, g => ({ ...g, doors: g.doors.map((d, i) => i === idx ? { ...d, port } : d) }))
    }
    function setDoorNorm(id: number, idx: number, which: 'open' | 'close', val: number) {
        const v = Math.max(0, Math.min(10000, Math.round(val || 0)))
        updateGearChannel(id, g => ({ ...g, doors: g.doors.map((d, i) => i === idx ? { ...d, [which]: v } : d) }))
    }
    function onPickMotor(id: number, e: Event) {
        const k = selValue(e); if (k) setMotorPort(id, parsePortKey(k, 'hbridge'))
    }
    function onPickDoor(id: number, e: Event) {
        const k = selValue(e); if (k) { addGearDoorWithPort(id, parsePortKey(k, 'servo')); resetSelect(e) }
    }
    function addGearDoorWithPort(id: number, port: PortRefT) {
        updateGearChannel(id, g => g.doors.length >= 2 ? g
            : ({ ...g, doors: [...g.doors, { port, open: 10000, close: 0 }] }))
    }

    async function safe(fn: () => Promise<void>) {
        busy = true; error = ''
        try { await fn() } catch (e) { error = String(e) } finally { busy = false }
        refreshGearStatus().catch(() => {})
    }
</script>

<div class="card gear-card">
    <div class="card-header">
        <h3>Gear / Undercarriage</h3>
        <div class="header-actions">
            <!-- Enable is a deliberate ACTION (Rule 45).  It gates OPERATION
                 (deploy/retract + the RC channel firing); configuration below
                 stays editable either way so a bench setup can be authored
                 before the first enable. -->
            <button class="small state-toggle" class:state-on={enabled}
                    on:click={() => setGearEnabled(!enabled)} disabled={busy}
                    title={enabled ? 'Effect enabled — Apply pushes it; firmware attaches the motor + door roles.'
                                   : 'Effect disabled — configuration is kept but nothing deploys. Click to enable.'}>
                {enabled ? '✓ Enabled' : '▶ Disabled'}
            </button>
            <button class="small" on:click={() => addGearChannel()} disabled={busy}
                    title="Add an undercarriage strut (nose, main left, main right, …) — each binds one gear motor + up to two door servos.">
                + Add strut
            </button>
        </div>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}

    <!-- Status row (Engine pattern): live fleet summary + the ONE global
         deploy/retract trigger.  ON→OFF (retract) always available
         (emergency); OFF→ON gated on clean config (Rule 35/48). -->
    <div class="status-row">
        <div class="status">
            <span class="status-label">Fleet</span>
            <span class="state-pill phase {anyErrored ? 'phase-error' : anyMoving ? 'phase-deploying' : allDeployed ? 'phase-deployed' : 'phase-retracted'}">
                {strutCount === 0 ? 'no struts' : fleetText}
            </span>
            {#if strutCount > 0}<span class="hint compact">{strutCount} strut{strutCount === 1 ? '' : 's'} · {cfg?.coord?.replace('_', '-')}</span>{/if}
            {#if !enabled}<span class="hint compact warn">disabled — configuration only</span>{/if}
        </div>
        <div class="controls">
            <button class="small state-toggle"
                    on:click={() => safe(() => gearAll(GearAllDeploy))}
                    disabled={busy || dirty || hasErrors || !enabled || strutCount === 0}
                    title={dirty ? 'Apply changes first — fleet deploy runs the LOADED firmware config'
                         : hasErrors ? 'Resolve validation errors first'
                         : !enabled ? 'Enable the effect first'
                         : strutCount === 0 ? 'Add a strut first'
                         : 'Deploy every strut (GEAR_ALL deploy)'}>
                ▶ Deploy all
            </button>
            <button class="small state-toggle danger"
                    on:click={() => safe(() => gearAll(GearAllRetract))}
                    disabled={busy || strutCount === 0}
                    title="Retract every strut (GEAR_ALL retract) — always available (emergency cutoff)">
                ■ Retract all
            </button>
        </div>
    </div>

    <!-- ═══ 1 · Control ════════════════════════════════════════════════ -->
    <div class="section-head">
        Control
        <span class="hint">how the whole set is commanded — coordination across struts + the one RC up/down channel.</span>
    </div>
    <!-- Mode choice ≤4 options → segmented toggle (Rule 60.3). -->
    <div class="form-row">
        <span class="field-label">Coordination</span>
        <div class="seg-select">
            {#each coordOptions as o}
                <button class="seg" class:on={cfg?.coord === o.id}
                        on:click={() => setGearCoord(o.id)} disabled={busy}
                        title={o.hint}>{o.label}</button>
            {/each}
        </div>
    </div>
    <div class="form-row"><span class="field-label"></span><span class="hint">{coordHint}</span></div>

    <!-- RC up/down channel (Rule 36 cluster + Rule 43 named inputs).  One
         switch drives the WHOLE undercarriage: above the threshold retracts
         (gear up), below deploys; Invert flips.  Firmware failsafe always
         deploys on RC loss. -->
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
        actionVerb={cfg?.input?.invert ? 'Deploys' : 'Retracts'}
        onChange={(n) => updateGearInput(i => ({
            ...i, name: n.inputId,
            thresholdUs: n.thresholdUs, hysteresisUs: n.hysteresisUs,
        }))} />
    {#if cfg?.input?.name}
        <div class="form-row">
            <span class="field-label">Direction</span>
            <button class="small state-toggle" class:state-on={cfg.input.invert}
                    on:click={() => updateGearInput(i => ({ ...i, invert: !i.invert }))}
                    disabled={busy}
                    title={cfg.input.invert
                        ? 'Inverted: above the threshold DEPLOYS (gear down). Click for normal.'
                        : 'Normal: above the threshold RETRACTS (gear up). Click to invert.'}>
                {cfg.input.invert ? '↑ high = deploy' : '↑ high = retract'}
            </button>
            <span class="hint">RC-loss failsafe always lowers the gear, whichever direction you pick.</span>
        </div>
    {:else if chanOpts.length === 0}
        <div class="form-row"><span class="field-label"></span>
            <span class="hint warn">No named input channels — name one in /hubfx.yaml's inputs (IO tab) to drive the gear from the radio.</span>
        </div>
    {/if}

    <!-- ═══ 2 · Transit sounds ═════════════════════════════════════════ -->
    <div class="section-head" class:section-error={soundsHaveErrors}>
        Transit sounds
        <span class="hint">optional — the matching WAV loops on the dedicated Gear audio channel while any strut is moving, and stops when the set settles.</span>
        {#if soundsHaveErrors}<span class="section-err-tag">missing files</span>{/if}
    </div>
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

    <!-- ═══ 3 · Struts ═════════════════════════════════════════════════ -->
    <div class="section-head">
        Struts
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
            <button class="small" on:click={() => addGearChannel()} disabled={busy}>+ Add the first strut</button>
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
        {@const deployed     = isDeployed(gch.id)}
        {@const errored      = isErrored(gch.id)}
        {@const order        = cfg.gears.findIndex(g => g.id === gch.id)}
        <div class="card group-card" class:invalid={chanErrors}>
            <div class="card-header inner">
                <h4>
                    {#if cfg.coord === 'sequenced'}<span class="seq-badge" title="Sequenced order — cards run top-to-bottom; this strut cycles in slot {order + 1}.">#{order + 1}</span>{/if}
                    {gch.name || `Strut ${order + 1}`}
                </h4>
                <div class="header-actions">
                    <span class="state-pill phase {phaseClass($gearPhases[gch.id]?.phase ?? 1)}">{pillText(gch.id)}</span>
                    {#if errored}
                        <button class="small" on:click={() => safe(() => gearReset(gch.id))} disabled={busy}
                                title="Clear the error state (ERROR → Retracted) so deploy/retract are accepted again.">
                            ⟳ Reset
                        </button>
                    {/if}
                    <!-- Per-strut op-cluster (Rule 48): bench-test this leg
                         alone.  ON→OFF (retract) always allowed; OFF→ON gated. -->
                    <button class="small state-toggle" class:danger={deployed}
                            on:click={() => safe(() => (deployed ? gearRetract(gch.id) : gearDeploy(gch.id)))}
                            disabled={deployed ? busy : (busy || dirty || chanErrors || !enabled)}
                            title={deployed ? 'Retract: open doors → run motor up → close doors (always available)'
                                 : dirty ? 'Apply changes before deploying — tests the loaded firmware config'
                                 : chanErrors ? 'Resolve validation errors first'
                                 : !enabled ? 'Enable the effect first'
                                 : 'Deploy: open doors → run motor down → close doors'}>
                        {deployed ? '■ Retract' : '▶ Deploy'}
                    </button>
                    <button class="small" on:click={() => safe(() => gearStop(gch.id))} disabled={busy}
                            title="Brake the motor mid-motion (does not clear an error — use Reset).">Stop</button>
                    <button class="small btn-slot" on:click={() => moveGearChannel(gch.id, -1)}
                            disabled={busy || order === 0}
                            title="Move up — in sequenced coordination this strut cycles earlier.">↑</button>
                    <button class="small btn-slot" on:click={() => moveGearChannel(gch.id, 1)}
                            disabled={busy || order === cfg.gears.length - 1}
                            title="Move down — in sequenced coordination this strut cycles later.">↓</button>
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
                            <!-- Drive parameters (Rule 60.6 grid).  Signed
                                 duty: deploy normally +, retract −. -->
                            <div class="form-grid cols-2">
                                <div class="form-field">
                                    <span class="field-label">Deploy duty</span>
                                    <input class="field-input narrow" type="number" min="-32767" max="32767" step="1000"
                                           value={gch.deployDuty}
                                           on:change={(e) => setField(gch.id, 'deployDuty', Math.round(numValue(e)))}
                                           disabled={busy} title="Signed H-bridge duty for 'going down' (-32767..32767)." />
                                </div>
                                <div class="form-field">
                                    <span class="field-label">Retract duty</span>
                                    <input class="field-input narrow" type="number" min="-32767" max="32767" step="1000"
                                           value={gch.retractDuty}
                                           on:change={(e) => setField(gch.id, 'retractDuty', Math.round(numValue(e)))}
                                           disabled={busy} title="Signed H-bridge duty for 'going up' (-32767..32767)." />
                                </div>
                                <div class="form-field">
                                    <span class="field-label">Travel timeout (ms)</span>
                                    <input class="field-input narrow" type="number" min="0" max="60000" step="500"
                                           value={gch.timeoutMs}
                                           on:change={(e) => setField(gch.id, 'timeoutMs', Math.max(0, Math.round(numValue(e))))}
                                           disabled={busy} title="Full-travel watchdog (ms) — the endstop seek aborts to ERROR after this. 0 = seek until stall." />
                                </div>
                            </div>
                            <div class="form-row">
                                <span class="hint">endstop calibration lives on the BiDcMotor role — tune it on the IO tab.</span>
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
                            <!-- Normalized positions (servo-intent: 0 =
                                 calibrated min end, 10000 = max). -->
                            <div class="form-row sub">
                                <span class="field-label">Open / Close</span>
                                <input class="field-input narrow" type="number" min="0" max="10000" step="100"
                                       value={d.open}
                                       on:change={(e) => setDoorNorm(gch.id, i, 'open', numValue(e))}
                                       disabled={busy} title="Normalized OPEN position [0..10000]." />
                                <span class="unit">open</span>
                                <input class="field-input narrow" type="number" min="0" max="10000" step="100"
                                       value={d.close}
                                       on:change={(e) => setDoorNorm(gch.id, i, 'close', numValue(e))}
                                       disabled={busy} title="Normalized CLOSED position [0..10000]." />
                                <span class="unit">close</span>
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
</div>

<style>
    .gear-card { margin-bottom: 14px; }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .header-actions button { height: 28px; box-sizing: border-box; }

    /* Status row (Engine pattern) */
    .status-row { display: flex; align-items: center; justify-content: space-between; gap: 10px; padding: 6px 0 10px; border-bottom: 1px solid var(--border); margin-bottom: 4px; flex-wrap: wrap; }
    .status { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
    .status-label { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-dim); }
    .controls { display: flex; align-items: center; gap: 8px; }
    .controls button { height: 28px; box-sizing: border-box; }

    /* Rule 60.8 background hierarchy: the strut card keeps the STANDARD
       nested-.card chrome (--bg-surface, like GunFx's gun/smoke cards);
       only the .sub-frame subsystem boxes inside it go --bg-raised. */
    .group-card { margin: 6px 0 12px; }
    .group-card.invalid { border-color: var(--error); background: rgba(255,80,80,0.05); }
    .card-header.inner { padding: 4px 0 8px; border-bottom: 1px dashed var(--border); margin-bottom: 8px; }
    .card-header.inner h4 { font-size: 13px; font-weight: 600; color: var(--text-bright); }
    .seq-badge { font-family: var(--font-mono); font-size: 10px; font-weight: 700; color: var(--accent); border: 1px solid var(--accent); border-radius: 3px; padding: 1px 5px; margin-right: 6px; vertical-align: 1px; }

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
    .state-pill.phase.phase-unknown    { color: var(--text-dim); }

    .form-row.sub { margin-left: 0; }

    .servo-widget-row { margin-top: 2px; }

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

    .form-field { display: flex; flex-direction: column; gap: 3px; }
    .form-field .field-label { font-size: 11px; color: var(--text-dim); }

    .hint.compact { font-size: 10px; }
    .hint.warn { color: var(--warning); }
    .hint.err { color: var(--error); }

    .empty-state { padding: 14px; border: 1px dashed var(--border); border-radius: 5px; color: var(--text-dim); font-size: 12px; margin: 8px 0; }
    .empty-state p { margin: 0 0 8px; }

    .grp-issues { margin: 10px 0 2px; padding-left: 4px; list-style: none; }
    .grp-issue.err { color: var(--error); font-size: 11px; padding: 2px 0; }

    .btn-slot { min-width: 28px; }
</style>
