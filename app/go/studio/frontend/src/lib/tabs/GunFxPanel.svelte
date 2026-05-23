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
        gunfxDraft, gunfxConfig, gunfxDirty, gunfxStatus, gunfxVerbose,
        loadGunFxConfig, saveGunFxConfig, refreshGunFxStatus,
        addGun, removeGun, setEnabled, updateGun, addRofItem, removeRofItem,
        gunFire, gunStartFiring, gunStopFiring, gunSmokeArm,
        gunManualSet, gunManualRelease, gunVerboseSubscribe,
        installVerboseListener, uninstallVerboseListener,
        ManualFlag, type GunManualStateT,
        type GunT, type GunFxConfigT, type RofItemT, type PortRefT,
    } from '../gunfx'
    import {
        deviceModel, type Port, formatPortRail,
        liveChannels, liveChannelKey, usToPct,
    } from '../devicemodel'

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

    onMount(() => {
        loadGunFxConfig().catch(e => { error = String(e) })
        refreshGunFxStatus().catch(() => {})
        installVerboseListener()
        return () => { unsub(); uninstallVerboseListener() }
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

    // ── Manual control / puppet mode (Phase 4d, Rule 41) ─────────────
    // Per-gun map: id → "is manual override engaged in the UI?". Toggles
    // independently of the firmware's `_manual.active` (which also
    // auto-releases after 5 s without a SET).  When operator flips
    // the switch on, we subscribe to verbose-status broadcasts; when
    // off, we send MANUAL_RELEASE + unsubscribe.
    const manualOn = new Map<number, boolean>()
    let manualOnTick = 0   // bump to force Svelte to re-eval the verbose readout

    function isManualOn(id: number): boolean { return manualOn.get(id) === true }

    async function toggleManual(id: number, on: boolean) {
        busy = true; error = ''
        try {
            manualOn.set(id, on)
            manualOnTick++
            await gunVerboseSubscribe(id, on)
            if (!on) {
                await gunManualRelease(id)
            }
        } catch (e) { error = String(e) } finally { busy = false }
    }

    // Bulk push helper — assembles a `ManualState` from the args and
    // sends it.  `flagSet` is the bitmask of which fields are being
    // pushed in THIS call (firmware leaves others at their prior manual
    // value or RC-driven).
    async function pushManual(id: number, flagSet: number, partial: Partial<GunManualStateT>) {
        if (!isManualOn(id)) return
        try {
            const v = $gunfxVerbose[id]
            const state: GunManualStateT = {
                flags: flagSet,
                yawUs: partial.yawUs ?? v?.yawTargetUs ?? 1500,
                pitchUs: partial.pitchUs ?? v?.pitchTargetUs ?? 1500,
                rofIndex: partial.rofIndex ?? v?.rofIndex ?? 0xFF,
                fireHold: partial.fireHold ?? 0,
                smokeArm: partial.smokeArm ?? (v?.smokeArmed ? 1 : 0),
                smokeFanBurst: partial.smokeFanBurst ?? 0,
            }
            await gunManualSet(id, state)
        } catch (e) { error = String(e) }
    }

    // Debounced yaw / pitch slider push — operator drags fast, we send
    // at ~30 Hz max.  Per-gun timer so two guns don't share state.
    const slewTimers = new Map<string, ReturnType<typeof setTimeout>>()
    function scheduleAxisPush(id: number, axis: 'yaw'|'pitch', us: number) {
        const key = `${id}|${axis}`
        const existing = slewTimers.get(key)
        if (existing) clearTimeout(existing)
        slewTimers.set(key, setTimeout(() => {
            slewTimers.delete(key)
            pushManual(id, axis === 'yaw' ? ManualFlag.Yaw : ManualFlag.Pitch,
                       axis === 'yaw' ? { yawUs: us } : { pitchUs: us })
        }, 32))
    }

    // Fire-hold button: mouse-down → send fire=1; mouse-up → fire=0.
    function fireHoldDown(id: number) { pushManual(id, ManualFlag.Fire, { fireHold: 1 }) }
    function fireHoldUp(id: number)   { pushManual(id, ManualFlag.Fire, { fireHold: 0 }) }

    function toggleManualSmoke(id: number) {
        const v = $gunfxVerbose[id]
        pushManual(id, ManualFlag.Smoke, { smokeArm: v?.smokeArmed ? 0 : 1 })
    }
    function manualFanBurst(id: number) {
        pushManual(id, ManualFlag.FanBurst, { smokeFanBurst: 1 })
    }
    function setManualRof(id: number, rofIdx: number) {
        pushManual(id, ManualFlag.Rof, { rofIndex: rofIdx })
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

    function detectBandOverlaps(items: RofItemT[]): number[] {
        // Returns ROF-item indices that overlap with at least one other.
        const out: number[] = []
        for (let i = 0; i < items.length; i++) {
            const a = items[i]
            for (let j = i + 1; j < items.length; j++) {
                const b = items[j]
                const al = a.bandLoUs || 1000, ah = a.bandHiUs || 2000
                const bl = b.bandLoUs || 1000, bh = b.bandHiUs || 2000
                if (al <= bh && bl <= ah) {
                    if (!out.includes(i)) out.push(i)
                    if (!out.includes(j)) out.push(j)
                }
            }
        }
        return out
    }
</script>

<div class="tab-content">
    <div class="card-header">
        <h3>GunFX</h3>
        <div class="header-actions">
            <label class="enable-toggle">
                <input type="checkbox" checked={cfg?.enabled ?? false}
                       on:change={(e) => setEnabled(boolValue(e))} disabled={busy} />
                <span>{cfg?.enabled ? 'Enabled' : 'Disabled'}</span>
            </label>
            <button class="small" on:click={addGun} disabled={busy || (cfg?.guns?.length ?? 0) >= 4} title="Add a gun (max 4)">+ Add gun</button>
            <button class="small" on:click={onReload} disabled={busy} title="Reload from /gunfx.yaml">↻ Refresh</button>
            <button class="small primary" on:click={onApply}
                    disabled={busy || !$gunfxDirty}
                    title={$gunfxDirty ? 'Save + reload /gunfx.yaml' : 'No changes to apply'}>✓ Apply</button>
            <span class="dirty-flag" class:on={$gunfxDirty}>
                {$gunfxDirty ? 'unapplied changes' : 'in sync'}
            </span>
        </div>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}
    {#if !cfg?.enabled}
        <div class="empty-state">GunFX is disabled. Toggle on above to author gun configurations.</div>
    {:else if (cfg?.guns?.length ?? 0) === 0}
        <div class="empty-state">No guns configured. Click <b>+ Add gun</b> to start.</div>
    {/if}

    {#if cfg?.enabled}
        {#each cfg.guns as gun (gun.id)}
            {@const st = statusFor(gun.id)}
            <div class="card gun-card">
                <div class="card-header inner">
                    <h4>Gun {gun.id}{gun.name ? ` · ${gun.name}` : ''}</h4>
                    <div class="header-actions">
                        {#if st}
                            <span class="state-pill" class:firing={st.firing}>{st.firing ? '▶ firing' : 'idle'}</span>
                            <span class="state-pill" class:smoke={st.smokeArmed}>{st.smokeArmed ? 'smoke armed' : 'smoke off'}</span>
                        {/if}
                        <button class="small" on:click={() => gunFire(gun.id)} disabled={busy} title="Fire one shot now">▶ Fire</button>
                        <button class="small" on:click={() => gunStartFiring(gun.id, 0)} disabled={busy} title="Start auto-fire">▶▶ Auto</button>
                        <button class="small" on:click={() => gunStopFiring(gun.id)} disabled={busy} title="Stop auto-fire">■ Stop</button>
                        <button class="small" on:click={() => gunSmokeArm(gun.id, !st?.smokeArmed)} disabled={busy}>{st?.smokeArmed ? 'smoke off' : 'arm smoke'}</button>
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
                    <div class="rof-row" class:invalid={isOverlap}>
                        <span class="rof-idx">#{i + 1}</span>
                        <input class="field-input" type="text" placeholder="name" style="width:90px"
                               value={item.name}
                               on:input={(e) => setRofItem(gun.id, i, 'name', inputValue(e))} disabled={busy} />
                        <span class="unit">band</span>
                        <input class="field-input narrow" type="number" min="0" max="2200" step="10"
                               value={item.bandLoUs}
                               on:change={(e) => setRofItem(gun.id, i, 'bandLoUs', numValue(e))} disabled={busy} />
                        <span class="unit">–</span>
                        <input class="field-input narrow" type="number" min="0" max="2200" step="10"
                               value={item.bandHiUs}
                               on:change={(e) => setRofItem(gun.id, i, 'bandHiUs', numValue(e))} disabled={busy} />
                        <span class="unit">µs</span>
                        <input class="field-input narrow" type="number" min="0" max="3000" step="10"
                               value={item.rpm}
                               on:change={(e) => setRofItem(gun.id, i, 'rpm', numValue(e))} disabled={busy} />
                        <span class="unit">rpm</span>
                        <input class="field-input" type="text" placeholder="/sounds/...wav" style="flex:1"
                               value={item.soundPath}
                               on:input={(e) => setRofItem(gun.id, i, 'soundPath', inputValue(e))} disabled={busy} />
                        <button class="small danger" on:click={() => removeRofItem(gun.id, i)} disabled={busy}>×</button>
                    </div>
                {/each}
                <div class="form-row">
                    <button class="small" on:click={() => addRofItem(gun.id)} disabled={busy || gun.rof.items.length >= 8}>+ Add ROF item</button>
                    <span class="hint">Phase 4b: bands rendered as non-overlapping coloured zones on a live input bar.</span>
                </div>

                <!-- MUZZLE FLASH -->
                <div class="section-head" class:section-warn={noFreePortOf('pwm', 'output', gun.muzzleFlash.port)}>
                    Muzzle flash
                    {#if noFreePortOf('pwm', 'output', gun.muzzleFlash.port)}<span class="section-warn-tag">no free PWM port</span>{/if}
                </div>
                <div class="form-row">
                    <span class="field-label">LED port</span>
                    <select class="field-input wide" value={portRefToKey(gun.muzzleFlash.port)}
                            on:change={(e) => setMuzzleField(gun.id, 'port', parsePortOption(selValue(e), 'pwm'))}
                            disabled={busy}>
                        <option value="">— none —</option>
                        {#each portsOfKind('pwm', 'output') as p}
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

                <!-- RECOIL -->
                <div class="section-head" class:section-warn={noFreePortOf('servo', 'output', gun.recoil.port)}>
                    Recoil servo
                    {#if noFreePortOf('servo', 'output', gun.recoil.port)}<span class="section-warn-tag">no free servo port</span>{/if}
                    <span class="hint">motion shape (speed/accel/jerk) lives on the port-role row in the IO tab — Rule 42</span>
                </div>
                <div class="form-row">
                    <span class="field-label">Servo port</span>
                    <select class="field-input wide" value={portRefToKey(gun.recoil.port)}
                            on:change={(e) => setRecoilField(gun.id, 'port', parsePortOption(selValue(e), 'servo'))}
                            disabled={busy}>
                        <option value="">— none —</option>
                        {#each portsOfKind('servo', 'output') as p}
                            <option value={refOptValue(p)}>{refOptLabel(p)}</option>
                        {/each}
                    </select>
                </div>
                <div class="form-row">
                    <span class="field-label">Jerk</span>
                    <input class="field-input narrow" type="number" min="0" max="500" value={gun.recoil.jerkUs}
                           on:change={(e) => setRecoilField(gun.id, 'jerkUs', numValue(e))} disabled={busy} />
                    <span class="unit">µs</span>
                    <span class="field-label">Hold</span>
                    <input class="field-input narrow" type="number" min="0" max="1000" value={gun.recoil.holdMs}
                           on:change={(e) => setRecoilField(gun.id, 'holdMs', numValue(e))} disabled={busy} />
                    <span class="unit">ms</span>
                </div>

                <!-- SMOKE: HEATER + FAN (intent layer only; element_mv lives on the role) -->
                <div class="section-head" class:section-warn={noFreePortOf('pwm', 'output', gun.smoke.heater.port) || noFreePortOf('pwm', 'output', gun.smoke.fan.port)}>
                    Smoke
                    {#if noFreePortOf('pwm', 'output', gun.smoke.heater.port) || noFreePortOf('pwm', 'output', gun.smoke.fan.port)}
                        <span class="section-warn-tag">no free PWM port for {
                            noFreePortOf('pwm', 'output', gun.smoke.heater.port) && noFreePortOf('pwm', 'output', gun.smoke.fan.port) ? 'heater + fan'
                          : noFreePortOf('pwm', 'output', gun.smoke.heater.port) ? 'heater'
                          : 'fan'
                        }</span>
                    {/if}
                    <span class="hint">element_mv + scaling configured on the role-attach row (IO tab)</span>
                </div>
                <div class="smoke-grid">
                    <div class="smoke-col">
                        <div class="col-head">Heater</div>
                        <div class="form-row">
                            <span class="field-label">Port</span>
                            <select class="field-input" style="flex:1" value={portRefToKey(gun.smoke.heater.port)}
                                    on:change={(e) => setHeaterField(gun.id, 'port', parsePortOption(selValue(e), 'pwm'))}
                                    disabled={busy}>
                                <option value="">— none —</option>
                                {#each portsOfKind('pwm', 'output') as p}
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
                                    disabled={busy}>
                                <option value="">— none —</option>
                                {#each portsOfKind('pwm', 'output') as p}
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

                <!-- YAW / PITCH -->
                {#each axisKeys as which (which)}
                    {@const axis = axisOf(gun, which)}
                    {@const axisWarn = axisNoFreePort(axis)}
                    <div class="section-head" class:section-warn={axisWarn}>
                        <label class="enable-toggle inline">
                            <input type="checkbox" checked={axis.enabled}
                                   on:change={(e) => setAxisField(gun.id, which, 'enabled', boolValue(e))} disabled={busy} />
                            {which.charAt(0).toUpperCase() + which.slice(1)} axis
                        </label>
                        {#if axisWarn}<span class="section-warn-tag">no free servo port</span>{/if}
                        <span class="hint">servo motion shape lives on the port-role row (IO tab)</span>
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

                        <!-- Phase 4c: range-mapped axis bar.  Live µs marker
                             on a 1000..2000 µs scale; the configured
                             neutral position is highlighted.  Useful for
                             eyeballing whether the channel mapping puts
                             the axis where the operator expects. -->
                        {#if axis.input}
                            {@const liveAx = liveUsFor(axis.input)}
                            <div class="axis-bar" class:nosignal={!liveAx || !liveAx.valid}>
                                <div class="axis-neutral" style="left:{usToPct(axis.neutralUs)}%" title="neutral {axis.neutralUs} µs"></div>
                                {#if liveAx && liveAx.valid}
                                    <div class="axis-fill" style="width:{usToPct(liveAx.us)}%"></div>
                                    <span class="axis-readout">{liveAx.us} µs</span>
                                {:else}
                                    <span class="axis-nosignal">{axis.input ? 'NO SIGNAL' : 'no channel bound'}</span>
                                {/if}
                            </div>
                        {/if}
                    {/if}
                {/each}

                <!-- MANUAL CONTROL / PUPPET MODE (Phase 4d, Rule 41) ─────-->
                <div class="section-head">
                    <label class="enable-toggle inline">
                        <input type="checkbox" checked={isManualOn(gun.id)}
                               on:change={(e) => toggleManual(gun.id, boolValue(e))}
                               disabled={busy || $gunfxDirty} />
                        Manual control {isManualOn(gun.id) ? '(OVERRIDING RC)' : '(RC driven)'}
                    </label>
                    <span class="hint">
                        {#if $gunfxDirty}
                            Apply changes before engaging manual mode
                        {:else}
                            firmware reverts to RC after 5 s with no input — Studio crash-safe
                        {/if}
                    </span>
                </div>

                {#if isManualOn(gun.id)}
                    {@const live = $gunfxVerbose[gun.id]}
                    <div class="puppet-grid">
                        <!-- LEFT COLUMN — actuator overrides -->
                        <div class="puppet-col">
                            <div class="col-head">Drive</div>

                            <!-- Yaw slider -->
                            {#if gun.yaw.enabled}
                                <div class="form-row">
                                    <span class="field-label" style="width:60px">Yaw</span>
                                    <input class="slider" type="range" min="500" max="2500" step="1"
                                           value={live?.yawTargetUs ?? 1500}
                                           on:input={(e) => scheduleAxisPush(gun.id, 'yaw', numValue(e))}
                                           disabled={busy} />
                                    <span class="unit mono">{live?.yawTargetUs ?? 1500} µs</span>
                                    {#if live && live.yawCurrentUs !== live.yawTargetUs}
                                        <span class="live-trail">→ {live.yawCurrentUs}</span>
                                    {/if}
                                </div>
                            {/if}

                            <!-- Pitch slider -->
                            {#if gun.pitch.enabled}
                                <div class="form-row">
                                    <span class="field-label" style="width:60px">Pitch</span>
                                    <input class="slider" type="range" min="500" max="2500" step="1"
                                           value={live?.pitchTargetUs ?? 1500}
                                           on:input={(e) => scheduleAxisPush(gun.id, 'pitch', numValue(e))}
                                           disabled={busy} />
                                    <span class="unit mono">{live?.pitchTargetUs ?? 1500} µs</span>
                                    {#if live && live.pitchCurrentUs !== live.pitchTargetUs}
                                        <span class="live-trail">→ {live.pitchCurrentUs}</span>
                                    {/if}
                                </div>
                            {/if}

                            <!-- ROF selector -->
                            {#if gun.rof.items.length > 0}
                                <div class="form-row">
                                    <span class="field-label" style="width:60px">ROF</span>
                                    <select class="field-input" style="flex:1"
                                            value={live?.rofIndex ?? 0xFF}
                                            on:change={(e) => setManualRof(gun.id, Number(selValue(e)))}
                                            disabled={busy}>
                                        <option value={255}>— none armed —</option>
                                        {#each gun.rof.items as it, i (i)}
                                            <option value={i}>{it.name || `rof${i + 1}`} · {it.rpm} rpm</option>
                                        {/each}
                                    </select>
                                </div>
                            {/if}

                            <!-- Fire (mouse-hold) + smoke + fan burst -->
                            <div class="form-row">
                                <button class="fire-btn"
                                        class:held={live?.firing}
                                        on:mousedown={() => fireHoldDown(gun.id)}
                                        on:mouseup={() => fireHoldUp(gun.id)}
                                        on:mouseleave={() => live?.firing && fireHoldUp(gun.id)}
                                        disabled={busy}>
                                    ● {live?.firing ? 'FIRING — release to stop' : 'HOLD TO FIRE'}
                                </button>
                            </div>
                            <div class="form-row">
                                <button class="small" on:click={() => toggleManualSmoke(gun.id)} disabled={busy}>
                                    {live?.smokeArmed ? '■ disarm smoke' : '◆ arm smoke'}
                                </button>
                                <button class="small" on:click={() => manualFanBurst(gun.id)} disabled={busy}
                                        title="One-shot fan puff (uses fan_puff_ms)">
                                    ⚡ fan burst
                                </button>
                            </div>
                        </div>

                        <!-- RIGHT COLUMN — live status mirror -->
                        <div class="puppet-col mirror">
                            <div class="col-head">Live mirror <span class="rate">@ ~10 Hz</span></div>
                            {#if live}
                                <div class="mirror-row">
                                    <span class="mlabel">mode</span>
                                    <span class="mval" class:m-manual={live.mode === 1}>{live.mode === 1 ? 'MANUAL' : 'RC'}</span>
                                </div>
                                <div class="mirror-row">
                                    <span class="mlabel">firing</span>
                                    <span class="mval" class:m-on={live.firing}>{live.firing ? 'YES' : '—'}</span>
                                    <span class="mlabel">smoke</span>
                                    <span class="mval" class:m-on={live.smokeArmed}>{live.smokeArmed ? 'ARMED' : '—'}</span>
                                    <span class="mlabel">fan</span>
                                    <span class="mval" class:m-on={live.smokeFanRunning}>{live.smokeFanRunning ? 'ON' : '—'}</span>
                                </div>
                                <div class="mirror-row">
                                    <span class="mlabel">heater</span>
                                    <span class="mval">{live.heaterDutyPct}%</span>
                                    {#if live.heaterTempCx10 !== 32767}
                                        <span class="mval">{(live.heaterTempCx10 / 10).toFixed(1)} °C</span>
                                    {:else}
                                        <span class="mval m-dim">no sensor</span>
                                    {/if}
                                </div>
                                <div class="mirror-row">
                                    <span class="mlabel">trigger</span>
                                    <span class="mval mono">{live.triggerUs} µs</span>
                                    <span class="mlabel">rof @</span>
                                    <span class="mval mono">{live.rofSelectorUs} µs</span>
                                    <span class="mval">→ #{live.rofIndex === 255 ? '—' : live.rofIndex + 1}</span>
                                </div>
                                {#if gun.yaw.enabled || gun.pitch.enabled}
                                    <div class="mirror-row">
                                        {#if gun.yaw.enabled}
                                            <span class="mlabel">yaw</span>
                                            <span class="mval mono">{live.yawCurrentUs} / {live.yawTargetUs}</span>
                                        {/if}
                                        {#if gun.pitch.enabled}
                                            <span class="mlabel">pitch</span>
                                            <span class="mval mono">{live.pitchCurrentUs} / {live.pitchTargetUs}</span>
                                        {/if}
                                    </div>
                                {/if}
                                <div class="mirror-row">
                                    <span class="mlabel">shots fired</span>
                                    <span class="mval mono">{live.shotsThisSession}</span>
                                </div>
                            {:else}
                                <div class="empty-state mirror-empty">
                                    waiting for first verbose-status packet…
                                </div>
                            {/if}
                        </div>
                    </div>
                {/if}
            </div>
        {/each}
    {/if}

    <!-- Footnote: deferred Phase 4 polish that doesn't ship here. -->
    {#if cfg?.enabled && cfg.guns.length > 0}
        <div class="phase4-note">
            Coming in Phase 4 follow-ups:
            <ul>
                <li><b>4b</b> — multi-band ROF overlay on a live channel bar (Rule 38); element-voltage UI on the IO tab's port-role row (Rule 42).</li>
                <li><b>4c</b> — range-mapped axis bars for yaw + pitch with min/center/max markers.</li>
                <li><b>4d</b> — manual override "puppet mode" subsection with verbose-status mirror (Rule 41).</li>
            </ul>
        </div>
    {/if}
</div>

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

    .rof-row { display: flex; align-items: center; gap: 6px; padding: 4px 0; }
    .rof-row.invalid { border-left: 3px solid var(--error); padding-left: 4px; background: rgba(255,80,80,0.05); }
    .rof-idx { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); width: 24px; flex-shrink: 0; }

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

    .phase4-note { margin-top: 18px; padding: 10px 12px; background: var(--bg-raised); border-left: 3px solid var(--accent); border-radius: 0 4px 4px 0; font-size: 11px; color: var(--text-dim); }
    .phase4-note ul { margin: 4px 0 0 18px; padding: 0; }
    .phase4-note li { margin: 2px 0; }

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
</style>
