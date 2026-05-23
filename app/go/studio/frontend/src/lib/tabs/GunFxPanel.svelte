<!-- ScaleFX Studio — GunFX panel (Phase 4a of GunFX rollout,
     instructions/22).

     Per-gun cards in a stacked list.  Each card carries trigger / ROF /
     muzzle / recoil / smoke / yaw / pitch sections.  Detailed UI polish
     (multi-band ROF overlay on a live bar — Phase 4b; manual-control
     puppet panel — Phase 4d) lands in follow-up turns; this gives
     operators a working editor for /gunfx.yaml today.

     Cross-references to the design rules:
       - Card + enable toggle      → Rule 34
       - Apply gated on dirty      → Rule 35
       - Channel-setup cluster     → Rule 36 (used inline per gun)
       - Port pickers across boards → Rule 34 (port-rail label)
       - Element scaling lives on the role-attach row, NOT here
         → Rule 42 (so smoke heater/fan show element_mv only as a
           ROLE-set value via the IO tab; this panel exposes the
           intent fields only) -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import {
        gunfxDraft, gunfxConfig, gunfxDirty, gunfxStatus,
        loadGunFxConfig, saveGunFxConfig, refreshGunFxStatus,
        addGun, removeGun, setEnabled, updateGun, addRofItem, removeRofItem,
        gunFire, gunStartFiring, gunStopFiring, gunSmokeArm,
        detectBandOverlaps,
        type GunT, type GunFxConfigT, type RofItemT, type PortRefT,
    } from '../gunfx'
    import ServoProfileEditor from '../components/ServoProfileEditor.svelte'
    import { SetPortProfile } from '../../../wailsjs/go/main/App'
    import {
        deviceModel, type Port, formatPortRail,
        liveChannels, liveChannelKey, usToPct,
        RoleKind, claimsForPort,
    } from '../devicemodel'
    import { pickFile } from '../filepicker'

    // Rule 43 — named-channel options come from /hubfx.yaml's `inputs:`
    // block (same source EnginePanel reads for `toggle.input`).
    // `channelFunctions` is the catalog of well-known channel function
    // IDs (engine_toggle, gun_trigger, gun_rof, …) plus operator-added
    // custom names; `$deviceModel.inputs[*].channels[*].function` is
    // the actual assignment.  Items with `function == 'unassigned'` are
    // hidden from the picker (no name to refer to).
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

    let busy = false
    let error = ''

    let cfg: GunFxConfigT
    const unsub = gunfxDraft.subscribe(c => { cfg = c })

    // Rule 46: panel doesn't register with the dirty-registry — the
    // domain module (gunfx.ts) owns `gunfxConfigSource` (incl. its
    // own `gunfxHasErrors` derived store covering ROF band overlaps)
    // and App.svelte registers it once at startup.
    onMount(() => {
        loadGunFxConfig().catch(e => { error = String(e) })
        refreshGunFxStatus().catch(() => {})
        return () => { unsub() }
    })
    onDestroy(unsub)

    function mark(): void { gunfxDraft.set(cfg) }

    async function onApply() {
        busy = true; error = ''
        try { await saveGunFxConfig() } catch (e) { error = String(e) } finally { busy = false }
    }
    async function onReload() {
        busy = true; error = ''
        try { await loadGunFxConfig() } catch (e) { error = String(e) } finally { busy = false }
    }

    // ── Port pickers ─────────────────────────────────────────────────
    // Filter the device model's ports by required kind + direction.
    function portsOfKind(kindName: 'servo'|'pwm'|'hbridge'|'input', direction: 'output'|'input'): Port[] {
        return $deviceModel.ports.filter(p => p.kindName === kindName && p.direction === direction)
    }
    function portRefKey(r: PortRefT): string {
        return r.guid && r.kind && r.idx !== undefined ? `${r.guid}|${r.kind}|${r.idx}` : ''
    }
    function portToRef(p: Port): PortRefT {
        return { board: '', guid: p.ref.guid, kind: p.kindName, idx: p.ref.index }
    }

    // ── Live status helpers ──────────────────────────────────────────
    function statusFor(id: number) {
        return $gunfxStatus.find(s => s.id === id)
    }

    // ── Per-gun mutators (small wrappers so the template stays clean) ──
    function setGunField<K extends keyof GunT>(id: number, key: K, val: GunT[K]) {
        updateGun(id, g => ({ ...g, [key]: val }))
    }
    function setTriggerField(id: number, key: keyof GunT['trigger'], val: any) {
        updateGun(id, g => ({ ...g, trigger: { ...g.trigger, [key]: val } }))
    }
    function setRofField(id: number, key: keyof GunT['rof'], val: any) {
        updateGun(id, g => ({ ...g, rof: { ...g.rof, [key]: val } }))
    }
    function setRofItem(id: number, i: number, key: keyof RofItemT, val: any) {
        updateGun(id, g => ({
            ...g,
            rof: {
                ...g.rof,
                items: g.rof.items.map((it, idx) => idx === i ? { ...it, [key]: val } : it),
            },
        }))
    }
    // Browse SD card for a per-ROF-item sound file.  Same pattern as
    // EnginePanel sound rows: Rule 34 picker is parametrised by backend
    // (`targets: 'sd'` hides the Flash tab); browse is `…` on the left,
    // Clear is on the right.
    async function browseRofSound(id: number, i: number) {
        const p = await pickFile({ targets: 'sd' })
        if (p != null) setRofItem(id, i, 'soundPath', p)
    }
    function clearRofSound(id: number, i: number) {
        setRofItem(id, i, 'soundPath', '')
    }
    function setMuzzleField(id: number, key: keyof GunT['muzzleFlash'], val: any) {
        updateGun(id, g => ({ ...g, muzzleFlash: { ...g.muzzleFlash, [key]: val } }))
    }
    function setRecoilField(id: number, key: keyof GunT['recoil'], val: any) {
        updateGun(id, g => ({ ...g, recoil: { ...g.recoil, [key]: val } }))
    }
    function setHeaterField(id: number, key: keyof GunT['smoke']['heater'], val: any) {
        updateGun(id, g => ({ ...g, smoke: { ...g.smoke, heater: { ...g.smoke.heater, [key]: val } } }))
    }
    function setFanField(id: number, key: keyof GunT['smoke']['fan'], val: any) {
        updateGun(id, g => ({ ...g, smoke: { ...g.smoke, fan: { ...g.smoke.fan, [key]: val } } }))
    }
    function setAxisField(id: number, which: 'yaw'|'pitch', key: keyof GunT['yaw'], val: any) {
        updateGun(id, g => ({ ...g, [which]: { ...g[which], [key]: val } }))
    }
    type AxisKey = 'yaw'|'pitch'
    function axisOf(g: GunT, which: AxisKey) { return which === 'yaw' ? g.yaw : g.pitch }
    const axisKeys: AxisKey[] = ['yaw', 'pitch']

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    function boolValue(e: Event): boolean { return (e.target as HTMLInputElement).checked }

    // Render a port-ref's option key (matches the value emitted by the
    // dropdown so Svelte's reactive `value` binding lines up).
    function refOptValue(p: Port): string {
        return `${p.ref.guid}|${p.kindName}|${p.ref.index}`
    }
    function refOptLabel(p: Port): string {
        const rail = formatPortRail(p.voltageMv)
        return `${p.boardName ?? 'Hub'} · ${p.hardwareName}${rail ? ` (${rail})` : ''}`
    }
    function parsePortOption(key: string, kindName: PortRefT['kind']): PortRefT {
        if (!key) return { board: '', guid: '', kind: kindName, idx: 0 }
        const [guid, kind, idxStr] = key.split('|')
        return { board: '', guid, kind, idx: Number(idxStr) }
    }
    // Build the dropdown's selected-value key from a stored PortRefT so
    // we don't have to inline `as any` casts inside Svelte attribute
    // expressions (the parser chokes on those).  Returns "" when the
    // port-ref is empty / unset.
    function portRefToKey(r: PortRefT): string {
        if (!r || !r.guid || !r.kind) return ''
        return `${r.guid}|${r.kind}|${r.idx}`
    }

    // Manual / puppet helpers removed (Phase 4 polish 2026-05-23) —
    // the per-gun test row in the card header (Fire / Auto / Stop /
    // smoke toggle) replaces them.  See gunfx.ts for the still-live
    // gunManualSet / gunManualRelease / gunVerboseSubscribe wire
    // wrappers — they stay available for a future debug overlay.

    // Rule 42 storage + Rule 44 editing surface — servo motion profile
    // lives in /hubfx.yaml's ports[] block (canonical store).  The
    // GunFx panel reads it via $deviceModel.ports[i].profile and writes
    // it via SetPortProfile, which updates Studio's overlay, live-pushes
    // ServoSetProfile to the role, and marks the hub config dirty so
    // the next Save persists.  Hub-local + expander ports both supported.
    type ProfileT = { minUs: number; maxUs: number; centerUs: number; reversed: boolean; maxSpeedUsPerSec: number; maxAccelUsPerSec2: number; maxJerkUsPerSec3: number }
    const defaultProfile = (): ProfileT => ({
        minUs: 1000, maxUs: 2000, centerUs: 1500, reversed: false,
        maxSpeedUsPerSec: 800, maxAccelUsPerSec2: 1600, maxJerkUsPerSec3: 0,
    })
    // Look up the profile already attached to a port (from /hubfx.yaml
    // via the device model).  Returns a fresh default when the port has
    // no profile set yet — first edit on this port creates the entry.
    function profileForPort(port: PortRefT): ProfileT {
        if (!port || !port.kind) return defaultProfile()
        const k = `${port.guid}|${port.kind}|${port.idx}`
        const dm = $deviceModel
        for (const p of dm.ports) {
            if (`${p.ref.guid}|${p.kindName}|${p.ref.index}` === k && p.profile) {
                return { ...p.profile }
            }
        }
        return defaultProfile()
    }
    // PortKind byte for the SetPortProfile Wails call.  Only `servo`
    // ever reaches this code path (profile is servo-only), so a single
    // mapping is enough — see app/go/protocol/ports/ports.go for the
    // canonical enum if other kinds ever need profile data.
    const PORT_KIND_SERVO = 1
    const profilePushTimers = new Map<string, ReturnType<typeof setTimeout>>()
    function schedulePushProfile(port: PortRefT, prof: ProfileT) {
        if (!port || port.kind !== 'servo') return
        const key = `${port.guid}|${port.kind}|${port.idx}`
        const existing = profilePushTimers.get(key)
        if (existing) clearTimeout(existing)
        profilePushTimers.set(key, setTimeout(() => {
            profilePushTimers.delete(key)
            // SetPortProfile updates the overlay + live-pushes to the
            // role + emits devicemodel:changed (the panel re-renders
            // with the new profile reflected in $deviceModel).
            SetPortProfile(port.guid, PORT_KIND_SERVO, port.idx, prof)
                .catch(e => { error = String(e) })
        }, 350))
    }

    // ── Live µs lookup from a named channel (Rule 43 + 36) ─────────────
    // For a function id like "gun_fire_mode", find which (input port,
    // channel) carries it AND look up the live value the dispatcher is
    // streaming.  Returns null when the name isn't assigned to any
    // channel yet (operator hasn't configured the IO tab).
    function liveUsFor(fnId: string): { us: number; valid: boolean } | null {
        if (!fnId) return null
        for (const inp of $deviceModel.inputs) {
            for (const c of inp.channels) {
                if (c.function !== fnId) continue
                const lc = $liveChannels[liveChannelKey({ guid: inp.port.guid, kind: 4, index: inp.port.index }, c.channel)]
                if (!lc) return { us: 1500, valid: false }
                return { us: lc.us, valid: lc.valid }
            }
        }
        return null
    }

    // ── Multi-band ROF overlay (Phase 4b, Rule 38) ─────────────────────
    // Each ROF item declares [bandLoUs, bandHiUs]; on the live channel
    // bar the bands render as non-overlapping coloured zones, plus a
    // red-stripe hatch where bands overlap (validation warning).
    // Colour cycle uses 4 palette entries — adequate for the 8-item cap.
    type BandPaint = { lo: number; hi: number; name: string; rpm: number; armed: boolean; color: string }

    function bandPalette(i: number): string {
        const palette = ['#5b9dff', '#ffa05b', '#5bd28b', '#d65bd2']
        return palette[i % palette.length]
    }
    function paintRofBands(items: RofItemT[], liveUs: number | null): BandPaint[] {
        return items.map((it, i) => {
            const lo = it.bandLoUs || 1000
            const hi = it.bandHiUs || 2000
            const armed = liveUs != null && liveUs >= lo && liveUs <= hi
            return { lo, hi, name: it.name || `rof${i + 1}`, rpm: it.rpm, armed, color: bandPalette(i) }
        })
    }
    // ── Optional-section yellow warnings (Phase 4, Rule 39) ───────────
    // When an optional section is engaged (or implicitly required) AND
    // no candidate port is available, surface a soft amber warning
    // BUT don't block Apply (yellow is non-fatal, unlike Rule 35 red).
    //
    // Heuristic: a section has "no free port" when the candidate list
    // is empty AND the operator hasn't already picked a port (an
    // already-claimed port still counts as configured).
    function noFreePortOf(kindName: 'servo'|'pwm', direction: 'output'|'input', currentRef: PortRefT): boolean {
        if (portRefToKey(currentRef)) return false  // already set
        return portsOfKind(kindName, direction).length === 0
    }
    function axisNoFreePort(axis: GunT['yaw']): boolean {
        if (!axis.enabled) return false
        return noFreePortOf('servo', 'output', axis.servoPort)
    }

    // ── Role-filtered port pickers (heater + fan) ────────────────────
    //
    // The smoke heater drives a HEATER element; the smoke fan drives a
    // DC MOTOR.  Per Rule 42 those mechanisms live on the role layer,
    // so the operator first attaches the `Heater` / `DcMotor` role on
    // the port-role row in the IO tab (with element_mv, scaling, …);
    // ONLY then can the gun reference that port here.  This filter
    // also hides ports already claimed by another effect (or by a
    // sibling gun in the same draft) — preventing the two-guns-share-
    // one-heater footgun.
    //
    // The currently-selected port is ALWAYS included so the operator
    // sees their choice (otherwise switching role on the IO tab would
    // make the smoke port silently disappear from the dropdown).
    function otherGunsSmokeRefs(currentGunId: number, which: 'heater'|'fan'): PortRefT[] {
        const out: PortRefT[] = []
        for (const g of cfg.guns) {
            if (g.id === currentGunId) continue
            const r = which === 'heater' ? g.smoke.heater.port : g.smoke.fan.port
            if (portRefToKey(r)) out.push(r)
        }
        return out
    }
    // Sibling guns' muzzle flash ports — used to hide an LED port that's
    // already claimed by another gun (Rule 39 + the same role-filter
    // pattern as the smoke pickers).
    function otherGunsMuzzleRefs(currentGunId: number): PortRefT[] {
        const out: PortRefT[] = []
        for (const g of cfg.guns) {
            if (g.id === currentGunId) continue
            if (portRefToKey(g.muzzleFlash.port)) out.push(g.muzzleFlash.port)
        }
        return out
    }
    function rolePortsFor(roleKind: number, currentRef: PortRefT, otherRefs: PortRefT[]): Port[] {
        const curKey = portRefToKey(currentRef)
        const otherKeys = new Set(otherRefs.map(portRefToKey).filter(k => k))
        return $deviceModel.ports.filter(p => {
            if (p.kindName !== 'pwm' || p.direction !== 'output') return false
            if (p.roleKind !== roleKind) return false
            const k = refOptValue(p)
            // Always surface the operator's current pick.
            if (k === curKey) return true
            // Exclude ports already in use by another gun in this draft.
            if (otherKeys.has(k)) return false
            // Exclude ports claimed by some other effect via the domain
            // system (LandingLight, GearControl, …).  GunFx doesn't
            // claim through that system today, so an entry here means
            // it's a non-gun consumer.
            if (claimsForPort($deviceModel.claims, p.ref).length > 0) return false
            return true
        })
    }
    function noRolePortFor(roleKind: number, currentRef: PortRefT, otherRefs: PortRefT[]): boolean {
        if (portRefToKey(currentRef)) return false  // already set
        return rolePortsFor(roleKind, currentRef, otherRefs).length === 0
    }

    // `detectBandOverlaps` is now imported from gunfx.ts (Rule 46 —
    // domain owns its own validation helpers + DirtySource).
</script>

<div class="card gunfx-card" class:disabled={!cfg?.enabled}>
    <!-- Rule 45 header cluster — structure matches EnginePanel exactly:
         [Enable-Button] [Apply] [dirty-flag] ‖ [+ Add gun] [↻ Refresh].
         Card-header carries only the title; the cluster lives in a
         status-row so the layout is consistent across effect panels. -->
    <div class="card-header">
        <h3>GunFX</h3>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}

    <div class="status-row">
        <div class="status">
            {#if cfg?.enabled}
                <span class="status-label">Guns</span>
                <span class="state-pill">{cfg?.guns?.length ?? 0} configured</span>
            {/if}
        </div>
        <div class="controls">
            <!-- Apply lives in the global ConfigToolbar (top bar);
                 this panel only carries the enable-toggle + Add gun.
                 Refresh is global too.  Per-gun Fire/Auto/Smoke still
                 gate on the local gunfxDirty so an operator can't test
                 against a stale firmware config. -->
            <button class="small state-toggle" class:state-on={cfg?.enabled}
                    on:click={() => setEnabled(!cfg?.enabled)} disabled={busy}
                    title={cfg?.enabled ? 'Disable GunFX — press Apply (top bar) to push.' : 'Enable GunFX — press Apply (top bar) to push.'}>
                {cfg?.enabled ? '✓ Enabled' : '▶ Disabled'}
            </button>
            {#if cfg?.enabled}
                <span class="ctrl-sep" aria-hidden="true"></span>
                <button class="small" on:click={addGun} disabled={busy || (cfg?.guns?.length ?? 0) >= 4} title="Add a gun (max 4)">+ Add gun</button>
            {/if}
        </div>
    </div>
    {#if !cfg?.enabled}
        <div class="empty-state">GunFX is disabled. Toggle on above to author gun configurations.</div>
    {:else if (cfg?.guns?.length ?? 0) === 0}
        <div class="empty-state">No guns configured. Click <b>+ Add gun</b> to start.</div>
    {/if}

    {#if cfg?.enabled}
        {#each cfg.guns as gun (gun.id)}
            {@const st = statusFor(gun.id)}
            <!-- Role-filtered port-picker context — must live at
                 each-block scope because Svelte 3 requires {@const} as
                 a direct child of a control-flow block.  Used by the
                 Muzzle / Smoke sections below to filter dropdowns to
                 ports that ALREADY have the right role attached on
                 the IO tab (Rule 42). -->
            {@const heaterOthers  = otherGunsSmokeRefs(gun.id, 'heater')}
            {@const fanOthers     = otherGunsSmokeRefs(gun.id, 'fan')}
            {@const muzzleOthers  = otherGunsMuzzleRefs(gun.id)}
            {@const heaterMissing = noRolePortFor(RoleKind.Heater,      gun.smoke.heater.port, heaterOthers)}
            {@const fanMissing    = noRolePortFor(RoleKind.DcMotor,     gun.smoke.fan.port,    fanOthers)}
            {@const muzzleMissing = noRolePortFor(RoleKind.LedAnimator, gun.muzzleFlash.port,  muzzleOthers)}
            <div class="card gun-card">
                <div class="card-header inner">
                    <h4>Gun {gun.id}{gun.name ? ` · ${gun.name}` : ''}</h4>
                    <div class="header-actions">
                        {#if st}
                            <span class="state-pill" class:firing={st.firing}>{st.firing ? '▶ firing' : 'idle'}</span>
                            <span class="state-pill" class:smoke={st.smokeArmed}>{st.smokeArmed ? 'smoke armed' : 'smoke off'}</span>
                        {/if}
                        <!-- Rule 35 — operational actions run the firmware's
                             CURRENTLY-LOADED config; if the draft has
                             unapplied edits, pressing Fire here would
                             test the OLD config and look like a bug
                             (e.g. "I changed the sound but Fire plays
                             the old one").  Gate on $gunfxDirty.  Stop
                             stays always-on as an emergency cutoff. -->
                        <button class="small" on:click={() => gunFire(gun.id)}
                                disabled={busy || $gunfxDirty}
                                title={$gunfxDirty ? 'Apply changes before firing — Fire tests the loaded firmware config' : 'Fire one shot now'}>▶ Fire</button>
                        <button class="small" on:click={() => gunStartFiring(gun.id, 0)}
                                disabled={busy || $gunfxDirty}
                                title={$gunfxDirty ? 'Apply changes before auto-fire' : 'Start auto-fire (RPM from ROF selector)'}>▶▶ Auto</button>
                        <button class="small" on:click={() => gunStopFiring(gun.id)} disabled={busy} title="Stop auto-fire (always enabled — emergency cutoff)">■ Stop</button>
                        <button class="small" on:click={() => gunSmokeArm(gun.id, !st?.smokeArmed)}
                                disabled={busy || $gunfxDirty}
                                title={$gunfxDirty ? 'Apply changes before arming smoke' : st?.smokeArmed ? 'Disarm smoke heater' : 'Arm smoke heater'}>{st?.smokeArmed ? 'smoke off' : 'arm smoke'}</button>
                        <button class="small danger" on:click={() => removeGun(gun.id)} disabled={busy} title="Remove this gun">× Remove</button>
                    </div>
                </div>

                <!-- Name -->
                <div class="form-row">
                    <span class="field-label">Name</span>
                    <input class="field-input wide" type="text" maxlength="15"
                           value={gun.name} on:input={(e) => setGunField(gun.id, 'name', inputValue(e))}
                           placeholder="e.g. main / port / bow" disabled={busy} />
                </div>

                <!-- TRIGGER ─ Rule 43: pick from named channels ─────────-->
                <div class="section-head">Trigger (fire on/off)
                    <span class="hint">named channel from the IO tab's <b>inputs[]</b> block</span>
                </div>
                <div class="form-row">
                    <span class="field-label">Channel</span>
                    <select class="field-input wide" value={gun.trigger.input}
                            on:change={(e) => setTriggerField(gun.id, 'input', selValue(e))}
                            disabled={busy}>
                        <option value="">— none (manual only) —</option>
                        {#each chanOpts as o}
                            <option value={o.fnId}>{o.label}</option>
                        {/each}
                    </select>
                </div>
                <div class="form-row">
                    <span class="field-label">Fires when channel ≥</span>
                    <input class="field-input narrow" type="number" min="800" max="2200" step="10"
                           value={gun.trigger.thresholdUs}
                           on:change={(e) => setTriggerField(gun.id, 'thresholdUs', numValue(e))} disabled={busy} />
                    <span class="unit">µs</span>
                    <span class="trigger-pm">±</span>
                    <input class="field-input narrow" type="number" min="0" max="500" step="5"
                           value={gun.trigger.hysteresisUs}
                           on:change={(e) => setTriggerField(gun.id, 'hysteresisUs', numValue(e))} disabled={busy} />
                    <span class="unit">µs hysteresis</span>
                </div>

                <!-- ROF ─ Rule 43: pick from named channels ─────────────-->
                <div class="section-head">Rate of fire
                    <span class="hint">selector channel + multi-band item table</span>
                </div>
                <div class="form-row">
                    <span class="field-label">Selector channel</span>
                    <select class="field-input wide" value={gun.rof.input}
                            on:change={(e) => setRofField(gun.id, 'input', selValue(e))}
                            disabled={busy}>
                        <option value="">— none (use item #0 always) —</option>
                        {#each chanOpts as o}
                            <option value={o.fnId}>{o.label}</option>
                        {/each}
                    </select>
                </div>
                <!-- Rule 38: multi-band overlay on a live channel bar.
                     Renders only when a ROF selector channel is bound
                     AND at least one ROF item exists. -->
                {#if gun.rof.input && gun.rof.items.length > 0}
                    {@const liveSel = liveUsFor(gun.rof.input)}
                    {@const bands = paintRofBands(gun.rof.items, liveSel?.valid ? liveSel.us : null)}
                    {@const overlaps = detectBandOverlaps(gun.rof.items)}
                    <div class="rof-bar" class:nosignal={!liveSel || !liveSel.valid} class:overlap-error={overlaps.length > 0}>
                        {#each bands as b}
                            <div class="rof-band" class:armed={b.armed}
                                 style="left:{usToPct(b.lo)}%; width:{Math.max(0.4, usToPct(b.hi) - usToPct(b.lo))}%; background:color-mix(in srgb, {b.color} 30%, transparent); border-color:color-mix(in srgb, {b.color} 80%, transparent)"
                                 title="{b.name} · {b.lo}–{b.hi} µs · {b.rpm} rpm">
                                {b.name}
                            </div>
                        {/each}
                        {#if liveSel && liveSel.valid}
                            <div class="rof-mark" style="left:{usToPct(liveSel.us)}%"
                                 title="live: {liveSel.us} µs"></div>
                        {/if}
                        {#if !liveSel || !liveSel.valid}
                            <span class="rof-nosignal">{gun.rof.input ? 'NO SIGNAL' : 'no channel bound'}</span>
                        {/if}
                    </div>
                    {#if overlaps.length > 0}
                        <div class="row-err">⚠ ROF bands overlap (items #{overlaps.map(i => i + 1).join(', #')}) — operator stick crossing the overlap will jitter between items</div>
                    {/if}
                {/if}

                {#each gun.rof.items as item, i (i)}
                    {@const isOverlap = gun.rof.items.length > 1 && detectBandOverlaps(gun.rof.items).includes(i)}
                    <div class="rof-item" class:invalid={isOverlap}
                         style="--band-color: {bandPalette(i)}">
                        <!-- Row 1 — index, name, band, RPM, remove -->
                        <div class="rof-item-row">
                            <span class="rof-idx-pill"><span class="rof-swatch"></span>#{i + 1}</span>
                            <label class="rof-field">
                                <span class="rof-label">Name</span>
                                <input class="field-input" type="text" maxlength="15"
                                       placeholder="e.g. burst" value={item.name}
                                       on:input={(e) => setRofItem(gun.id, i, 'name', inputValue(e))}
                                       disabled={busy} />
                            </label>
                            <label class="rof-field rof-band-field">
                                <span class="rof-label">Band µs</span>
                                <div class="rof-band-inputs">
                                    <input class="field-input narrow" type="number" min="0" max="2200" step="10"
                                           value={item.bandLoUs}
                                           on:change={(e) => setRofItem(gun.id, i, 'bandLoUs', numValue(e))}
                                           disabled={busy} title="Band low µs" />
                                    <span class="rof-band-sep">–</span>
                                    <input class="field-input narrow" type="number" min="0" max="2200" step="10"
                                           value={item.bandHiUs}
                                           on:change={(e) => setRofItem(gun.id, i, 'bandHiUs', numValue(e))}
                                           disabled={busy} title="Band high µs" />
                                </div>
                            </label>
                            <label class="rof-field rof-rpm-field">
                                <span class="rof-label">Rate of fire</span>
                                <div class="rof-rpm-inputs">
                                    <input class="field-input narrow" type="number" min="0" max="3000" step="10"
                                           value={item.rpm}
                                           on:change={(e) => setRofItem(gun.id, i, 'rpm', numValue(e))}
                                           disabled={busy} />
                                    <span class="unit">rpm</span>
                                </div>
                            </label>
                            <button class="small danger rof-remove"
                                    on:click={() => removeRofItem(gun.id, i)}
                                    disabled={busy} title="Remove this ROF item">× Remove</button>
                        </div>
                        <!-- Row 2 — sound path with browse / clear (same
                             pattern as EnginePanel sound rows, Rule 34) -->
                        <div class="rof-item-row">
                            <span class="rof-idx-pill placeholder" aria-hidden="true"></span>
                            <label class="rof-field rof-sound-field">
                                <span class="rof-label">Sound (SD)</span>
                                <input class="field-input" type="text"
                                       placeholder="/sounds/gun/burst.wav  (optional)"
                                       value={item.soundPath}
                                       on:input={(e) => setRofItem(gun.id, i, 'soundPath', inputValue(e))}
                                       disabled={busy} />
                            </label>
                            <button class="small btn-slot" on:click={() => browseRofSound(gun.id, i)}
                                    disabled={busy} title="Browse SD card">…</button>
                            <button class="small btn-slot" on:click={() => clearRofSound(gun.id, i)}
                                    disabled={busy || !item.soundPath}
                                    title="Clear — per-shot sample is optional">Clear</button>
                        </div>
                    </div>
                {/each}
                <div class="form-row">
                    <button class="small" on:click={() => addRofItem(gun.id)} disabled={busy || gun.rof.items.length >= 8}>+ Add ROF item</button>
                    <span class="hint">Each item arms when the selector channel falls in its band — bands shown as coloured zones above.</span>
                </div>

                <!-- MUZZLE FLASH — only PWM ports with the LedAnimator
                     role attached (IO tab) AND not claimed by another
                     effect / sibling gun are listed.  Rule 39 yellow
                     warning when no candidate exists.  Previously the
                     panel auto-picked LedAnimator-1 on first enable
                     because the picker was unfiltered. -->
                <div class="section-head" class:section-warn={muzzleMissing}>
                    Muzzle flash
                    {#if muzzleMissing}<span class="section-warn-tag" title="Open the IO tab and attach the LedAnimator role to a free PWM port, then return here.">no PWM port with LedAnimator role attached</span>{/if}
                    <span class="hint">attach LedAnimator role on the IO tab — only role-bound ports show up here</span>
                </div>
                <div class="form-row">
                    <span class="field-label">LED port</span>
                    <select class="field-input wide" value={portRefToKey(gun.muzzleFlash.port)}
                            on:change={(e) => setMuzzleField(gun.id, 'port', parsePortOption(selValue(e), 'pwm'))}
                            disabled={busy}
                            title="Only PWM ports with the LedAnimator role attached are listed (IO tab → attach LedAnimator first).">
                        <option value="">— none —</option>
                        {#each rolePortsFor(RoleKind.LedAnimator, gun.muzzleFlash.port, muzzleOthers) as p}
                            <option value={refOptValue(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                </div>
                <div class="form-row">
                    <span class="field-label">Duration</span>
                    <input class="field-input narrow" type="number" min="1" max="1000" value={gun.muzzleFlash.durationMs}
                           on:change={(e) => setMuzzleField(gun.id, 'durationMs', numValue(e))} disabled={busy} />
                    <span class="unit">ms</span>
                    <span class="field-label">Brightness</span>
                    <input class="field-input narrow" type="number" min="0" max="100" value={gun.muzzleFlash.brightness}
                           on:change={(e) => setMuzzleField(gun.id, 'brightness', numValue(e))} disabled={busy} />
                    <span class="unit">%</span>
                </div>

                <!-- TURRET CONTROL — Phase 4 polish 2026-05-23.
                     Groups yaw + pitch axes (each with its own
                     ServoProfileEditor — Rule 42, motion shape on the
                     role layer) and the Recoil behaviour (no dedicated
                     servo: it kicks the chosen axis on each shot).
                     Either axis can be left disabled; recoil silently
                     no-ops when the chosen axis is disabled. -->
                <div class="section-head">Turret control
                    <span class="hint">yaw + pitch share one servo motion profile per axis (IO tab); recoil kicks the chosen axis on every shot</span>
                </div>
                {#each axisKeys as which (which)}
                    {@const axis = axisOf(gun, which)}
                    {@const axisWarn = axisNoFreePort(axis)}
                    <div class="turret-axis" class:axis-warn={axisWarn}>
                        <div class="axis-head">
                            <label class="enable-toggle inline">
                                <input type="checkbox" checked={axis.enabled}
                                       on:change={(e) => setAxisField(gun.id, which, 'enabled', boolValue(e))} disabled={busy} />
                                <span class="axis-title">{which.charAt(0).toUpperCase() + which.slice(1)}</span>
                            </label>
                            {#if axisWarn}<span class="section-warn-tag">no free servo port</span>{/if}
                        </div>
                        {#if axis.enabled}
                            <div class="form-row">
                                <span class="field-label">Servo port</span>
                                <select class="field-input wide" value={portRefToKey(axis.servoPort)}
                                        on:change={(e) => setAxisField(gun.id, which, 'servoPort', parsePortOption(selValue(e), 'servo'))}
                                        disabled={busy}>
                                    <option value="">— none —</option>
                                    {#each portsOfKind('servo', 'output') as p}
                                        <option value={refOptValue(p)}>{refOptLabel(p)}</option>
                                    {/each}
                                </select>
                            </div>
                            <div class="form-row">
                                <span class="field-label">Channel</span>
                                <select class="field-input wide" value={axis.input}
                                        on:change={(e) => setAxisField(gun.id, which, 'input', selValue(e))}
                                        disabled={busy}>
                                    <option value="">— none (hold at neutral) —</option>
                                    {#each chanOpts as o}
                                        <option value={o.fnId}>{o.label}</option>
                                    {/each}
                                </select>
                                <span class="field-label">Neutral</span>
                                <input class="field-input narrow" type="number" min="500" max="2500" step="10"
                                       value={axis.neutralUs}
                                       on:change={(e) => setAxisField(gun.id, which, 'neutralUs', numValue(e))} disabled={busy} />
                                <span class="unit">µs</span>
                            </div>

                            {#if axis.input}
                                {@const liveAx = liveUsFor(axis.input)}
                                <div class="axis-bar" class:nosignal={!liveAx || !liveAx.valid}>
                                    <div class="axis-neutral" style="left:{usToPct(axis.neutralUs)}%" title="neutral {axis.neutralUs} µs"></div>
                                    {#if liveAx && liveAx.valid}
                                        <div class="axis-fill" style="width:{usToPct(liveAx.us)}%"></div>
                                        <span class="axis-readout">{liveAx.us} µs</span>
                                    {:else}
                                        <span class="axis-nosignal">NO SIGNAL</span>
                                    {/if}
                                </div>
                            {/if}

                            <!-- Rule 42 storage + Rule 44 editing
                                 surface: the profile data lives in
                                 /hubfx.yaml's ports[] block (canonical),
                                 but the editor renders here next to
                                 the axis binding.  `profileForPort`
                                 looks up the current profile from the
                                 device-model overlay; `on:change` calls
                                 `SetPortProfile` which updates the
                                 overlay, live-pushes via
                                 `ServoSetProfile`, and marks the hub
                                 config dirty so the next Save persists
                                 to /hubfx.yaml. -->
                            <ServoProfileEditor
                                profile={profileForPort(axis.servoPort)}
                                label="{which.charAt(0).toUpperCase() + which.slice(1)} motion profile"
                                on:change={(e) => schedulePushProfile(axis.servoPort, e.detail)} />
                        {/if}
                    </div>
                {/each}

                <!-- Recoil sub-section — behaviour layered on top of
                     yaw/pitch.  No dedicated servo port (Phase 4
                     polish): jerk + hold + which axis to kick. -->
                <div class="turret-recoil">
                    <div class="recoil-head">
                        <label class="enable-toggle inline">
                            <input type="checkbox" checked={gun.recoil.enabled}
                                   on:change={(e) => setRecoilField(gun.id, 'enabled', boolValue(e))} disabled={busy} />
                            <span class="axis-title">Recoil</span>
                        </label>
                        <span class="hint">applied on each shot — kicks the chosen turret axis by Jerk for Hold ms, then returns</span>
                    </div>
                    {#if gun.recoil.enabled}
                        <div class="form-row">
                            <span class="field-label">Axis</span>
                            <select class="field-input narrow" value={gun.recoil.axis}
                                    on:change={(e) => setRecoilField(gun.id, 'axis', selValue(e))}
                                    disabled={busy}
                                    title="Which turret axis takes the recoil kick on each shot.">
                                <option value="pitch">pitch</option>
                                <option value="yaw">yaw</option>
                            </select>
                            <span class="field-label">Jerk</span>
                            <input class="field-input narrow" type="number" min="0" max="500" step="10"
                                   value={gun.recoil.jerkUs}
                                   on:change={(e) => setRecoilField(gun.id, 'jerkUs', numValue(e))} disabled={busy} />
                            <span class="unit">µs</span>
                            <span class="field-label">Hold</span>
                            <input class="field-input narrow" type="number" min="0" max="1000" step="10"
                                   value={gun.recoil.holdMs}
                                   on:change={(e) => setRecoilField(gun.id, 'holdMs', numValue(e))} disabled={busy} />
                            <span class="unit">ms</span>
                        </div>
                    {/if}
                </div>

                <!-- SMOKE: HEATER + FAN.  Rule 42 — element_mv + scaling
                     live on the role; the gun only references a PWM
                     port that ALREADY HAS the right role attached
                     (Heater for heater, DcMotor for fan).  The pickers
                     filter strictly to those role-attached ports so the
                     operator can't accidentally drive a bare PWM pin as
                     a heater.  Yellow warning (Rule 39) fires when no
                     suitable port exists AND the operator hasn't
                     already picked one.
                     heaterOthers / fanOthers / heaterMissing / fanMissing
                     are declared at the {#each} top — Svelte 3 won't
                     accept {@const} mid-block. -->
                <div class="section-head" class:section-warn={heaterMissing || fanMissing}>
                    Smoke
                    {#if heaterMissing || fanMissing}
                        <span class="section-warn-tag" title="Open the IO tab and attach the missing role(s) to a free PWM port — then return here to bind them.">
                            no PWM port with {
                                heaterMissing && fanMissing ? 'Heater + DcMotor'
                              : heaterMissing ? 'Heater'
                              : 'DcMotor'
                            } role attached
                        </span>
                    {/if}
                    <span class="hint">attach Heater / DcMotor role on the IO tab — element_mv + scaling live there (Rule 42)</span>
                </div>
                <div class="smoke-grid">
                    <div class="smoke-col">
                        <div class="col-head">Heater</div>
                        <div class="form-row">
                            <span class="field-label">Port</span>
                            <select class="field-input" style="flex:1" value={portRefToKey(gun.smoke.heater.port)}
                                    on:change={(e) => setHeaterField(gun.id, 'port', parsePortOption(selValue(e), 'pwm'))}
                                    disabled={busy}
                                    title="Only PWM ports with the Heater role attached are listed (IO tab → attach Heater first).">
                                <option value="">— none —</option>
                                {#each rolePortsFor(RoleKind.Heater, gun.smoke.heater.port, heaterOthers) as p}
                                    <option value={refOptValue(p)}>{refOptLabel(p)}</option>
                                {/each}
                            </select>
                        </div>
                        <div class="form-row">
                            <span class="field-label">Mode</span>
                            <select class="field-input" style="flex:1" value={gun.smoke.heater.mode}
                                    on:change={(e) => setHeaterField(gun.id, 'mode', selValue(e))} disabled={busy}>
                                <option value="always_on">always_on</option>
                                <option value="bang_bang">bang_bang</option>
                                <option value="closed_loop">closed_loop</option>
                            </select>
                        </div>
                        <div class="form-row">
                            <span class="field-label">Target</span>
                            <input class="field-input narrow" type="number" value={gun.smoke.heater.targetCx10}
                                   on:change={(e) => setHeaterField(gun.id, 'targetCx10', numValue(e))} disabled={busy} />
                            <span class="unit">cx10 ({(gun.smoke.heater.targetCx10/10).toFixed(1)} °C)</span>
                        </div>
                    </div>

                    <div class="smoke-col">
                        <div class="col-head">Fan</div>
                        <div class="form-row">
                            <span class="field-label">Port</span>
                            <select class="field-input" style="flex:1" value={portRefToKey(gun.smoke.fan.port)}
                                    on:change={(e) => setFanField(gun.id, 'port', parsePortOption(selValue(e), 'pwm'))}
                                    disabled={busy}
                                    title="Only PWM ports with the DcMotor role attached are listed (IO tab → attach DcMotor first).">
                                <option value="">— none —</option>
                                {#each rolePortsFor(RoleKind.DcMotor, gun.smoke.fan.port, fanOthers) as p}
                                    <option value={refOptValue(p)}>{refOptLabel(p)}</option>
                                {/each}
                            </select>
                        </div>
                        <div class="form-row">
                            <span class="field-label">Mode</span>
                            <select class="field-input" style="flex:1" value={gun.smoke.fan.mode}
                                    on:change={(e) => setFanField(gun.id, 'mode', selValue(e))} disabled={busy}>
                                <option value="off">off</option>
                                <option value="continuous">continuous</option>
                                <option value="puff_per_shot">puff_per_shot</option>
                                <option value="puff_on_fire_active">puff_on_fire_active</option>
                            </select>
                        </div>
                        <div class="form-row">
                            <span class="field-label">Puff width</span>
                            <input class="field-input narrow" type="number" min="20" max="2000" value={gun.smoke.fan.puffMs}
                                   on:change={(e) => setFanField(gun.id, 'puffMs', numValue(e))} disabled={busy} />
                            <span class="unit">ms</span>
                        </div>
                    </div>
                </div>

                <!-- Yaw / pitch are now inside the Turret control section above. -->

                <!-- Manual / puppet panel removed (Phase 4 polish 2026-05-23):
                     redundant with the per-gun test row in the card
                     header (▶ Fire / ▶▶ Auto / ■ Stop / smoke toggle).
                     If a live verbose-status mirror is ever needed
                     again it should be its own debug overlay, not part
                     of the configuration flow. -->
            </div>
        {/each}
    {/if}

</div>  <!-- /.card.gunfx-card -->


<style>
    .banner.err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); padding: 7px 10px; border-radius: 4px; margin: 6px 0; font-size: 12px; }
    .enable-toggle { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--text-dim); cursor: pointer; }
    .enable-toggle.inline { display: inline-flex; }
    .enable-toggle input { accent-color: var(--accent); }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .header-actions button { height: 28px; box-sizing: border-box; }

    .gun-card { margin-bottom: 14px; }
    .card-header.inner { padding: 4px 0 8px; border-bottom: 1px dashed var(--border); margin-bottom: 8px; }
    .card-header.inner h4 { font-size: 13px; font-weight: 600; color: var(--text-bright); }

    .state-pill { font-family: var(--font-mono); font-size: 10px; padding: 2px 8px; border-radius: 3px; background: var(--bg-input); color: var(--text-dim); border: 1px solid var(--border); text-transform: uppercase; letter-spacing: 0.4px; }
    .state-pill.firing { background: rgba(255,80,80,0.22); color: #ff8a85; border-color: rgba(255,80,80,0.5); }
    .state-pill.smoke { background: rgba(255,180,0,0.18); color: var(--warning); border-color: rgba(255,180,0,0.5); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    .section-head .hint { font-size: 9px; font-weight: 400; text-transform: none; letter-spacing: 0; color: var(--text-dim); font-style: italic; }
    /* Rule 39 — yellow warning on an optional section that's enabled
       but has no candidate port available.  Non-fatal: doesn't gate
       Apply, but flags the issue to the operator. */
    .section-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
    .section-warn-tag { font-size: 9px; font-weight: 700; color: var(--warning); padding: 1px 6px; border: 1px solid var(--warning); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .unit { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
    .trigger-pm { margin: 0 4px; color: var(--text-dim); font-family: var(--font-mono); font-weight: 700; }

    /* ROF item — two-row card per item, with a coloured side-stripe
       matching the band palette so the operator can visually pair a
       row with its zone on the overlay bar above. */
    .rof-item { display: flex; flex-direction: column; gap: 4px; padding: 8px 10px; margin: 6px 0; background: var(--bg-raised); border: 1px solid var(--border); border-left: 3px solid var(--band-color, var(--accent)); border-radius: 4px; }
    .rof-item.invalid { border-color: var(--error); border-left-color: var(--error); background: rgba(255,80,80,0.05); }
    .rof-item-row { display: flex; align-items: flex-end; gap: 8px; }

    /* Index pill — sits at the left edge with a colour swatch matching
       the band so the row+zone correspondence is unmistakable.  Row 2
       gets a placeholder pill of the same width to keep the labels
       vertically aligned across both rows. */
    .rof-idx-pill { display: inline-flex; align-items: center; gap: 4px; font-family: var(--font-mono); font-size: 11px; color: var(--text); font-weight: 600; padding: 4px 6px; min-width: 36px; box-sizing: border-box; flex-shrink: 0; }
    .rof-idx-pill.placeholder { visibility: hidden; }
    .rof-swatch { display: inline-block; width: 8px; height: 8px; border-radius: 2px; background: var(--band-color, var(--accent)); }

    /* Field-label + control stacked vertically — the canonical "labeled
       field" widget reusing the design-system .field-input height + the
       same 9px uppercase label as the section heads. */
    .rof-field { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
    .rof-label { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); }
    .rof-field > .field-input { width: 100%; box-sizing: border-box; }

    .rof-band-field   { flex: 0 0 160px; }
    .rof-rpm-field    { flex: 0 0 120px; }
    .rof-sound-field  { flex: 1; }
    .rof-field:not(.rof-band-field):not(.rof-rpm-field):not(.rof-sound-field) { flex: 1; min-width: 120px; }

    .rof-band-inputs { display: flex; align-items: center; gap: 4px; }
    .rof-band-inputs .field-input.narrow { width: 70px; min-width: 0; }
    .rof-band-sep { font-family: var(--font-mono); color: var(--text-dim); }

    .rof-rpm-inputs { display: flex; align-items: center; gap: 4px; }
    .rof-rpm-inputs .field-input.narrow { width: 70px; min-width: 0; }

    .rof-remove { flex-shrink: 0; }

    /* Multi-band ROF overlay (Rule 38) — non-overlapping coloured zones
       on a live channel bar; live µs draws as a vertical marker line.
       Overlap detection paints a red diagonal hatch on the whole bar so
       the operator sees it from across the room. */
    .rof-bar { position: relative; height: 22px; margin: 6px 0 2px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; }
    .rof-bar.nosignal { background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 6px, transparent 6px, transparent 12px); }
    .rof-bar.overlap-error { box-shadow: inset 0 0 0 2px var(--error); }
    .rof-bar.overlap-error::after { content: ''; position: absolute; inset: 0; background: repeating-linear-gradient(45deg, transparent, transparent 6px, rgba(255,80,80,0.18) 6px, rgba(255,80,80,0.18) 12px); pointer-events: none; }
    .rof-band { position: absolute; top: 1px; bottom: 1px; border-left: 1px solid; border-right: 1px solid; display: flex; align-items: center; justify-content: center; font-size: 9px; font-family: var(--font-mono); color: var(--text-bright); text-shadow: 0 0 3px rgba(0,0,0,0.7); white-space: nowrap; overflow: hidden; pointer-events: auto; cursor: help; }
    .rof-band.armed { box-shadow: inset 0 0 0 2px var(--accent); }
    .rof-mark { position: absolute; top: -3px; bottom: -3px; width: 2px; background: var(--success); box-shadow: 0 0 4px rgba(100,255,150,0.7); transform: translateX(-1px); pointer-events: none; }
    .rof-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-family: var(--font-mono); font-size: 9px; letter-spacing: 0.5px; color: var(--text-dim); }
    .row-err { font-size: 11px; color: var(--error); margin: 3px 0 0 28px; font-family: var(--font-mono); }

    /* Axis bar (Phase 4c) — live µs on a 1000..2000 µs scale.  Single
       fill + a neutral marker; the configured min/max range overlay is
       implicit (any port-role profile clamping is on the role side). */
    .axis-bar { position: relative; height: 14px; margin: 4px 0 6px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; }
    .axis-bar.nosignal { background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 6px, transparent 6px, transparent 12px); }
    .axis-fill { height: 100%; background: linear-gradient(90deg, var(--accent), var(--success)); transition: width 0.08s linear; }
    .axis-neutral { position: absolute; top: -2px; bottom: -2px; width: 1px; background: var(--text-dim); pointer-events: none; }
    .axis-readout { position: absolute; right: 6px; top: 0; line-height: 14px; font-family: var(--font-mono); font-size: 9px; color: var(--text-bright); text-shadow: 0 0 3px rgba(0,0,0,0.7); }
    .axis-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-family: var(--font-mono); font-size: 9px; letter-spacing: 0.5px; color: var(--text-dim); }

    .smoke-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .smoke-col { padding: 6px 10px; background: var(--bg-raised); border-radius: 4px; }
    .col-head { font-size: 10px; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 4px; }

    .dirty-flag { font-size: 11px; color: var(--text-dim); margin-left: 4px; }
    .dirty-flag.on { color: var(--warning); font-weight: 600; }

    /* ── Manual control / puppet mode (Phase 4d, Rule 41) ─────────── */
    .puppet-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 4px; }
    .puppet-col { padding: 8px 10px; background: var(--bg-raised); border-radius: 4px; border: 1px solid var(--border); }
    .puppet-col.mirror { background: color-mix(in srgb, var(--accent) 6%, var(--bg-raised)); }
    .col-head { font-size: 10px; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 6px; display: flex; align-items: baseline; gap: 6px; }
    .col-head .rate { font-size: 9px; font-style: italic; color: var(--text-dim); }

    .slider { flex: 1; height: 6px; -webkit-appearance: none; appearance: none; background: var(--bg-input); border-radius: 3px; outline: none; }
    .slider::-webkit-slider-thumb { -webkit-appearance: none; width: 14px; height: 14px; background: var(--accent); border-radius: 50%; cursor: pointer; }
    .slider::-moz-range-thumb { width: 14px; height: 14px; background: var(--accent); border-radius: 50%; cursor: pointer; border: none; }
    .mono { font-family: var(--font-mono); }
    .live-trail { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); }

    .fire-btn { width: 100%; height: 38px; background: var(--bg-input); color: var(--text-bright); border: 2px solid var(--border); border-radius: 5px; font-weight: 700; font-size: 12px; cursor: pointer; user-select: none; transition: background 0.05s, border-color 0.05s; }
    .fire-btn:hover { background: color-mix(in srgb, var(--error) 18%, var(--bg-input)); }
    .fire-btn.held { background: rgba(255,80,80,0.6); border-color: var(--error); color: #fff; box-shadow: 0 0 12px rgba(255,80,80,0.4); }
    .fire-btn:disabled { opacity: 0.5; cursor: not-allowed; }

    /* Mirror panel — read-only typed-data rows.  Uses the same colour
       semantics as the channel-setup cluster legend (Rule 36):
       green-on for active state, dim for off, mono for numbers. */
    .mirror-row { display: flex; align-items: center; gap: 6px; padding: 2px 0; flex-wrap: wrap; font-size: 11px; }
    .mlabel { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); min-width: 50px; }
    .mval { font-family: var(--font-mono); font-size: 11px; color: var(--text); }
    .mval.m-on { color: var(--success); font-weight: 700; }
    .mval.m-manual { color: var(--warning); font-weight: 700; }
    .mval.m-dim { color: var(--text-dim); font-style: italic; }
    .mirror-empty { padding: 12px; text-align: center; font-style: italic; }

    /* Turret control — grouped yaw / pitch / recoil block.  Frame
       only (matches the legacy ScaleFX pattern in _archive/): a thin
       border around each axis-box, NO background tint that competes
       with the card's own surface.  Rule 39 yellow warning surfaces
       on the border colour (still no fill). */
    .turret-axis,
    .turret-recoil {
        border: 1px solid var(--border);
        border-radius: 4px;
        padding: 8px 10px;
        margin: 6px 0;
    }
    .turret-axis.axis-warn {
        border-color: color-mix(in srgb, var(--warning) 70%, var(--border));
    }
    .axis-head,
    .recoil-head {
        display: flex; align-items: baseline; gap: 8px;
        margin-bottom: 6px;
        padding-bottom: 4px;
        border-bottom: 1px dashed color-mix(in srgb, var(--border) 50%, transparent);
    }
    .axis-title {
        font-size: 11px; font-weight: 700;
        color: var(--text-bright);
        text-transform: uppercase; letter-spacing: 0.4px;
    }
    .axis-rule42-note {
        margin: 6px 0 0; padding: 6px 8px;
        font-size: 10px; font-style: italic;
        color: var(--text-dim);
        background: color-mix(in srgb, var(--accent) 5%, transparent);
        border-left: 2px solid color-mix(in srgb, var(--accent) 40%, transparent);
        border-radius: 0 3px 3px 0;
        line-height: 1.4;
    }
</style>
