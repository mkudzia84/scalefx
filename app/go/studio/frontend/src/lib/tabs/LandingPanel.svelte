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
        addLandingLight, removeLandingLight, updateLandingLight, saveLandingConfig,
        landingActivate, landingDeactivate,
        landingItemErrors,
        installLandingPhaseListener,
        type LandingConfigT, type LandingLightT, type LandingServoT, type LandingLedT,
        type PortRefT,
    } from '../landing'
    import {
        deviceModel, type Port, type Claim,
        claimsForPort, RoleKind, liveChannels, liveChannelKey,
    } from '../devicemodel'
    import { effectClaims } from '../effect-claims'
    import { lightfxDraft, setLandingGroupProgram } from '../lightfx'
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'
    import ServoWidget from '../components/ServoWidget.svelte'
    import ServoIoWidget from '../components/ServoIoWidget.svelte'
    import { portRefToKey, modelPortKey, parsePortKey, refOptLabel } from '../components/port_keys'
    import { servoStatus, servoStatusKey, installServoStatusListener, setServoLiveView } from '../servo_status'
    import { collectChannelOptions } from '../channels'
    import { makeProfileForPort } from '../servo_calibration'
    import { openEndpointsFor, resolveEndpointUs, summariseEndpoints, US_CAL_MAX } from '../endpoint_setter'

    // Shared reactive servo-profile factory (servo_calibration.ts) — rebuilds
    // on $deviceModel change so a Calibrate→Save isn't frozen on the old value.
    $: profileForPort = makeProfileForPort($deviceModel)
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

    // The firmware emits LL_PHASE_EVENT on every phase change, but that's a
    // TAG_ASYNC packet on the LOSSY async queue (Rule 53) — the ~50 Hz RC
    // live-view broadcasts can overflow it and drop a phase event, leaving the
    // pill stale.  So in addition to the event stream we POLL LL_STATUS on a
    // slow timer: it returns the current phase of every group, so the state
    // self-heals within one tick even if an event was dropped.
    let statusTimer: ReturnType<typeof setInterval> | undefined
    onMount(() => {
        installLandingPhaseListener()
        loadLandingConfig().catch(e => { error = String(e) })
        refreshLandingStatus().catch(() => {})
        statusTimer = setInterval(() => { refreshLandingStatus().catch(() => {}) }, 1500)
        // Live servo-position stream (20 Hz) for the ServoIoWidget output bars.
        installServoStatusListener()
        setServoLiveView(true).catch(() => {})
        return () => unsub()
    })
    onDestroy(() => {
        unsub()
        if (statusTimer) clearInterval(statusTimer)
        setServoLiveView(false).catch(() => {})
    })

    function mark(): void { landingDraft.set(cfg) }

    /** Endpoints dialog for a landing group's open/close positions (all
     *  servos in the group get the same targets; the FIRST servo is used
     *  for the live jog).  Save persists /landing.yaml immediately. */
    function openLandingEndpoints(light: LandingLightT) {
        const first = light.servos[0]?.port
        if (!first || !first.kind) return
        const prof = profileForPort(first) ?? { minUs: 1000, maxUs: 2000, centerUs: 1500, maxSpeedUsPerSec: 0, maxAccelUsPerSec2: 0, maxJerkUsPerSec3: 0 }
        openEndpointsFor({
            guid: first.guid ?? '', portIdx: first.idx,
            portLabel: labelFor(first),
            labelA: 'Open', labelB: 'Closed',
            minUs: prof.minUs, maxUs: prof.maxUs,
            aUs: light.openUs, bUs: light.closeUs,
            onSave: async (aUs, bUs) => {
                updateLandingLight(light.id, l => ({ ...l, openUs: aUs, closeUs: bUs }))
                await saveLandingConfig()
            },
        })
    }

    // ─── Named-channel option list (Rule 43) ─────────────────────────
    // Reused for the activation-source "input channel" picker.
    $: chanOpts = collectChannelOptions($deviceModel)

    // ─── Port picker helpers (mirrors GunFxPanel pattern, Rule 34) ──
    // The `guid|kind|idx` key builders + the alias-aware refOptLabel live in
    // the shared, unit-tested port_keys module (aliased to the historical
    // names so call sites are unchanged) — see port_keys.test.ts.
    const refOptKey = modelPortKey
    const parsePortOption = (key: string, kindName: PortRefT['kind']) =>
        parsePortKey(key, kindName) as PortRefT

    // ─── Unclaimed pool — only show ports that BOTH have the right
    //     role attached AND aren't claimed by some other effect /
    //     sibling landing-light group.  Now uses $effectClaims (hard
    //     domain claims + synthesized claims from every effect draft,
    //     see lib/effect-claims.ts) so GunFx muzzle / LightFx program
    //     channels / sibling landing groups are all visible here. ───
    // NOTE: every pool helper takes `claims` / `ports` / `lights` as explicit
    // arguments rather than reading the stores inside the body.  The callers
    // pass `$effectClaims` / `$deviceModel.ports` / `cfg.lights` in the
    // `{@const}` block, so Svelte SEES those reactive deps in the expression
    // and recomputes the pool when they change — removing a servo/LED (which
    // mutates the draft → $effectClaims) or disabling another effect updates
    // the dropdowns immediately.  (Reading the stores inside the functions made
    // the @const blind to them, so the pool went stale — the "removed LED still
    // shows no ports" bug.)
    function isClaimedByOther(claims: Claim[], p: Port, exceptRefs: PortRefT[]): boolean {
        const cs = claimsForPort(claims, p.ref)
        if (cs.length === 0) return false
        for (const c of cs) {
            const exempt = exceptRefs.some(r =>
                r.guid === c.port.guid && r.idx === c.port.index)
            if (!exempt) return true
        }
        return false
    }

    const refKey = (guid: string, idx: number) => `${guid}#${idx}`

    // Every servo / LED ref already used across ALL landing groups (this group
    // + siblings).  A landing draft's picks aren't synthesized into the
    // `landing` claims with enough granularity to self-exclude, so we exclude
    // them explicitly here — otherwise an already-assigned port lingers in the
    // Add dropdown.
    function usedRefs(lights: LandingLightT[], which: 'servos'|'leds'): Set<string> {
        const out = new Set<string>()
        for (const l of lights) {
            const list = which === 'servos' ? l.servos : l.leds
            for (const e of list) out.add(refKey(e.port.guid, e.port.idx))
        }
        return out
    }

    // Ports with `role` attached, not claimed by ANOTHER effect, and not used
    // by ANY landing group — EXCEPT the editing row's own `keep` pick, which is
    // forced in so its picker stays stable.  `keep = null` ⇒ the "+ Add" pool.
    function availPorts(ports: Port[], claims: Claim[], kindName: 'servo'|'pwm',
                        role: number, used: Set<string>, keep: PortRefT | null): Port[] {
        return ports.filter(p => {
            if (p.kindName !== kindName || p.direction !== 'output') return false
            // The editing row's own pick ("keep") survives BEFORE the roleKind
            // filter, so a momentarily re-roled/detached port (after Apply /
            // refresh) doesn't drop out and blank the <select>.
            if (keep && keep.guid === p.ref.guid && keep.idx === p.ref.index) return true
            if (p.roleKind !== role) return false
            if (isClaimedByOther(claims, p, [])) return false
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
    function phaseClass(p: number | undefined): string {
        switch (p) {
            case 1: return 'phase-retracted'
            case 2: return 'phase-deploying'
            case 3: return 'phase-deployed'
            case 4: return 'phase-retracting'
            default: return 'phase-unknown'
        }
    }
    // One-line wiring summary for the status row.
    function activationLabel(a: LandingLightT['activation']): string {
        if (a.mode === 'input_channel') return a.input ? `RC · ${a.input}` : 'RC channel (unset)'
        if (a.mode === 'program')       return a.program ? `program · ${a.program}` : 'program (unset)'
        return 'manual'
    }

    // Deploy direction: SOLELY the servo profile's ↔ Reversed flag (set in
    // ⚙ Calibrate) — Rule 42 single source, matching gear doors/struts.  The
    // old per-group toggle (swapped openUs/closeUs sentinels) is retired; the
    // fields stay in the DTO only for config round-trip and are ignored by
    // the firmware since 2.45.4.

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
        <h3>Retractable Lights</h3>
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
        {@const phase        = $landingPhases[light.id]}
        {@const deployed     = phase?.phase === 2 || phase?.phase === 3}
        {@const issues       = landingItemErrors(cfg.lights, cfg.lights.findIndex(l => l.id === light.id))}
        {@const hasErrors    = issues.length > 0}
        {@const usedServos   = usedRefs(cfg.lights, 'servos')}
        {@const usedLeds     = usedRefs(cfg.lights, 'leds')}
        {@const servoAddPool = availPorts($deviceModel.ports, $effectClaims, 'servo', RoleKind.ServoActuator, usedServos, null)}
        {@const ledAddPool   = availPorts($deviceModel.ports, $effectClaims, 'pwm',   RoleKind.LedAnimator,   usedLeds,   null)}
        {@const chosenChan   = chanOpts.find(o => o.fnId === light.activation.input)}
        {@const liveUs       = chosenChan ? $liveChannels[liveChannelKey({guid: chosenChan.portGuid, kind: 4, index: chosenChan.portIdx}, chosenChan.channel)] : null}
        <div class="card group-card" class:invalid={hasErrors}>
            <div class="card-header inner">
                <h4>{light.name || 'Landing group'}</h4>
                <div class="header-actions">
                    <!-- Live deploy state sits right next to the manual
                         Deploy/Retract toggle (Rule 48): the pill shows what the
                         firmware reports, the button is the action.  Pill defaults
                         to Retracted before the first LL_PHASE_EVENT. -->
                    <span class="state-pill phase {phaseClass(phase?.phase ?? 1)}">{phase?.name ?? 'Retracted'}</span>
                    <!-- ON→OFF (retract) always enabled — emergency cutoff;
                         OFF→ON (deploy) gated on dirty/errors. -->
                    <button class="small state-toggle" class:danger={deployed}
                            on:click={async () => { (deployed ? await landingDeactivate(light.id) : await landingActivate(light.id)); refreshLandingStatus().catch(() => {}) }}
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

            <!-- One-line wiring summary (the live deploy state is the pill in the
                 card header, next to Deploy/Retract). -->
            <div class="group-summary">
                {light.servos.length} servo{light.servos.length === 1 ? '' : 's'}
                · {light.leds.length} LED{light.leds.length === 1 ? '' : 's'}
                · {light.fadeInMs > 0 ? `${light.fadeInMs} ms fade-in` : 'hard on'}
                · {activationLabel(light.activation)}
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
            <div class="section-head">
                Servos
                <span class="hint">all move together; LEDs come on only once EVERY servo reaches its open position</span>
                <!-- "no free servos" warning is on the + Add row below, not duplicated here. -->
            </div>
            <!-- 2.46.0 explicit-position model: open/close are ABSOLUTE µs
                 (group-wide, all servos get the same targets) set with the
                 endpoints widget.  No direction flags anywhere. -->
            <div class="form-row">
                <span class="field-label">Positions</span>
                <span class="hint compact">{summariseEndpoints('Open', light.openUs, 'Closed', light.closeUs, 0, 0)}</span>
                <button class="small btn-slot" on:click={() => openLandingEndpoints(light)}
                        disabled={busy || light.servos.length === 0}
                        title="Live-jog the servo and capture the OPEN and CLOSED positions as absolute µs — saved to /landing.yaml immediately.">◧ Positions…</button>
            </div>
            {#each light.servos as s, i (i)}
                <div class="form-row">
                    <span class="field-label">Servo #{i + 1}</span>
                    <select class="field-input wide" value={portRefToKey(s.port)}
                            on:change={(e) => setServoPort(light.id, i, parsePortOption(selValue(e), 'servo'))}
                            disabled={busy}>
                        <option value="">— pick a free servo port —</option>
                        {#each availPorts($deviceModel.ports, $effectClaims, 'servo', RoleKind.ServoActuator, usedServos, s.port) as p}
                            <option value={refOptKey(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                    <button class="small danger btn-slot" on:click={() => removeServo(light.id, i)} disabled={busy}>× Remove</button>
                </div>
                {#if s.port && s.port.kind}
                    {@const prof = profileForPort(s.port) ?? ({ minUs: 1000, maxUs: 2000, centerUs: 1500, maxSpeedUsPerSec: 0, maxAccelUsPerSec2: 0, maxJerkUsPerSec3: 0 })}
                    {@const sv   = $servoStatus[servoStatusKey(s.port.guid, s.port.idx)]}
                    <!-- Live servo output bar (same ServoIoWidget GunFx yaw/pitch
                         uses; input side hidden — a landing servo is deploy/retract
                         driven, not RC-axis driven).  Position from the 20 Hz
                         servo_status stream enabled in onMount.  Spans the full
                         card width (no label gutter) so the travel bar is as wide
                         as possible. -->
                    <div class="io-widget-row">
                        <ServoIoWidget
                            hasInput={false}
                            hasServo={true}
                            outputLabel="Servo #{i + 1} output"
                            minUs={prof.minUs} maxUs={prof.maxUs} centerUs={prof.centerUs} openUs={resolveEndpointUs(light.openUs ?? US_CAL_MAX, prof.minUs, prof.maxUs)}
                            servo={sv ?? null} />
                    </div>
                {/if}
                <!-- Per-servo profile widget (Rule 44 + new popup pattern):
                     compact summary + Calibrate button.
                     Profile data flows through the device model (server-
                     authoritative); jog + save go through the popup. -->
                <div class="form-row servo-widget-row">
                    <span class="field-label"></span>
                    <ServoWidget
                        port={s.port}
                        portLabel={labelFor(s.port)}
                        profile={profileForPort(s.port)}
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
            <div class="section-head" class:section-error={light.leds.length === 0}>
                LEDs
                <span class="hint">brightness is per-LED; the firmware uses the FIRST LED's brightness for the group state</span>
                {#if light.leds.length === 0}
                    <span class="section-err-tag">no LEDs — group will not deploy</span>
                {/if}
                <!-- "no free LEDs" warning is on the + Add row below, not duplicated here. -->
            </div>
            {#each light.leds as e, i (i)}
                <div class="form-row">
                    <span class="field-label">LED #{i + 1}</span>
                    <select class="field-input wide" value={portRefToKey(e.port)}
                            on:change={(ev) => setLedPort(light.id, i, parsePortOption(selValue(ev), 'pwm'))}
                            disabled={busy}>
                        <option value="">— pick a free PWM port —</option>
                        {#each availPorts($deviceModel.ports, $effectClaims, 'pwm', RoleKind.LedAnimator, usedLeds, e.port) as p}
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
                <!-- Rule 36: an RC-channel-gated action uses the shared
                     ChannelToggleCluster (live channel bar + threshold mark +
                     hysteresis band + NO-SIGNAL state) — same widget EngineFx
                     uses for its on/off gate. -->
                <ChannelToggleCluster
                    channelLabel="Deploy channel"
                    emptyOption="— pick a named channel —"
                    options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
                    inputId={light.activation.input}
                    thresholdUs={light.activation.thresholdUs}
                    hysteresisUs={light.activation.hysteresisUs}
                    liveUs={liveUs?.us ?? null}
                    liveValid={liveUs?.valid ?? false}
                    busy={busy}
                    actionVerb="Deploys"
                    onChange={(n) => {
                        updateLandingLight(light.id, l => ({
                            ...l,
                            activation: {
                                ...l.activation,
                                input: n.inputId,
                                thresholdUs: n.thresholdUs,
                                hysteresisUs: n.hysteresisUs,
                            },
                        }))
                    }} />
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
    /* Match the 28 px row-button height so the pill aligns with Deploy/Retract
       + Remove in the header-actions row (Rule 34 control-height). */
    .state-pill.phase { height: 28px; display: inline-flex; align-items: center; box-sizing: border-box; font-family: var(--font-mono); font-size: 10px; padding: 0 8px; border-radius: 3px; background: var(--bg-input); border: 1px solid var(--border); text-transform: uppercase; letter-spacing: 0.4px; }
    .state-pill.phase.phase-retracted  { color: var(--text-dim); }
    .state-pill.phase.phase-deploying  { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-deployed   { color: var(--success); border-color: var(--success); background: rgba(100,200,120,0.12); }
    .state-pill.phase.phase-retracting { color: var(--warning); border-color: var(--warning); }
    .state-pill.phase.phase-unknown    { color: var(--text-dim); }
    .group-summary { font-size: 11px; color: var(--text-dim); font-family: var(--font-mono); margin: 2px 0 10px; }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    /* Full-bleed servo output bar — no label gutter so the travel bar is
       as wide as the card (wider than the indented form-row widgets). */
    .io-widget-row { display: block; margin: 2px 0 4px; }
    .io-widget-row :global(.srv-io) { width: 100%; }
    .unit        { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }

    .grp-issues { list-style: none; margin: 6px 0 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
    .grp-issue { font-size: 11px; line-height: 1.35; padding-left: 4px; }
    .grp-issue.err { color: var(--error); }
</style>
