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
        type Port, formatPortRail, RoleKind,
    } from '../devicemodel'
    import { checkFiles } from '../effects'
    import { pickFile } from '../filepicker'
    import { freePortPool } from '../components/port_pool'
    import ServoWidget from '../components/ServoWidget.svelte'
    import ServoIoWidget from '../components/ServoIoWidget.svelte'
    import SoundRow from '../components/SoundRow.svelte'
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'
    import { servoStatus, servoStatusKey, installServoStatusListener, setServoLiveView } from '../servo_status'
    import type { ServoProfileT } from '../servo_calibration'

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

    // ─── Port-picker helpers (Rule 34 / 49) ──────────────────────────
    function portRefToKey(p: PortRefT): string {
        if (!p || !p.kind) return ''
        return `${p.guid}|${p.kind}|${p.idx}`
    }
    function modelPortKey(p: Port): string {
        return `${p.ref.guid}|${p.kindName}|${p.ref.index}`
    }
    function parsePortKey(key: string, kind: string): PortRefT {
        const [guid, , idxStr] = key.split('|')
        return { board: '', guid, kind, idx: Number(idxStr) }
    }
    function refOptLabel(p: Port): string {
        const rail  = formatPortRail(p.voltageMv)
        const alias = p.name && p.name.trim()
        const head  = alias ? `${alias} (${p.hardwareName})` : p.hardwareName
        return `${p.boardName ?? 'Hub'} · ${head}${rail ? ` · ${rail}` : ''}`
    }

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

    // Servo profile lookup (Rule 44) — reactive factory so the closure
    // re-runs whenever $deviceModel changes (the landing-panel trap).
    function makeProfileForPort(dm: typeof $deviceModel) {
        return (port: PortRefT | null | undefined): ServoProfileT | null => {
            if (!port || !port.kind) return null
            for (const p of dm.ports) {
                if (p.ref.guid === port.guid && p.kindName === port.kind
                    && p.ref.index === port.idx && p.profile) {
                    return { ...p.profile } as ServoProfileT
                }
            }
            return null
        }
    }
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
    type ChanOpt = { fnId: string; label: string; portGuid: string; portIdx: number; channel: number }
    $: chanOpts = collectChannels($deviceModel)
    function collectChannels(_dm: typeof $deviceModel): ChanOpt[] {
        const fns = new Map($deviceModel.channelFunctions.map(f => [f.id, f.label] as const))
        const out: ChanOpt[] = []
        for (const inp of $deviceModel.inputs) {
            for (const c of inp.channels) {
                if (c.function === 'unassigned') continue
                out.push({
                    fnId: c.function,
                    label: `CH${c.channel + 1} · ${fns.get(c.function) ?? c.function}`,
                    portGuid: inp.port.guid, portIdx: inp.port.index, channel: c.channel,
                })
            }
        }
        return out
    }
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
        const next = { deploy: '', retract: '' }
        const s = cfg?.sounds
        if (s) {
            for (const k of ['deploy', 'retract'] as const) {
                const p = s[k]
                if (p && !p.startsWith('/')) next[k] = 'path must be absolute (start with /)'
            }
            const probe = [s.deploy, s.retract].filter(p => !!p && p.startsWith('/'))
            if (probe.length > 0) {
                const exists = await checkFiles(probe)
                for (const k of ['deploy', 'retract'] as const) {
                    const p = s[k]
                    if (p && p.startsWith('/') && !exists[p]) next[k] = `file not found on SD: ${p}`
                }
            }
        }
        soundErrors = next
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

    function onCoord(e: Event) { setGearCoord(selValue(e) as CoordMode) }
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
    <div class="form-row">
        <span class="field-label">Coordination</span>
        <select class="field-input wide" value={cfg?.coord}
                on:change={onCoord} disabled={busy}>
            {#each coordOptions as o}<option value={o.id}>{o.label}</option>{/each}
        </select>
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
        {@const issues       = gearItemErrors(cfg.gears, cfg.gears.findIndex(g => g.id === gch.id), $deviceModel.ports, $deviceModel.claims)}
        {@const chanErrors   = issues.length > 0}
        {@const usedMotors   = usedMotorRefs(cfg.gears)}
        {@const usedDoors    = usedDoorRefs(cfg.gears)}
        {@const motorOpts    = motorPool($deviceModel.ports, $deviceModel.claims, usedMotors, gch.motor)}
        {@const motorPoolEmpty= motorOpts.length === 0 && (!gch.motor || !gch.motor.kind)}
        {@const doorAddPool  = doorPool($deviceModel.ports, $deviceModel.claims, usedDoors, null)}
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

            <!-- Gear motor (Rule 49 role-filtered pool + Rule 39 yellow warn) -->
            <div class="section-head sub" class:section-warn={motorPoolEmpty}>
                Gear motor
                <span class="hint">an H-bridge port with a BiDcMotor role — drives the leg to the endstop (stall-detected on the expander).</span>
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
                <div class="form-row"><span class="field-label"></span>
                    <span class="hint warn">No H-bridge has a BiDcMotor role attached — attach one on the IO tab, then pick it here.</span>
                </div>
            {/if}
            {#if gch.motor.kind}
                <!-- Drive parameters, inline (config belongs on the panel;
                     modals are for live-tune dialogs only).  Signed duty:
                     deploy is normally +, retract −; swap signs if the leg
                     runs backwards. -->
                <div class="form-grid cols-3">
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
                <div class="form-row"><span class="field-label"></span>
                    <span class="hint">endstop calibration (LiveRatio + ceiling guard) lives on the BiDcMotor role — tune it on the IO tab.</span>
                </div>
            {/if}

            <!-- Doors (Rule 49 servo pool) -->
            <div class="section-head sub" class:section-warn={doorPoolEmpty}>
                Doors (≤2)
                <span class="hint">door servos open before the motor runs and close after, per the sequencing below.</span>
                {#if doorPoolEmpty}<span class="section-warn-tag">no ServoActuator port</span>{/if}
            </div>
            {#each gch.doors as d, i (i)}
                {@const dPool = doorPool($deviceModel.ports, $deviceModel.claims, usedDoors, d.port)}
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
                <!-- Normalized open/close positions (servo-intent rule —
                     0 = calibrated min end, 10000 = max end; the role's
                     calibration + REV flag do the µs mapping). -->
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
                <!-- Per-servo calibration (Rule 44: ↔ Reversed + ⚙ Calibrate…) -->
                <div class="form-row servo-widget-row">
                    <span class="field-label"></span>
                    <ServoWidget
                        port={d.port}
                        portLabel={labelForPort(d.port)}
                        profile={profileForPort(d.port)}
                        busy={busy} />
                </div>
                <!-- Live door mirror (Rule 42/53): position + target from the
                     20 Hz servo-status stream — watch the door travel during
                     a deploy test.  Servo side only (doors have no RC input). -->
                {#if d.port && d.port.kind}
                    {@const dProf = profileForPort(d.port) ?? ({ minUs: 1000, maxUs: 2000, centerUs: 1500, reversed: false, maxSpeedUsPerSec: 0, maxAccelUsPerSec2: 0, maxJerkUsPerSec3: 0 })}
                    {@const dSv = $servoStatus[servoStatusKey(d.port.guid, d.port.idx)]}
                    <div class="form-row servo-widget-row">
                        <span class="field-label"></span>
                        <ServoIoWidget
                            hasInput={false}
                            inputUs={null}
                            inputValid={false}
                            neutralUs={1500}
                            hasServo={true}
                            minUs={dProf.minUs} maxUs={dProf.maxUs} centerUs={dProf.centerUs} reversed={dProf.reversed}
                            servo={dSv ?? null} />
                    </div>
                {/if}
            {/each}
            {#if gch.doors.length < 2}
                <div class="form-row">
                    <span class="field-label"></span>
                    <select class="field-input wide" on:change={(e) => onPickDoor(gch.id, e)}
                            disabled={busy || doorAddPool.length === 0}>
                        <option value="">{doorAddPool.length === 0 ? '— no free servo ports —' : '+ Add door…'}</option>
                        {#each doorAddPool as p}
                            <option value={modelPortKey(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                    {#if doorAddPool.length === 0 && gch.doors.length === 0}
                        <span class="hint">no doors is fine — the motor leg runs bare; attach ServoActuators on the IO tab to add doors.</span>
                    {/if}
                </div>
            {/if}

            <!-- Door sequencing (maps onto openMode + closePolicy) -->
            {#if gch.doors.length >= 1}
                <div class="section-head sub">
                    Door sequencing
                    <span class="hint">how the doors open (the motor waits for them) and which close again once the gear is down.</span>
                </div>
                <div class="form-row radio-row">
                    <span class="field-label">Opening</span>
                    <label class="radio"><input type="radio" name="dm{gch.id}" value="sync"
                        checked={gch.doorMode === 'sync'}
                        on:change={() => setField(gch.id, 'doorMode', 'sync')} disabled={busy} /> Both together</label>
                    <label class="radio"><input type="radio" name="dm{gch.id}" value="delay"
                        checked={gch.doorMode === 'delay'}
                        on:change={() => setField(gch.id, 'doorMode', 'delay')} disabled={busy} /> Staggered delay</label>
                    {#if gch.doorMode === 'delay'}
                        <input class="field-input narrow" type="number" min="0" max="10000" step="50"
                               value={gch.doorDelayMs}
                               on:change={(e) => setField(gch.id, 'doorDelayMs', Math.max(0, Math.round(numValue(e))))}
                               disabled={busy} title="Delay before door 2 starts opening (ms)." />
                        <span class="unit">ms</span>
                    {/if}
                    <label class="radio"><input type="radio" name="dm{gch.id}" value="sequence"
                        checked={gch.doorMode === 'sequence'}
                        on:change={() => setField(gch.id, 'doorMode', 'sequence')} disabled={busy} /> One fully, then the other</label>
                </div>
                {#if gch.doorMode === 'delay' && (!gch.doorDelayMs || gch.doorDelayMs <= 0)}
                    <div class="form-row"><span class="field-label"></span>
                        <span class="hint err">Delay mode needs a positive delay — set door 2's stagger above.</span>
                    </div>
                {/if}
                <div class="form-row radio-row">
                    <span class="field-label">After deploy</span>
                    <label class="radio"><input type="radio" name="cp{gch.id}" value="both"
                        checked={gch.closePolicy === 'both'}
                        on:change={() => setField(gch.id, 'closePolicy', 'both')} disabled={busy} /> Both close</label>
                    <label class="radio"><input type="radio" name="cp{gch.id}" value="first"
                        checked={gch.closePolicy === 'first'}
                        on:change={() => setField(gch.id, 'closePolicy', 'first')} disabled={busy} /> One closes</label>
                    <label class="radio"><input type="radio" name="cp{gch.id}" value="none"
                        checked={gch.closePolicy === 'none'}
                        on:change={() => setField(gch.id, 'closePolicy', 'none')} disabled={busy} /> None close</label>
                    <span class="hint compact">retract re-stows whatever was opened.</span>
                </div>
                {#if gch.closePolicy === 'first' && gch.doors.length < 2}
                    <div class="form-row"><span class="field-label"></span>
                        <span class="hint err">"One closes" needs 2 doors — add a second door or pick a different policy.</span>
                    </div>
                {/if}
            {/if}

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

    .group-card { margin: 6px 0 12px; padding: 8px 10px; background: var(--bg-raised); border-radius: 5px; }
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
    .form-row.radio-row { gap: 12px; flex-wrap: wrap; }
    .radio { display: inline-flex; align-items: center; gap: 5px; font-size: 12px; color: var(--text); cursor: pointer; }
    .radio input { cursor: pointer; }

    .servo-widget-row { margin-top: 2px; }

    .form-grid.cols-3 { display: grid; grid-template-columns: repeat(3, minmax(120px, 1fr)); gap: 8px 12px; margin: 6px 0; }
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
