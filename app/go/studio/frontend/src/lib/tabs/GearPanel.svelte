<!-- GearPanel — Gear / Undercarriage editor (Gear tab, instructions/29
     §3).  Each channel binds one gear motor (BiDcMotor on an H-bridge)
     + ≤2 door servos (ServoActuator) and the door-pairing / close
     policy.  Deploy / Retract brackets the motor seek with door legs on
     the firmware side; this panel is a pure VIEW on the gear domain's
     stores (Rule 46) — it registers nothing, validates nothing, applies
     nothing.

     Cross-references:
       - Card / form-row / button-cluster      → Rule 34
       - Description typography                 → Rule 50
       - Validation surfacing                  → Rule 35 (red) / Rule 39 (yellow)
       - Output-port pickers (role-filtered)   → Rule 49 (freePortPool)
       - Enable button                         → Rule 45
       - Deploy/Retract + Deploy-all toggle    → Rule 48 (action-toggle)
       - Servo widgets                         → ServoWidget + ServoCalibrationDialog
       - Modular dirty source                  → Rule 46 (gearConfigSource)
-->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import {
        gearDraft, gearDirty, gearHasErrors, gearPhases,
        loadGearConfig, refreshGearStatus,
        addGearChannel, removeGearChannel, setGearEnabled, setGearCoord,
        updateGearChannel, addGearDoor, removeGearDoor,
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
    import SoundRow from '../components/SoundRow.svelte'
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'
    import type { ServoProfileT } from '../servo_calibration'

    let busy = false
    let error = ''

    let cfg: GearConfigT
    const unsub = gearDraft.subscribe(c => { cfg = c })

    // The firmware emits GEAR_PHASE_EVENT on every phase / sub-phase
    // change, but that's a TAG_ASYNC packet on the LOSSY async queue
    // (Rule 53) — a ~50 Hz RC live-view flood can overflow it and drop an
    // event, leaving the pill stale.  So we also POLL GEAR_STATUS on a
    // slow timer; it returns the current phase+subphase of every channel,
    // self-healing within one tick even after a dropped event.
    let statusTimer: ReturnType<typeof setInterval> | undefined
    onMount(() => {
        installGearPhaseListener()
        loadGearConfig().catch(e => { error = String(e) })
        refreshGearStatus().catch(() => {})
        statusTimer = setInterval(() => { refreshGearStatus().catch(() => {}) }, 1500)
    })
    onDestroy(() => {
        unsub()
        if (statusTimer) clearInterval(statusTimer)
        if (motorModalGear !== null) closeMotorModal()
    })

    $: dirty     = $gearDirty
    $: hasErrors = $gearHasErrors

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

    /** Every motor/door ref used across ALL channels (so a port picked by
     *  a sibling channel/row doesn't linger as "free").  `keep` is the
     *  editing row's own pick — always forced into its own pool so the
     *  picker stays stable. */
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

    /** Pool for the motor picker: hbridge ports with BiDcMotor role,
     *  unclaimed, not used by another channel (except this row's `keep`). */
    function motorPool(ports: Port[], claims: any[], used: Set<string>, keep: PortRefT | null): Port[] {
        const exempt = keep && keep.kind ? [{ guid: keep.guid, kind: keep.kind, idx: keep.idx }] : []
        return freePortPool(ports, claims, 'hbridge', RoleKind.BiDcMotor, exempt)
            .filter(p => {
                if (keep && keep.kind && keep.guid === p.ref.guid && keep.idx === p.ref.index) return true
                return !used.has(`${p.ref.guid}#${p.ref.index}`)
            })
    }
    /** Pool for a door servo picker: servo ports with ServoActuator role. */
    function doorPool(ports: Port[], claims: any[], used: Set<string>, keep: PortRefT | null): Port[] {
        const exempt = keep && keep.kind ? [{ guid: keep.guid, kind: keep.kind, idx: keep.idx }] : []
        return freePortPool(ports, claims, 'servo', RoleKind.ServoActuator, exempt)
            .filter(p => {
                if (keep && keep.kind && keep.guid === p.ref.guid && keep.idx === p.ref.index) return true
                return !used.has(`${p.ref.guid}#${p.ref.index}`)
            })
    }

    // Servo profile lookup for the door ServoWidget (Rule 44).  Reactive
    // factory so the closure re-runs whenever $deviceModel changes (the
    // landing-panel "calibration froze on first render" trap).
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

    // ─── RC up/down channel (Rule 36 cluster + Rule 43 named inputs) ──
    // Same option source as EnginePanel's driver channel: every named
    // (non-unassigned) channel from /hubfx.yaml inputs[].
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

    // ─── Transit sounds (Rule 47 SoundRow ×2, both OPTIONAL) ─────────
    // Existence-validated on SD with the engine panel's debounce pattern;
    // an empty path is valid (that direction simply plays nothing).
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

    // (typed `string` because Svelte's {#each} loop alias is un-narrowed —
    // the only call sites pass 'deploy' | 'retract')
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
        { id: 'independent', label: 'Independent', hint: 'each channel deploys/retracts on its own — no cross-channel sync.' },
        { id: 'door_sync',   label: 'Door-sync',   hint: 'all channels open their doors together, then run motors independently.' },
        { id: 'full_sync',   label: 'Full-sync',   hint: 'all channels move in lockstep: doors open together, motors run together, doors close together.' },
        { id: 'sequenced',   label: 'Sequenced',   hint: 'one channel runs its full cycle, then the next — front-to-rear order.' },
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
    /** A channel is "deployed-ish" (the Deploy/Retract toggle shows
     *  Retract) when its phase is Deploying(2) or Deployed(3). */
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

    // ─── Motor modal (deploy/retract test + duty/timeout config) ──────
    // Full hub-forwarded manual jog is a documented TODO (instructions/29
    // §3.4) — this modal stays at deploy/retract bench-test + the
    // duty/timeout config knobs; no role-forwarding bindings invented.
    let motorModalGear: number | null = null
    function openMotorModal(id: number) { motorModalGear = id }
    function closeMotorModal() { motorModalGear = null }
    $: modalChannel = motorModalGear === null ? null : cfg?.gears.find(g => g.id === motorModalGear) ?? null

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
            <!-- Enable is a deliberate ACTION (Rule 45), not a checkbox. -->
            <button class="small state-toggle" class:state-on={cfg?.enabled}
                    on:click={() => setGearEnabled(!cfg?.enabled)} disabled={busy}
                    title={cfg?.enabled ? 'Effect enabled — Apply pushes it; firmware attaches the motor + door roles.'
                                        : 'Effect disabled — channels exist but nothing deploys.'}>
                {cfg?.enabled ? '✓ Enabled' : '▶ Disabled'}
            </button>
            <button class="small" on:click={() => addGearChannel()} disabled={busy}>+ Add channel</button>
        </div>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}

    <!-- Coordination -->
    <div class="form-row">
        <span class="field-label">Coordination</span>
        <select class="field-input wide" value={cfg?.coord}
                on:change={onCoord} disabled={busy}>
            {#each coordOptions as o}<option value={o.id}>{o.label}</option>{/each}
        </select>
    </div>
    <div class="form-row"><span class="field-label"></span><span class="hint">{coordHint}</span></div>

    {#if cfg?.enabled}
        <!-- RC up/down channel (Rule 36 shared cluster + Rule 43 named
             inputs).  One switch drives the WHOLE undercarriage: above the
             threshold retracts (gear up), below deploys; Invert flips.
             Firmware failsafe always deploys on RC loss. -->
        <ChannelToggleCluster
            channelLabel="Up/down channel"
            emptyOption="— manual only —"
            options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
            inputId={cfg.input.name}
            thresholdUs={cfg.input.thresholdUs}
            hysteresisUs={cfg.input.hysteresisUs}
            liveUs={liveUs?.us ?? null}
            liveValid={liveUs?.valid ?? false}
            busy={busy}
            actionVerb={cfg.input.invert ? 'Deploys' : 'Retracts'}
            onChange={(n) => updateGearInput(i => ({
                ...i, name: n.inputId,
                thresholdUs: n.thresholdUs, hysteresisUs: n.hysteresisUs,
            }))} />
        {#if cfg.input.name}
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
        {/if}

        <!-- Transit sounds (Rule 47).  Both OPTIONAL — the matching WAV
             loops on the dedicated Gear mixer channel while any channel is
             moving in that direction, and stops when the set settles.  The
             speaker button cycles ONE shared stereo mask (gear plays one
             transit sound at a time, like the engine). -->
        <div class="section-head" class:section-error={soundsHaveErrors}>
            Transit sounds {#if soundsHaveErrors}<span class="section-err-tag">missing files</span>{/if}
        </div>
        {#each ['deploy', 'retract'] as f}
            <SoundRow
                label={f}
                placeholder={'/sounds/…  (optional)'}
                value={cfg.sounds[f]}
                outputMask={cfg.sounds.outputMask}
                busy={busy}
                required={false}
                error={soundErrors[f]}
                onPathChange={(v) => { updateGearSounds(s => ({ ...s, [f]: v })); scheduleValidate() }}
                onMaskChange={(m) => updateGearSounds(s => ({ ...s, outputMask: m }))}
                onBrowse={() => browseSound(f)}
                onClear={() => clearSound(f)} />
        {/each}
    {/if}

    <!-- Fleet trigger (decision #4) — one Deploy-all / Retract-all action.
         ON→OFF (retract) always available (emergency); OFF→ON gated. -->
    <div class="form-row fleet-row">
        <span class="field-label">All channels</span>
        <button class="small state-toggle"
                on:click={() => safe(() => gearAll(GearAllDeploy))}
                disabled={busy || dirty || hasErrors || !cfg?.enabled}
                title={dirty ? 'Apply changes first — fleet deploy runs the LOADED firmware config'
                     : hasErrors ? 'Resolve validation errors first'
                     : !cfg?.enabled ? 'Enable the effect first'
                     : 'Deploy every channel (GEAR_ALL deploy)'}>
            ▶ Deploy all
        </button>
        <button class="small state-toggle danger"
                on:click={() => safe(() => gearAll(GearAllRetract))}
                disabled={busy}
                title="Retract every channel (GEAR_ALL retract) — always available (emergency cutoff)">
            ■ Retract all
        </button>
        <span class="hint">the one global trigger drives every channel; per-channel buttons below are for bench testing.</span>
    </div>

    {#if cfg && cfg.gears.length === 0}
        <div class="empty-state">
            No undercarriage channels yet — click <strong>+ Add channel</strong> to author the first one.
            Each channel binds a gear motor (BiDcMotor H-bridge) and up to two door servos; deploy opens
            the doors, runs the motor to the endstop, then closes the doors per your policy.
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
        <div class="card group-card" class:invalid={chanErrors}>
            <div class="card-header inner">
                <h4>{gch.name || 'Gear channel'}</h4>
                <div class="header-actions">
                    <span class="state-pill phase {phaseClass($gearPhases[gch.id]?.phase ?? 1)}">{pillText(gch.id)}</span>
                    {#if errored}
                        <button class="small" on:click={() => safe(() => gearReset(gch.id))} disabled={busy}
                                title="Clear the error state (ERROR → Retracted) so deploy/retract are accepted again.">
                            ⟳ Reset
                        </button>
                    {/if}
                    <!-- Rule 48 action-toggle: ON→OFF (retract) always allowed;
                         OFF→ON (deploy) gated on dirty/errors. -->
                    <button class="small state-toggle" class:danger={deployed}
                            on:click={() => safe(() => (deployed ? gearRetract(gch.id) : gearDeploy(gch.id)))}
                            disabled={deployed ? busy : (busy || dirty || chanErrors || !cfg?.enabled)}
                            title={deployed ? 'Retract: open doors → run motor up → close doors (always available)'
                                 : dirty ? 'Apply changes before deploying — tests the loaded firmware config'
                                 : chanErrors ? 'Resolve validation errors first'
                                 : !cfg?.enabled ? 'Enable the effect first'
                                 : 'Deploy: open doors → run motor down → close doors'}>
                        {deployed ? '■ Retract' : '▶ Deploy'}
                    </button>
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
            <div class="section-head" class:section-warn={motorPoolEmpty}>
                Gear motor
                <span class="hint">an H-bridge port with a BiDcMotor role — drives the leg up/down to the endstop.</span>
                {#if motorPoolEmpty}<span class="section-warn-tag">no BiDcMotor port</span>{/if}
            </div>
            <div class="form-row">
                <span class="field-label">Motor</span>
                <select class="field-input wide" value={portRefToKey(gch.motor)}
                        on:change={(e) => onPickMotor(gch.id, e)}
                        disabled={busy || motorPoolEmpty}>
                    <option value="">{motorPoolEmpty ? '— no free BiDcMotor port —' : '— pick a gear motor —'}</option>
                    {#each motorOpts as p}
                        <option value={modelPortKey(p)}>{refOptLabel(p)}</option>
                    {/each}
                </select>
                <button class="small" on:click={() => openMotorModal(gch.id)}
                        disabled={busy || !gch.motor.kind}
                        title="Bench-test deploy/retract + set the motor duty + travel timeout.">
                    ⚙ Motor…
                </button>
            </div>
            {#if motorPoolEmpty}
                <div class="form-row"><span class="field-label"></span>
                    <span class="hint warn">No H-bridge has a BiDcMotor role attached — attach one on the IO tab, then pick it here.</span>
                </div>
            {/if}

            <!-- Doors (Rule 49 servo pool) -->
            <div class="section-head" class:section-warn={doorPoolEmpty}>
                Doors (≤2)
                <span class="hint">door servos open before the motor runs and close after, per the pairing + close policy below.</span>
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
                <!-- Normalized open/close positions (servo-intent rule). -->
                <div class="form-row sub">
                    <span class="field-label">Open / Close</span>
                    <input class="field-input narrow" type="number" min="0" max="10000" step="100"
                           value={d.open}
                           on:change={(e) => setDoorNorm(gch.id, i, 'open', numValue(e))}
                           disabled={busy} title="Normalized OPEN position [0..10000] — 0 = calibrated min end, 10000 = max end." />
                    <span class="unit">open</span>
                    <input class="field-input narrow" type="number" min="0" max="10000" step="100"
                           value={d.close}
                           on:change={(e) => setDoorNorm(gch.id, i, 'close', numValue(e))}
                           disabled={busy} title="Normalized CLOSED position [0..10000]." />
                    <span class="unit">close</span>
                </div>
                <!-- Per-servo calibration widget (Rule 44 + reversed toggle). -->
                <div class="form-row servo-widget-row">
                    <span class="field-label"></span>
                    <ServoWidget
                        port={d.port}
                        portLabel={labelForPort(d.port)}
                        profile={profileForPort(d.port)}
                        busy={busy} />
                </div>
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
                    {#if doorAddPool.length === 0}
                        <span class="hint warn">No free servo ports — attach a ServoActuator on the IO tab to add a door.</span>
                    {/if}
                </div>
            {/if}

            <!-- Door pairing (maps onto openMode) -->
            {#if gch.doors.length >= 1}
                <div class="section-head">
                    Door pairing
                    <span class="hint">how the two doors open: together, staggered by a delay, or one fully then the other.</span>
                </div>
                <div class="form-row radio-row">
                    <label class="radio"><input type="radio" name="dm{gch.id}" value="sequence"
                        checked={gch.doorMode === 'sequence'}
                        on:change={() => setField(gch.id, 'doorMode', 'sequence')} disabled={busy} /> Sequence (one then the other)</label>
                    <label class="radio"><input type="radio" name="dm{gch.id}" value="delay"
                        checked={gch.doorMode === 'delay'}
                        on:change={() => setField(gch.id, 'doorMode', 'delay')} disabled={busy} /> Delay</label>
                    {#if gch.doorMode === 'delay'}
                        <input class="field-input narrow" type="number" min="0" max="10000" step="50"
                               value={gch.doorDelayMs}
                               on:change={(e) => setField(gch.id, 'doorDelayMs', Math.max(0, Math.round(numValue(e))))}
                               disabled={busy} title="Delay before door 2 starts opening (ms)." />
                        <span class="unit">ms</span>
                    {/if}
                    <label class="radio"><input type="radio" name="dm{gch.id}" value="sync"
                        checked={gch.doorMode === 'sync'}
                        on:change={() => setField(gch.id, 'doorMode', 'sync')} disabled={busy} /> Both together</label>
                </div>
                {#if gch.doorMode === 'delay' && (!gch.doorDelayMs || gch.doorDelayMs <= 0)}
                    <div class="form-row"><span class="field-label"></span>
                        <span class="hint err">Delay mode needs a positive delay — set door 2's stagger above.</span>
                    </div>
                {/if}

                <!-- Post-deploy close policy (maps onto closePolicy) -->
                <div class="section-head">
                    After deploy
                    <span class="hint">which doors close once the gear is down (retract re-opens whatever was closed).</span>
                </div>
                <div class="form-row radio-row">
                    <label class="radio"><input type="radio" name="cp{gch.id}" value="both"
                        checked={gch.closePolicy === 'both'}
                        on:change={() => setField(gch.id, 'closePolicy', 'both')} disabled={busy} /> Both close</label>
                    <label class="radio"><input type="radio" name="cp{gch.id}" value="first"
                        checked={gch.closePolicy === 'first'}
                        on:change={() => setField(gch.id, 'closePolicy', 'first')} disabled={busy} /> One closes</label>
                    <label class="radio"><input type="radio" name="cp{gch.id}" value="none"
                        checked={gch.closePolicy === 'none'}
                        on:change={() => setField(gch.id, 'closePolicy', 'none')} disabled={busy} /> None close</label>
                </div>
                {#if gch.closePolicy === 'first' && gch.doors.length < 2}
                    <div class="form-row"><span class="field-label"></span>
                        <span class="hint err">"One closes" needs 2 doors — add a second door or pick a different policy.</span>
                    </div>
                {/if}
            {/if}

            <!-- Per-channel issues -->
            {#if issues.length > 0}
                <ul class="grp-issues">
                    {#each issues as msg}<li class="grp-issue err">⚠ {msg}</li>{/each}
                </ul>
            {/if}
        </div>
    {/each}
</div>

<!-- Motor modal — deploy/retract bench test + duty/timeout config. -->
{#if modalChannel}
    <div class="modal-backdrop" on:click={closeMotorModal} role="presentation">
        <div class="modal" on:click|stopPropagation role="dialog" aria-modal="true">
            <div class="modal-head">
                <h4>Motor · {modalChannel.name || `gear ${modalChannel.id}`}</h4>
                <button class="small" on:click={closeMotorModal}>✕ Close</button>
            </div>
            <div class="modal-body">
                <div class="form-row">
                    <span class="field-label">Live phase</span>
                    <span class="state-pill phase {phaseClass($gearPhases[modalChannel.id]?.phase ?? 1)}">{pillText(modalChannel.id)}</span>
                </div>
                <div class="form-row">
                    <span class="field-label">Bench test</span>
                    <button class="small state-toggle" on:click={() => safe(() => gearDeploy(modalChannel.id))}
                            disabled={busy || dirty || hasErrors}
                            title={dirty ? 'Apply first — tests the loaded config' : 'Deploy this channel'}>
                        ▶ Deploy
                    </button>
                    <button class="small state-toggle danger" on:click={() => safe(() => gearRetract(modalChannel.id))}
                            disabled={busy} title="Retract this channel (always available)">
                        ■ Retract
                    </button>
                    <button class="small" on:click={() => safe(() => gearStop(modalChannel.id))} disabled={busy}
                            title="Brake the motor mid-motion.">Stop</button>
                </div>
                <div class="form-row">
                    <span class="field-label">Deploy duty</span>
                    <input class="field-input narrow" type="number" min="-32767" max="32767" step="1000"
                           value={modalChannel.deployDuty}
                           on:change={(e) => setField(modalChannel.id, 'deployDuty', Math.round(numValue(e)))}
                           disabled={busy} title="Signed H-bridge duty for 'going down' (-32767..32767)." />
                    <span class="hint compact">signed duty for the deploy (down) direction</span>
                </div>
                <div class="form-row">
                    <span class="field-label">Retract duty</span>
                    <input class="field-input narrow" type="number" min="-32767" max="32767" step="1000"
                           value={modalChannel.retractDuty}
                           on:change={(e) => setField(modalChannel.id, 'retractDuty', Math.round(numValue(e)))}
                           disabled={busy} title="Signed H-bridge duty for 'going up' (-32767..32767)." />
                    <span class="hint compact">signed duty for the retract (up) direction</span>
                </div>
                <div class="form-row">
                    <span class="field-label">Travel timeout</span>
                    <input class="field-input narrow" type="number" min="0" max="60000" step="500"
                           value={modalChannel.timeoutMs}
                           on:change={(e) => setField(modalChannel.id, 'timeoutMs', Math.max(0, Math.round(numValue(e))))}
                           disabled={busy} title="Full-travel watchdog (ms) — the seek aborts to ERROR after this. 0 = none." />
                    <span class="unit">ms</span>
                </div>
                <p class="help-text">
                    Endstop calibration (LiveRatio + ceiling guard) lives on the BiDcMotor role, not here —
                    tune it on the IO tab.  Full hub-forwarded manual jog is a planned follow-up.
                </p>
                {#if error}<div class="banner err">{error}</div>{/if}
            </div>
        </div>
    </div>
{/if}

<style>
    .gear-card { margin-bottom: 14px; }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .header-actions button { height: 28px; box-sizing: border-box; }

    .fleet-row { gap: 8px; }
    .fleet-row button { height: 28px; box-sizing: border-box; }

    .group-card { margin: 6px 0 12px; padding: 8px 10px; background: var(--bg-raised); border-radius: 5px; }
    .group-card.invalid { border-color: var(--error); background: rgba(255,80,80,0.05); }
    .card-header.inner { padding: 4px 0 8px; border-bottom: 1px dashed var(--border); margin-bottom: 8px; }
    .card-header.inner h4 { font-size: 13px; font-weight: 600; color: var(--text-bright); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; flex-wrap: wrap; }
    .section-head .hint { font-weight: 400; text-transform: none; letter-spacing: 0; }
    .section-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
    .section-warn-tag { font-size: 9px; font-weight: 700; color: var(--warning); padding: 1px 6px; border: 1px solid var(--warning); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }

    .state-pill.phase { height: 28px; display: inline-flex; align-items: center; box-sizing: border-box; font-family: var(--font-mono); font-size: 10px; padding: 0 8px; border-radius: 3px; background: var(--bg-input); border: 1px solid var(--border); text-transform: uppercase; letter-spacing: 0.4px; }
    .state-pill.phase.phase-retracted  { color: var(--text-dim); }
    .state-pill.phase.phase-deploying  { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-deployed   { color: var(--success); border-color: var(--success); background: rgba(100,200,120,0.12); }
    .state-pill.phase.phase-retracting { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-error      { color: var(--error); border-color: var(--error); background: rgba(255,80,80,0.12); }
    .state-pill.phase.phase-unknown    { color: var(--text-dim); }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .form-row.sub { padding-left: 0; }
    .radio-row { gap: 14px; flex-wrap: wrap; }
    .radio { font-size: 11px; color: var(--text); display: inline-flex; align-items: center; gap: 4px; }

    .grp-issues { list-style: none; margin: 6px 0 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
    .grp-issue { font-size: 11px; line-height: 1.35; padding-left: 4px; }
    .grp-issue.err { color: var(--error); }

    /* Motor modal */
    .modal-backdrop { position: fixed; inset: 0; background: rgba(0,0,0,0.5); display: flex; align-items: center; justify-content: center; z-index: 100; }
    .modal { background: var(--bg-raised); border: 1px solid var(--border); border-radius: 6px; width: 460px; max-width: 90vw; box-shadow: 0 8px 32px rgba(0,0,0,0.4); }
    .modal-head { display: flex; align-items: center; justify-content: space-between; padding: 10px 14px; border-bottom: 1px solid var(--border); }
    .modal-head h4 { font-size: 13px; font-weight: 600; color: var(--text-bright); }
    .modal-body { padding: 12px 14px; display: flex; flex-direction: column; gap: 8px; }
    .modal-body .form-row { align-items: center; gap: 8px; }
</style>
