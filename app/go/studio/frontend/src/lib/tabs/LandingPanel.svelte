<!-- LandingPanel — landing-light groups editor (Lighting tab right column).
     Each group binds 1+ LEDs + 1+ servos and an activation source
     (manual / RC channel / LightFx program).  The .op-cluster
     Activate / Deactivate buttons (Rule 48) drive the firmware
     directly so the operator can bench-test the deploy → wait-for-
     servos → LEDs-on sequence without an RC link.

     Cross-references:
       - Card / form-row / button-cluster      → Rule 34
       - Validation surfacing                  → Rule 35
       - Cross-board port pickers (alias)      → Rule 34 §refOptLabel
       - Operational action cluster            → Rule 48
       - Add/remove list pattern               → Catalog § 14
       - Modular dirty source                  → Rule 46 (landingConfigSource)
-->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import {
        landingDraft, landingDirty, landingPhases,
        loadLandingConfig, refreshLandingStatus,
        addLandingLight, removeLandingLight, updateLandingLight,
        landingActivate, landingDeactivate,
        landingItemErrors,
        installLandingPhaseListener,
        type LandingConfigT, type LandingLightT, type LandingServoT, type LandingLedT,
        type PortRefT,
    } from '../landing'
    import {
        deviceModel, type Port, formatPortRail,
        claimsForPort, RoleKind,
    } from '../devicemodel'
    import { effectClaims } from '../effect-claims'
    import { lightfxDraft, setLandingGroupProgram } from '../lightfx'
    import ServoWidget from '../components/ServoWidget.svelte'
    import type { ServoProfileT } from '../servo_calibration'

    /** Read the device-model profile for `(guid, idx)` so the
     *  ServoWidget can show its summary + pre-seed the calibration
     *  dialog with the current values.  Returns null when the port
     *  has no profile yet (role defaults). */
    function profileFor(port: PortRefT | null | undefined): ServoProfileT | null {
        if (!port || !port.kind) return null
        for (const p of $deviceModel.ports) {
            if (p.ref.guid === port.guid && p.kindName === port.kind && p.ref.index === port.idx) {
                return (p.profile as any) ?? null
            }
        }
        return null
    }
    /** Compact label for the calibration dialog header — same shape
     *  the port-picker shows (alias-aware). */
    function labelFor(port: PortRefT | null | undefined): string {
        if (!port || !port.kind) return ''
        for (const p of $deviceModel.ports) {
            if (p.ref.guid === port.guid && p.kindName === port.kind && p.ref.index === port.idx) {
                return refOptLabel(p)
            }
        }
        return `Servo ${port.idx}`
    }

    let busy = false
    let error = ''

    let cfg: LandingConfigT
    const unsub = landingDraft.subscribe(c => { cfg = c })

    onMount(() => {
        installLandingPhaseListener()
        loadLandingConfig().catch(e => { error = String(e) })
        refreshLandingStatus().catch(() => {})
        return () => unsub()
    })
    onDestroy(() => unsub())

    function mark(): void { landingDraft.set(cfg) }

    // ─── Named-channel option list (Rule 43) ─────────────────────────
    // Reused for the activation-source "input channel" picker.
    type ChanOpt = { fnId: string; label: string }
    $: chanOpts = collectChannelOpts($deviceModel)
    function collectChannelOpts(_dm: typeof $deviceModel): ChanOpt[] {
        const fns = new Map($deviceModel.channelFunctions.map(f => [f.id, f.label] as const))
        const out: ChanOpt[] = []
        for (const inp of $deviceModel.inputs) {
            for (const c of inp.channels) {
                if (c.function === 'unassigned') continue
                out.push({
                    fnId: c.function,
                    label: `CH${c.channel + 1} · ${fns.get(c.function) ?? c.function}`,
                })
            }
        }
        return out
    }

    // ─── Port picker helpers (mirrors GunFxPanel pattern, Rule 34) ──
    function refOptKey(p: Port): string { return `${p.ref.guid}|${p.kindName}|${p.ref.index}` }
    function refOptLabel(p: Port): string {
        const rail  = formatPortRail(p.voltageMv)
        const alias = p.name && p.name.trim()
        const head  = alias ? `${alias} (${p.hardwareName})` : p.hardwareName
        return `${p.boardName ?? 'Hub'} · ${head}${rail ? ` · ${rail}` : ''}`
    }
    function portRefToKey(r: PortRefT): string {
        if (!r || !r.guid || !r.kind) return ''
        return `${r.guid}|${r.kind}|${r.idx}`
    }
    function parsePortOption(key: string, kindName: PortRefT['kind']): PortRefT {
        if (!key) return { board: '', guid: '', kind: kindName, idx: 0 }
        const [guid, kind, idxStr] = key.split('|')
        return { board: '', guid, kind, idx: Number(idxStr) }
    }

    // ─── Unclaimed pool — only show ports that BOTH have the right
    //     role attached AND aren't claimed by some other effect /
    //     sibling landing-light group.  Now uses $effectClaims (hard
    //     domain claims + synthesized claims from every effect draft,
    //     see lib/effect-claims.ts) so GunFx muzzle / LightFx program
    //     channels / sibling landing groups are all visible here. ───
    function isClaimedByOther(p: Port, exceptRefs: PortRefT[]): boolean {
        const claims = claimsForPort($effectClaims, p.ref)
        if (claims.length === 0) return false
        // Exempt this group's own claims (so editing keeps the picker stable).
        for (const c of claims) {
            const sameAsException = exceptRefs.some(r =>
                r.guid === c.port.guid && r.idx === c.port.index)
            if (!sameAsException) return true
        }
        return false
    }
    function freeServos(currentRefs: PortRefT[]): Port[] {
        return $deviceModel.ports.filter(p =>
            p.kindName === 'servo' && p.direction === 'output'
            && p.roleKind === RoleKind.ServoActuator
            && !isClaimedByOther(p, currentRefs))
    }
    function freeLeds(currentRefs: PortRefT[]): Port[] {
        return $deviceModel.ports.filter(p =>
            p.kindName === 'pwm' && p.direction === 'output'
            && p.roleKind === RoleKind.LedAnimator
            && !isClaimedByOther(p, currentRefs))
    }

    const refKey = (guid: string, idx: number) => `${guid}#${idx}`

    // Every servo / LED ref already used across ALL landing groups (this
    // group + siblings).  A landing draft's picks aren't synthesized into
    // $effectClaims, so we exclude them explicitly here — otherwise an
    // already-assigned port still reads as "unclaimed" and lingers in the
    // Add dropdown.
    function usedRefs(which: 'servos'|'leds'): Set<string> {
        const out = new Set<string>()
        for (const l of cfg.lights) {
            const list = which === 'servos' ? l.servos : l.leds
            for (const e of list) out.add(refKey(e.port.guid, e.port.idx))
        }
        return out
    }

    // Ports with `role` attached, not claimed by ANOTHER effect, and not used
    // by ANY landing group — EXCEPT the editing row's own `keep` pick, which is
    // forced in so its picker stays stable.  `keep = null` ⇒ the "+ Add" pool.
    function availPorts(kindName: 'servo'|'pwm', role: number,
                        used: Set<string>, keep: PortRefT | null): Port[] {
        return $deviceModel.ports.filter(p => {
            if (p.kindName !== kindName || p.direction !== 'output' || p.roleKind !== role) return false
            if (keep && keep.guid === p.ref.guid && keep.idx === p.ref.index) return true
            if (isClaimedByOther(p, [])) return false
            return !used.has(refKey(p.ref.guid, p.ref.index))
        })
    }

    // ─── Mutators (per-group) ────────────────────────────────────────
    function setField<K extends keyof LandingLightT>(id: number, key: K, val: LandingLightT[K]) {
        updateLandingLight(id, l => ({ ...l, [key]: val }))
    }
    function setActivationMode(id: number, val: string) {
        const mode = val as LandingLightT['activation']['mode']
        updateLandingLight(id, l => ({ ...l, activation: { ...l.activation, mode } }))
        // Leaving "program" mode detaches this group's binding from every
        // program; entering it re-pushes the current selection (if any).
        const lt = cfg.lights.find(l => l.id === id)
        setLandingGroupProgram(id, mode === 'program' ? (lt?.activation.program ?? '') : '')
    }
    // Pick the program a group attaches to: persist on the group AND push
    // the {id, on} binding into that program's landing_bindings (option-b
    // program-attach — drives the firmware program→landing path).
    function setActivationProgram(id: number, name: string) {
        setActivation(id, 'program', name)
        setLandingGroupProgram(id, name)
    }
    // (whenProgram is fixed to "active" now — a group deploys while its
    // program runs; the firmware path can't express "deploy when inactive".)
    function addServo(id: number, port: PortRefT) {
        updateLandingLight(id, l => ({ ...l, servos: [...l.servos, { port }] }))
    }
    function removeServo(id: number, idx: number) {
        updateLandingLight(id, l => ({ ...l, servos: l.servos.filter((_, i) => i !== idx) }))
    }
    function setServoPort(id: number, idx: number, port: PortRefT) {
        updateLandingLight(id, l => ({
            ...l, servos: l.servos.map((s, i) => i === idx ? { port } : s),
        }))
    }
    function addLed(id: number, port: PortRefT) {
        updateLandingLight(id, l => ({
            ...l, leds: [...l.leds, { port, brightnessPct: 100 }],
        }))
    }
    function removeLed(id: number, idx: number) {
        updateLandingLight(id, l => ({ ...l, leds: l.leds.filter((_, i) => i !== idx) }))
    }
    function setLedPort(id: number, idx: number, port: PortRefT) {
        updateLandingLight(id, l => ({
            ...l, leds: l.leds.map((e, i) => i === idx ? { ...e, port } : e),
        }))
    }
    function setLedBrightness(id: number, idx: number, pct: number) {
        updateLandingLight(id, l => ({
            ...l, leds: l.leds.map((e, i) => i === idx ? { ...e, brightnessPct: pct } : e),
        }))
    }
    function setFadeIn(id: number, ms: number) {
        updateLandingLight(id, l => ({ ...l, fadeInMs: Math.max(0, Math.round(ms || 0)) }))
    }
    function setActivation<K extends keyof LandingLightT['activation']>(id: number, key: K, val: LandingLightT['activation'][K]) {
        updateLandingLight(id, l => ({ ...l, activation: { ...l.activation, [key]: val } }))
    }

    // ─── Live phase pill ─────────────────────────────────────────────
    function phaseFor(id: number) { return $landingPhases[id] }
    function phaseClass(p: number | undefined): string {
        switch (p) {
            case 1: return 'phase-retracted'
            case 2: return 'phase-deploying'
            case 3: return 'phase-deployed'
            case 4: return 'phase-retracting'
            default: return 'phase-unknown'
        }
    }

    // ─── Program picker (for activation.mode === 'program') ─────────
    // Reads the OTHER column's draft so the operator's in-flight
    // LightFx edits are picker-visible immediately.
    // (2026-05-24 refactor: the LightFx config now keeps the active
    // list as `activePrograms[]` — each entry already has the name
    // we want, no path stripping needed.  `?? []` guards against the
    // brief window before lightfxDraft has hydrated.)
    $: programNames = ($lightfxDraft.activePrograms ?? []).map(a => a.name)

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    function resetSelect(e: Event)  { (e.target as HTMLSelectElement).value = '' }
    function onPickServo(lightId: number, e: Event) {
        const k = selValue(e); if (k) { addServo(lightId, parsePortOption(k, 'servo')); resetSelect(e) }
    }
    function onPickLed(lightId: number, e: Event) {
        const k = selValue(e); if (k) { addLed(lightId, parsePortOption(k, 'pwm'));    resetSelect(e) }
    }
</script>

<div class="card landing-card">
    <div class="card-header">
        <h3>Landing Lights</h3>
        <div class="header-actions">
            <button class="small" on:click={() => addLandingLight()} disabled={busy}>+ Add group</button>
        </div>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}

    {#if cfg && cfg.lights.length === 0}
        <div class="empty-state">
            No landing-light groups yet — click <strong>+ Add group</strong> to author the first one.
            Each group bundles 1+ servos + 1+ LEDs; deploy fires the servos and only turns the LEDs
            on after every servo reaches its open position.
        </div>
    {/if}

    {#each (cfg?.lights ?? []) as light (light.id)}
        {@const phase        = phaseFor(light.id)}
        {@const deployed     = phase?.phase === 2 || phase?.phase === 3}
        {@const issues       = landingItemErrors(cfg.lights, cfg.lights.findIndex(l => l.id === light.id))}
        {@const hasErrors    = issues.length > 0}
        {@const usedServos   = usedRefs('servos')}
        {@const usedLeds     = usedRefs('leds')}
        {@const servoAddPool = availPorts('servo', RoleKind.ServoActuator, usedServos, null)}
        {@const ledAddPool   = availPorts('pwm',   RoleKind.LedAnimator,   usedLeds,   null)}
        {@const noFreeServos = freeServos([]).length === 0}
        {@const noFreeLeds   = freeLeds([]).length === 0}
        <div class="card group-card" class:invalid={hasErrors}>
            <div class="card-header inner">
                <h4>{light.name || 'Landing group'}</h4>
                <div class="header-actions">
                    {#if phase}
                        <span class="state-pill phase {phaseClass(phase.phase)}">{phase.name}</span>
                    {/if}
                    <!-- Single on/off toggle (Rule 48): deploy ⇄ retract.
                         ON→OFF (retract) always enabled — emergency
                         cutoff; OFF→ON (deploy) gated on dirty/errors. -->
                    <button class="small state-toggle" class:danger={deployed}
                            on:click={() => deployed ? landingDeactivate(light.id) : landingActivate(light.id)}
                            disabled={deployed ? busy : (busy || $landingDirty || hasErrors)}
                            title={deployed ? 'Retract: LEDs off, then servos → close (always available)'
                                 : $landingDirty ? 'Apply changes before deploying — tests the loaded firmware config'
                                 : hasErrors ? 'Resolve validation errors first'
                                 : 'Deploy: servos → open, then LEDs on'}>
                        {deployed ? 'Retract' : 'Deploy'}
                    </button>
                    <button class="small danger" on:click={() => { setLandingGroupProgram(light.id, ''); removeLandingLight(light.id) }} disabled={busy}>× Remove</button>
                </div>
            </div>

            <!-- Identity — just a friendly name.  The wire `id` and the
                 `owner` (which effect family may drive the group) are auto-
                 assigned under the hood: id from the next free slot, owner
                 derived from the activation mode at save (program ⇒ lightfx so
                 the program's landing_bindings can drive it; otherwise
                 landing-light).  Neither is operator-facing. -->
            <div class="form-row">
                <span class="field-label">Name</span>
                <input class="field-input wide" type="text" maxlength="15"
                       value={light.name}
                       on:input={(e) => setField(light.id, 'name', inputValue(e))}
                       disabled={busy} placeholder="e.g. nose, main, wing searchlight" />
            </div>

            <!-- Servo bank (pool computed at @const block above).
                 Rule 49: section-warn yellow chip when there are zero
                 servos with ServoActuator role attached + unclaimed
                 anywhere on the device — the operator needs to attach
                 the role on the IO tab before this group can fire. -->
            <div class="section-head" class:section-warn={noFreeServos}>
                Servos
                <span class="hint">all move together; LEDs come on only once EVERY servo reaches its open position</span>
                {#if noFreeServos}
                    <span class="section-warn-tag" title="No free servo ports with the ServoActuator role attached.  Open the IO tab and attach ServoActuator to a servo port (or unclaim one from another effect).">
                        no free servos — attach ServoActuator on the IO tab
                    </span>
                {/if}
            </div>
            <div class="form-row">
                <span class="field-label">Open µs</span>
                <input class="field-input narrow" type="number" min="800" max="2200" step="10"
                       value={light.openUs}
                       on:change={(e) => setField(light.id, 'openUs', numValue(e))} disabled={busy} />
                <span class="field-label">Close µs</span>
                <input class="field-input narrow" type="number" min="800" max="2200" step="10"
                       value={light.closeUs}
                       on:change={(e) => setField(light.id, 'closeUs', numValue(e))} disabled={busy} />
            </div>
            {#each light.servos as s, i (i)}
                <div class="form-row">
                    <span class="field-label">Servo #{i + 1}</span>
                    <select class="field-input wide" value={portRefToKey(s.port)}
                            on:change={(e) => setServoPort(light.id, i, parsePortOption(selValue(e), 'servo'))}
                            disabled={busy}>
                        <option value="">— pick a free servo port —</option>
                        {#each availPorts('servo', RoleKind.ServoActuator, usedServos, s.port) as p}
                            <option value={refOptKey(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                    <button class="small danger btn-slot" on:click={() => removeServo(light.id, i)} disabled={busy}>× Remove</button>
                </div>
                <!-- Per-servo profile widget (Rule 44 + new popup pattern):
                     compact summary + reversed toggle + Calibrate button.
                     Profile data flows through the device model (server-
                     authoritative); jog + save go through the popup. -->
                <div class="form-row servo-widget-row">
                    <span class="field-label"></span>
                    <ServoWidget
                        port={s.port}
                        portLabel={labelFor(s.port)}
                        profile={profileFor(s.port)}
                        busy={busy} />
                </div>
            {/each}
            {#if light.servos.length < 4}
                <div class="form-row">
                    <select class="field-input wide"
                            on:change={(e) => onPickServo(light.id, e)}
                            disabled={busy || servoAddPool.length === 0}>
                        <option value="">{servoAddPool.length === 0 ? '— no free servo ports —' : '+ Add servo…'}</option>
                        {#each servoAddPool as p}
                            <option value={refOptKey(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                    {#if servoAddPool.length === 0}
                        <span class="hint warn">No free servo ports — attach a ServoActuator on the IO tab, or free one from another group/effect, to add a servo.</span>
                    {/if}
                </div>
            {/if}

            <!-- LED bank (pool computed at @const block above).
                 Two-tier section state: RED `section-error` when this
                 group has zero LEDs (Rule 35 — the firmware drops
                 LED-less groups silently), YELLOW `section-warn`
                 when the DEVICE has zero free LED-Animator ports
                 anywhere (Rule 49 — operator needs to attach the role
                 on the IO tab). -->
            <div class="section-head"
                 class:section-error={light.leds.length === 0}
                 class:section-warn={light.leds.length > 0 && noFreeLeds}>
                LEDs
                <span class="hint">brightness is per-LED; the firmware uses the FIRST LED's brightness for the group state</span>
                {#if light.leds.length === 0}
                    <span class="section-err-tag">no LEDs — group will not deploy</span>
                {:else if noFreeLeds}
                    <span class="section-warn-tag" title="No free PWM ports with the LedAnimator role attached.  Open the IO tab and attach LedAnimator to add more LEDs to this group.">
                        no free LEDs — attach LedAnimator on the IO tab
                    </span>
                {/if}
            </div>
            {#each light.leds as e, i (i)}
                <div class="form-row">
                    <span class="field-label">LED #{i + 1}</span>
                    <select class="field-input wide" value={portRefToKey(e.port)}
                            on:change={(ev) => setLedPort(light.id, i, parsePortOption(selValue(ev), 'pwm'))}
                            disabled={busy}>
                        <option value="">— pick a free PWM port —</option>
                        {#each availPorts('pwm', RoleKind.LedAnimator, usedLeds, e.port) as p}
                            <option value={refOptKey(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                    <input class="field-input narrow" type="number" min="0" max="100"
                           value={e.brightnessPct}
                           on:change={(ev) => setLedBrightness(light.id, i, numValue(ev))}
                           disabled={busy} title="Brightness % when deployed" />
                    <button class="small danger btn-slot" on:click={() => removeLed(light.id, i)} disabled={busy}>× Remove</button>
                </div>
            {/each}
            {#if light.leds.length < 8}
                <div class="form-row">
                    <select class="field-input wide"
                            on:change={(e) => onPickLed(light.id, e)}
                            disabled={busy || ledAddPool.length === 0}>
                        <option value="">{ledAddPool.length === 0 ? '— no free LED ports —' : '+ Add LED…'}</option>
                        {#each ledAddPool as p}
                            <option value={refOptKey(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                    {#if ledAddPool.length === 0}
                        <span class="hint warn">No free PWM ports — attach a LedAnimator on the IO tab, or free one from another group/effect, to add an LED.</span>
                    {/if}
                </div>
            {/if}

            <!-- LED soft-start ramp (after the servo deploys) -->
            <div class="form-row">
                <span class="field-label">Fade-in</span>
                <input class="field-input narrow" type="number" min="0" max="10000" step="50"
                       value={light.fadeInMs ?? 0}
                       on:change={(ev) => setFadeIn(light.id, numValue(ev))}
                       disabled={busy}
                       title="LED soft-start: ramp 0→brightness over this many ms once the servo is fully deployed (0 = hard on)." />
                <span class="unit">ms</span>
                <span class="hint">ramps the bulbs up after the servo finishes deploying (0 = snap on)</span>
            </div>

            <!-- Activation source -->
            <div class="section-head">
                Activation
                <span class="hint">how the group deploys automatically; "Manual" leaves it to Studio / CLI / wire</span>
            </div>
            <div class="form-row">
                <span class="field-label">Mode</span>
                <select class="field-input wide" value={light.activation.mode}
                        on:change={(e) => setActivationMode(light.id, selValue(e))}
                        disabled={busy}>
                    <option value="manual">Manual only (Studio / CLI / wire)</option>
                    <option value="input_channel">Bound to RC input channel</option>
                    <option value="program">Tied to a LightFx program</option>
                </select>
            </div>
            {#if light.activation.mode === 'input_channel'}
                <div class="form-row">
                    <span class="field-label">Channel</span>
                    <select class="field-input wide" value={light.activation.input}
                            on:change={(e) => setActivation(light.id, 'input', selValue(e))}
                            disabled={busy}>
                        <option value="">— pick a named channel —</option>
                        {#each chanOpts as o}<option value={o.fnId}>{o.label}</option>{/each}
                    </select>
                </div>
                <div class="form-row">
                    <span class="field-label">Deploy when channel ≥</span>
                    <input class="field-input narrow" type="number" min="800" max="2200" step="10"
                           value={light.activation.thresholdUs}
                           on:change={(e) => setActivation(light.id, 'thresholdUs', numValue(e))} disabled={busy} />
                    <span class="unit">µs</span>
                    <span class="trigger-pm">±</span>
                    <input class="field-input narrow" type="number" min="0" max="500" step="5"
                           value={light.activation.hysteresisUs}
                           on:change={(e) => setActivation(light.id, 'hysteresisUs', numValue(e))} disabled={busy} />
                    <span class="unit">µs hysteresis</span>
                </div>
            {:else if light.activation.mode === 'program'}
                <div class="form-row">
                    <span class="field-label">Program</span>
                    <select class="field-input wide" value={light.activation.program}
                            on:change={(e) => setActivationProgram(light.id, selValue(e))}
                            disabled={busy}>
                        <option value="">— pick a program from the left column —</option>
                        {#each programNames as n}<option value={n}>{n}</option>{/each}
                    </select>
                </div>
                <div class="form-row">
                    <span class="hint">deploys while this program is active, retracts when you switch away (writes a <code>landing_bindings</code> entry into the program — also editable in the Lighting program editor)</span>
                </div>
            {/if}

            <!-- Per-row issues -->
            {#if issues.length > 0}
                <ul class="grp-issues">
                    {#each issues as msg}
                        <li class="grp-issue err">⚠ {msg}</li>
                    {/each}
                </ul>
            {/if}
        </div>
    {/each}
</div>

<style>
    .landing-card { margin-bottom: 14px; }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .header-actions button { height: 28px; box-sizing: border-box; }

    .group-card { margin: 6px 0 12px; padding: 8px 10px; background: var(--bg-raised); border-radius: 5px; }
    .group-card.invalid { border-color: var(--error); background: rgba(255,80,80,0.05); }
    .card-header.inner { padding: 4px 0 8px; border-bottom: 1px dashed var(--border); margin-bottom: 8px; }
    .card-header.inner h4 { font-size: 13px; font-weight: 600; color: var(--text-bright); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    .section-head .hint { font-size: 9px; font-weight: 400; text-transform: none; letter-spacing: 0; color: var(--text-dim); font-style: italic; }
    .section-head.section-error { color: var(--error); border-bottom-color: var(--error); }
    .section-err-tag { font-size: 9px; font-weight: 700; color: var(--error); padding: 1px 6px; border: 1px solid var(--error); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }
    /* Rule 49 yellow — non-blocking, surfaces empty role-port pool. */
    .section-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
    .section-warn-tag { font-size: 9px; font-weight: 700; color: var(--warning); padding: 1px 6px; border: 1px solid var(--warning); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }

    /* Phase pill — colour matches the live state so the operator can
       glance at a card and know whether it's commanded mid-motion. */
    .state-pill.phase { font-family: var(--font-mono); font-size: 10px; padding: 2px 8px; border-radius: 3px; background: var(--bg-input); border: 1px solid var(--border); text-transform: uppercase; letter-spacing: 0.4px; }
    .state-pill.phase.phase-retracted  { color: var(--text-dim); }
    .state-pill.phase.phase-deploying  { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-deployed   { color: var(--success); border-color: var(--success); background: rgba(100,200,120,0.12); }
    .state-pill.phase.phase-retracting { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-unknown    { color: var(--text-dim); }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .unit        { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
    .trigger-pm  { margin: 0 4px; color: var(--text-dim); font-family: var(--font-mono); font-weight: 700; }

    .grp-issues { list-style: none; margin: 6px 0 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
    .grp-issue { font-size: 11px; line-height: 1.35; padding-left: 4px; }
    .grp-issue.err { color: var(--error); }
</style>
