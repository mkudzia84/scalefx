<!-- LightFxPanel — LightFx master config + active program library
     (Lighting tab left column).

     REFACTOR (2026-05-24): programs are Studio-side PRESETS now.
     This panel owns:
       - Master enable + brightness (Rule 45 enable-button)
       - Active program list — each row is an inline editor (channels +
         events + landing bindings) with no modal popup.  Picking a
         preset from the library adds it to the active list; +New seeds
         a blank slot; Save-As clones the current draft into the user
         library under a new name; Apply uploads every active draft to
         /lightfx/programs/<name>.yaml and orphan-deletes anything else.
       - Program selector (Rule 38 ChannelBandCluster) — maps RC channel
         µs bands to active program NAMES.

     Cross-references:
       - Card / button cluster                       → Rule 34
       - Validation surfacing                        → Rule 35
       - Multi-band channel cluster (selector)       → Rule 38 (ChannelBandCluster.svelte)
       - Yellow warning on empty port pool           → Rule 39
       - Modular config source / Apply gate          → Rule 46 (lightfxConfigSource)
       - Output-port pickers (Rule 49)               → freePortPoolFiltered
-->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import {
        lightfxDraft, lightfxDirty, presetLibrary,
        loadLightFxConfig, refreshPresetLibrary,
        addPresetToActive, addBlankActive, removeActive,
        setActiveProgram, renameActive,
        savePresetAs, deletePreset,
        setSelectorInput, setSelectorHysteresis,
        setSelectorRange, addSelectorRange, removeSelectorRange,
        selectorErrors, programErrors,
        LIGHT_PRESETS, PRESET_GROUP_ORDER,
        defaultTrack, defaultEvent,
        type LightFxConfigT, type ProgramSelectorRangeT,
        type PresetLibraryEntryT, type ActiveProgramT,
        type ProgramT, type TrackT, type ProgramEventT, type LightEventKindT,
    } from '../lightfx'
    import { deviceModel, liveChannels, liveChannelKey, RoleKind, formatPortRail } from '../devicemodel'
    import { landingDraft } from '../landing'
    import { PreviewLightChannel, StopLightChannel } from '../../../wailsjs/go/main/App'
    import ChannelBandCluster from '../components/ChannelBandCluster.svelte'

    let busy = false
    let error = ''

    let cfg: LightFxConfigT
    const unsubCfg = lightfxDraft.subscribe(c => { cfg = c })

    // Which active slots are EXPANDED for inline editing.  Closed by
    // default so the panel doesn't dump 5+ huge editors at once on
    // re-connect; operator clicks Edit on the rows they care about.
    let expanded: Record<number, boolean> = {}

    // Save-As dialog inline state (we want a "modal-feel" name picker
    // without dragging a real dialog component back in).  When set,
    // `saveAsFor` is the active-list index; `saveAsName` is the typed
    // text; `saveAsError` is the validation message.
    let saveAsFor: number | null = null
    let saveAsName = ''
    let saveAsError = ''

    // Per-channel preview-playing state, keyed by (activeIdx, channelIdx)
    // → port-idx the LED was started on.  Used for the per-channel ▶/■
    // toggle and panel-unmount cleanup (no leaks when operator navigates
    // away with a preview running).
    let playing: Record<string, number> = {}
    function playKey(ai: number, ci: number): string { return `${ai}:${ci}` }

    onMount(() => {
        loadLightFxConfig().catch(e => { error = String(e) })
        refreshPresetLibrary().catch(() => {})
    })
    onDestroy(() => {
        unsubCfg()
        // Stop any per-channel previews we kicked off.
        for (const k of Object.keys(playing)) {
            const portIdx = playing[k]
            if (portIdx !== undefined) StopLightChannel(portIdx).catch(() => {})
        }
    })

    // ─── Named-channel options (Rule 43, for the selector picker) ────
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
    function liveUsFor(fnId: string): { us: number; valid: boolean } | null {
        if (!fnId) return null
        for (const inp of $deviceModel.inputs) {
            for (const c of inp.channels) {
                if (c.function !== fnId) continue
                const lc = $liveChannels[liveChannelKey(
                    { guid: inp.port.guid, kind: 4, index: inp.port.index }, c.channel)]
                if (!lc) return { us: 1500, valid: false }
                return { us: lc.us, valid: lc.valid }
            }
        }
        return null
    }

    // ─── Selector bands (Rule 38) — driven by active program names. ──
    const BAND_PALETTE = ['#5b9dff', '#ffa05b', '#5bd28b', '#d65bd2']
    $: activeNames = (cfg?.activePrograms ?? []).map(a => a.name)
    function buildSelectorBands(ranges: ProgramSelectorRangeT[], liveUs: number | null, liveValid: boolean) {
        return ranges.map((r, i) => ({
            loUs: r.fromUs, hiUs: r.toUs,
            name: r.program || `range${i + 1}`,
            meta: r.program ? `program · ${r.program}` : 'no program picked',
            color: BAND_PALETTE[i % BAND_PALETTE.length],
            armed: liveValid && liveUs != null
                && (r.fromUs === 0 || liveUs >= r.fromUs)
                && (r.toUs   === 0 || liveUs <= r.toUs),
        }))
    }
    function detectRangeOverlaps(ranges: ProgramSelectorRangeT[]): number[] {
        const out: number[] = []
        for (let i = 0; i < ranges.length; i++)
            for (let j = i + 1; j < ranges.length; j++) {
                const al = ranges[i].fromUs || 1000, ah = ranges[i].toUs || 2000
                const bl = ranges[j].fromUs || 1000, bh = ranges[j].toUs || 2000
                if (al <= bh && bl <= ah) {
                    if (!out.includes(i)) out.push(i)
                    if (!out.includes(j)) out.push(j)
                }
            }
        return out
    }
    // Suggest a non-overlapping band for the next selector range using
    // the shared `suggestNextBand` helper (lib/range-suggest.ts) — same
    // guard-aware logic GunFx ROF uses, so both widgets behave
    // consistently for the operator.
    function suggestNextRange(ranges: ProgramSelectorRangeT[]): {
        from: number; to: number; trimIdx: number; trimNewHi: number
    } {
        const explicit = ranges
            .filter(r => r.fromUs > 0 && r.toUs > 0 && r.toUs > r.fromUs)
        const indexed = ranges.map((r, idx) => ({ r, idx }))
            .filter(({ r }) => r.fromUs > 0 && r.toUs > 0 && r.toUs > r.fromUs)
        const sug = suggestNextBand(explicit.map(r => ({ lo: r.fromUs, hi: r.toUs })))
        const trimIdx = sug.trimSiblingIdx >= 0 ? indexed[sug.trimSiblingIdx].idx : -1
        return {
            from: sug.band.lo, to: sug.band.hi,
            trimIdx, trimNewHi: sug.trimSiblingNewHi,
        }
    }

    // ─── Templates picker grouping (by category) ─────────────────────
    $: groupedTemplates = groupTemplates($presetLibrary, cfg)
    function groupTemplates(lib: PresetLibraryEntryT[], _cfg: LightFxConfigT) {
        const used = new Set(cfg?.activePrograms.map(a => a.name) ?? [])
        const groups: Record<string, PresetLibraryEntryT[]> = {}
        for (const e of lib) {
            if (used.has(e.name)) continue          // already on active list
            if (!groups[e.category]) groups[e.category] = []
            groups[e.category].push(e)
        }
        return Object.entries(groups).sort(([a], [b]) => a.localeCompare(b))
    }

    // ─── Channel directory ───────────────────────────────────────────
    // v2 model: tracks reference LED channels by NAME.  The "channel"
    // is a named LedAnimator port on /hubfx.yaml; the IO tab is where
    // that name gets assigned.  This panel just consumes the list.
    //
    // `availableChannels` = every LedAnimator-roled OUTPUT port that
    // has a non-empty operator label.  Unlabelled ports are unreachable
    // from LightFx programs — they show up as a warning chip with a
    // "label this on the IO tab" pointer.
    type ChannelOpt = { name: string; portLabel: string }
    $: availableChannels = collectChannels($deviceModel)
    $: unlabelledLedCount = countUnlabelledLed($deviceModel)
    function collectChannels(_dm: typeof $deviceModel): ChannelOpt[] {
        const out: ChannelOpt[] = []
        for (const p of $deviceModel.ports) {
            if (p.direction !== 'output') continue
            if (p.kindName !== 'pwm') continue
            if (p.roleKind !== RoleKind.LedAnimator) continue
            const name = (p.name ?? '').trim()
            if (!name) continue
            const rail = formatPortRail(p.voltageMv)
            out.push({
                name,
                portLabel: `${p.boardName ?? 'Hub'} · ${p.hardwareName}${rail ? ` · ${rail}` : ''}`,
            })
        }
        // Stable alpha sort — predictable dropdown ordering across renders.
        out.sort((a, b) => a.name.localeCompare(b.name))
        return out
    }
    function countUnlabelledLed(_dm: typeof $deviceModel): number {
        let n = 0
        for (const p of $deviceModel.ports) {
            if (p.direction === 'output' && p.kindName === 'pwm'
                && p.roleKind === RoleKind.LedAnimator
                && (!p.name || !p.name.trim())) n++
        }
        return n
    }
    /** Returns the channel options available to track `ti` in active
     *  program `ai`: filters out names already used by SIBLING tracks
     *  in the same program (a program can't drive the same channel
     *  twice — two tracks would fight).  The current pick stays
     *  visible so editing doesn't drop it out of the dropdown. */
    function channelsForTrack(ai: number, ti: number, current: string): ChannelOpt[] {
        const used = new Set<string>()
        const slot = cfg?.activePrograms[ai]
        if (slot) {
            for (let i = 0; i < slot.program.tracks.length; i++) {
                if (i === ti) continue
                const c = slot.program.tracks[i].channel
                if (c) used.add(c)
            }
        }
        return availableChannels.filter(c => c.name === current || !used.has(c.name))
    }

    // Update one active slot's program through `setActiveProgram` —
    // every mutator below funnels through this.
    function mutateProgram(ai: number, fn: (p: ProgramT) => ProgramT) {
        const slot = cfg.activePrograms[ai]
        if (!slot) return
        setActiveProgram(ai, fn(slot.program))
    }
    function addTrack(ai: number) {
        // Seed with the first unused channel name if any are free; else
        // empty (operator picks from the dropdown).  No more "Channel N"
        // placeholder — the channel name IS the identity, not a label
        // the operator types in.
        mutateProgram(ai, p => {
            const used = new Set(p.tracks.map(t => t.channel).filter(Boolean))
            const seed = availableChannels.find(c => !used.has(c.name))?.name ?? ''
            return { ...p, tracks: [...p.tracks, defaultTrack(seed)] }
        })
    }
    function removeTrack(ai: number, ti: number) {
        mutateProgram(ai, p => ({ ...p, tracks: p.tracks.filter((_, i) => i !== ti) }))
    }
    function setTrack(ai: number, ti: number, patch: Partial<TrackT>) {
        mutateProgram(ai, p => ({ ...p, tracks: p.tracks.map((t, i) =>
            i === ti ? { ...t, ...patch } : t) }))
    }
    function addEvent(ai: number, ti: number) {
        mutateProgram(ai, p => ({ ...p, tracks: p.tracks.map((t, i) =>
            i === ti ? { ...t, events: [...t.events, defaultEvent({ kind: 'on', durationMs: 100 })] } : t) }))
    }
    function removeEvent(ai: number, ti: number, ei: number) {
        mutateProgram(ai, p => ({ ...p, tracks: p.tracks.map((t, i) =>
            i === ti ? { ...t, events: t.events.filter((_, j) => j !== ei) } : t) }))
    }
    function setEvent(ai: number, ti: number, ei: number, patch: Partial<ProgramEventT>) {
        mutateProgram(ai, p => ({ ...p, tracks: p.tracks.map((t, i) =>
            i === ti ? { ...t, events: t.events.map((e, j) =>
                j === ei ? { ...e, ...patch } : e) } : t) }))
    }

    function applyChannelPreset(ai: number, ti: number, presetId: string) {
        if (!presetId) return
        const preset = LIGHT_PRESETS.find(p => p.id === presetId)
        if (!preset) return
        // v2: preset only contributes events + loop.  The track's
        // `channel` reference (the operator's named LED) stays put —
        // we're picking a PATTERN to apply to an already-chosen channel,
        // not renaming the channel itself.
        mutateProgram(ai, p => ({ ...p, tracks: p.tracks.map((t, i) =>
            i === ti ? {
                ...t,
                loop:   preset.loop,
                events: preset.events.map(e => ({ ...e })),
            } : t) }))
    }
    function addLandingBinding(ai: number) {
        mutateProgram(ai, p => ({ ...p, landingBindings: [...p.landingBindings, { id: 0, state: 'on' }] }))
    }
    function removeLandingBinding(ai: number, bi: number) {
        mutateProgram(ai, p => ({ ...p, landingBindings: p.landingBindings.filter((_, i) => i !== bi) }))
    }
    function setBindingId(ai: number, bi: number, id: number) {
        mutateProgram(ai, p => ({ ...p, landingBindings: p.landingBindings.map((b, i) =>
            i === bi ? { ...b, id } : b) }))
    }
    function setBindingState(ai: number, bi: number, st: 'on' | 'off') {
        mutateProgram(ai, p => ({ ...p, landingBindings: p.landingBindings.map((b, i) =>
            i === bi ? { ...b, state: st } : b) }))
    }

    // ─── Per-event field map (which params each kind reads) ──────────
    const FIELD_MAP: Record<LightEventKindT, Array<keyof ProgramEventT>> = {
        on:       ['brightnessPct', 'durationMs'],
        off:      ['durationMs'],
        flash:    ['cycleMs', 'brightnessPct', 'flashPct', 'durationMs'],
        fade_in:  ['durationMs', 'brightnessPct'],
        fade_out: ['durationMs', 'brightnessPct'],
        fading:   ['cycleMs', 'minPct', 'maxPct', 'durationMs'],
        beacon:   ['cycleMs', 'minPct', 'maxPct', 'flashPct', 'durationMs'],
    }
    const KIND_OPTIONS: { id: LightEventKindT; label: string }[] = [
        { id: 'on',       label: 'On (steady)' },
        { id: 'off',      label: 'Off (gap)' },
        { id: 'flash',    label: 'Flash (square wave)' },
        { id: 'fade_in',  label: 'Fade in' },
        { id: 'fade_out', label: 'Fade out' },
        { id: 'fading',   label: 'Fading (sinusoidal)' },
        { id: 'beacon',   label: 'Beacon (peak with baseline)' },
    ]

    // ─── Per-track live preview (hub-local only for now) ─────────────
    // v2 model: tracks reference channels by NAME.  Resolution to a
    // port idx happens client-side here, by walking $deviceModel.ports
    // for the LedAnimator entry whose label matches.  Unresolved tracks
    // can't preview — operator labels the port on the IO tab first.
    function resolveChannelToPortIdx(channelName: string): {
        idx: number; guid: string
    } | null {
        if (!channelName) return null
        for (const p of $deviceModel.ports) {
            if (p.direction !== 'output' || p.kindName !== 'pwm') continue
            if (p.roleKind !== RoleKind.LedAnimator) continue
            if ((p.name ?? '').trim() === channelName) {
                return { idx: p.ref.index, guid: p.ref.guid }
            }
        }
        return null
    }
    async function playTrack(ai: number, ti: number) {
        const t = cfg.activePrograms[ai]?.program?.tracks[ti]
        if (!t || !t.channel || t.events.length === 0) return
        const resolved = resolveChannelToPortIdx(t.channel)
        if (!resolved) {
            error = `Channel "${t.channel}" isn't labelled on any LedAnimator port — label it on the IO tab first.`
            return
        }
        if (resolved.guid !== '') {
            error = `Preview not yet wired for expander port (${t.channel} on ${resolved.guid})`
            return
        }
        try {
            await PreviewLightChannel(resolved.idx, t.brightnessPct, t.loop, t.events.map(e => ({
                kind: e.kind, durationMs: e.durationMs, cycleMs: e.cycleMs,
                brightnessPct: e.brightnessPct, minPct: e.minPct,
                maxPct: e.maxPct, flashPct: e.flashPct,
            })))
            playing = { ...playing, [playKey(ai, ti)]: resolved.idx }
        } catch (e) { error = String(e) }
    }
    async function stopTrack(ai: number, ti: number) {
        const k = playKey(ai, ti)
        const portIdx = playing[k]
        if (portIdx === undefined) return
        try { await StopLightChannel(portIdx) } catch { /* best effort */ }
        const next = { ...playing }; delete next[k]; playing = next
    }

    // ─── Per-PROGRAM preview — fires every track that resolves to a
    //     port AND has events.  Each track queues independently (same
    //     LedAnimator role per port; the firmware times them off a
    //     shared clock so `loop: true` tracks stay phase-locked).  Stop
    //     halts every track this program owns. ───────────────────────
    async function playProgram(ai: number) {
        const slot = cfg.activePrograms[ai]
        if (!slot) return
        for (let ti = 0; ti < slot.program.tracks.length; ti++) {
            const t = slot.program.tracks[ti]
            if (!t.channel || t.events.length === 0) continue
            await playTrack(ai, ti)
        }
    }
    async function stopProgram(ai: number) {
        const slot = cfg.activePrograms[ai]
        if (!slot) return
        for (let ti = 0; ti < slot.program.tracks.length; ti++) {
            await stopTrack(ai, ti)
        }
    }
    /** True iff any track of program `ai` has a running preview. */
    function programIsPlaying(ai: number, p: Record<string, number>): boolean {
        const slot = cfg?.activePrograms[ai]
        if (!slot) return false
        for (let ti = 0; ti < slot.program.tracks.length; ti++) {
            if (p[playKey(ai, ti)] !== undefined) return true
        }
        return false
    }
    /** True iff program `ai` has at least one playable track (channel
     *  resolved + events) — disables Play when not. */
    function programHasPlayable(slot: ActiveProgramT): boolean {
        return slot.program.tracks.some(t =>
            !!t.channel && t.events.length > 0 && resolveChannelToPortIdx(t.channel) !== null)
    }

    // ─── Save-As (clones the active draft into the user templates) ───
    function openSaveAs(ai: number) {
        const slot = cfg.activePrograms[ai]
        saveAsFor = ai
        // Suggest "<current>-copy" so the operator just confirms unless
        // they want to type a more specific name.
        saveAsName = slot?.name ? `${slot.name}-copy` : 'untitled-copy'
        saveAsError = ''
    }
    function closeSaveAs() { saveAsFor = null; saveAsName = ''; saveAsError = '' }
    async function confirmSaveAs() {
        if (saveAsFor === null) return
        const name = saveAsName.trim()
        if (!/^[A-Za-z0-9_][A-Za-z0-9_\-]{0,63}$/.test(name)) {
            saveAsError = 'Invalid name — use letters, digits, _ and -; 1–64 chars; must start with letter/digit/_.'
            return
        }
        // Factory-name collisions are rejected server-side (no
        // shadowing rule, 2026-05-24).  User-vs-user collisions warrant
        // one local confirm before we overwrite.
        const existing = $presetLibrary.find(e => e.name === name)
        if (existing && existing.source === 'user') {
            if (!confirm(`Overwrite existing user template "${name}"?`)) return
        }
        try {
            await savePresetAs(saveAsFor, name)
            closeSaveAs()
        } catch (e) { saveAsError = String(e) }
    }

    async function onDeleteTemplate(name: string) {
        if (!confirm(`Delete user template "${name}"?  This only removes it from the Templates menu — already-active slots stay until you remove them too.`)) return
        try {
            await deletePreset(name)
        } catch (e) { error = String(e) }
    }

    // ─── Add-template picker (collapsible) ───────────────────────────
    let addOpen = false
    function pickTemplate(name: string) {
        addPresetToActive(name)
        addOpen = false
        // Auto-expand the just-added slot so the operator sees the
        // inline editor immediately (matches the + Blank button's behaviour).
        const idx = (cfg?.activePrograms.length ?? 1) - 1
        expanded = { ...expanded, [idx]: true }
    }
    function pickBlank() {
        addBlankActive()
        addOpen = false
        const idx = (cfg?.activePrograms.length ?? 1) - 1
        expanded = { ...expanded, [idx]: true }
    }

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    function checkedValue(e: Event): boolean { return (e.target as HTMLInputElement).checked }
    /** Svelte 4 attribute parser rejects TS `as` casts — this helper
     *  pulls the trimmed text out of the rename input.  Inline lambda
     *  in the template parses past the `as` cast as a JS comparison. */
    function strValue(e: Event): string { return (e.target as HTMLInputElement).value.trim() }
    function onPickEventKind(ai: number, ci: number, ei: number, val: string) {
        setEvent(ai, ci, ei, { kind: val as LightEventKindT })
    }
    function onPickChannelPreset(ai: number, ci: number, e: Event) {
        const v = selValue(e); applyChannelPreset(ai, ci, v)
        ;(e.target as HTMLSelectElement).value = ''
    }

    function onAddSelectorRange() {
        const next = suggestNextRange(cfg.programSelector.ranges)
        const first = activeNames.find(n =>
            !cfg.programSelector.ranges.some(r => r.program === n)) ?? activeNames[0] ?? ''
        // If the suggester bisected an existing range, trim that sibling
        // FIRST so the new band lands in the freshly-opened gap (same
        // pattern as GunFx ROF's addRofItem).  Skip when trimIdx == -1
        // (gap path didn't bisect).
        if (next.trimIdx >= 0) setSelectorRange(next.trimIdx, { toUs: next.trimNewHi })
        addSelectorRange({ fromUs: next.from, toUs: next.to, program: first })
    }

    $: selectorErrs = cfg ? selectorErrors(cfg) : []
</script>

<div class="card lightfx-card">
    <div class="card-header">
        <h3>Light Effects</h3>
        <div class="header-actions">
            <button class="small state-toggle" class:state-on={cfg?.enabled}
                    on:click={() => lightfxDraft.update(c => ({ ...c, enabled: !c.enabled }))}
                    disabled={busy}
                    title={cfg?.enabled
                        ? 'Disable LightFx (press Apply to push)'
                        : 'Enable LightFx (press Apply to push)'}>
                {cfg?.enabled ? '✓ Enabled' : '▶ Disabled'}
            </button>
        </div>
    </div>

    {#if error}<div class="banner err">{error}<button class="dismiss" on:click={() => error = ''}>✕</button></div>{/if}

    {#if cfg?.enabled}
        <!-- Master brightness -->
        <div class="form-row">
            <span class="field-label">Master brightness</span>
            <input class="field-input narrow" type="number" min="0" max="100"
                   value={cfg.masterBrightnessPct}
                   on:change={(e) => lightfxDraft.update(c => ({ ...c, masterBrightnessPct: Math.max(0, Math.min(100, numValue(e) | 0)) }))}
                   disabled={busy}
                   title="0–100 % — applied as an additional multiplier on every channel.  Acts as a global dimmer." />
            <span class="unit">%</span>
        </div>

        <!-- ─── Active programs ─────────────────────────────────────── -->
        <div class="section-head" class:section-error={cfg.activePrograms.length === 0}>
            Active programs
            <span class="hint">picked + edited here; Apply syncs to /lightfx/programs/ and removes orphans</span>
            {#if cfg.activePrograms.length === 0}<span class="section-err-tag">no programs — LightFx idle</span>{/if}
        </div>

        {#if cfg.activePrograms.length === 0}
            <div class="empty-state">
                No programs yet — add one below.  Pick a built-in
                <strong>Template</strong> as a starting point, or click
                <strong>+ Blank</strong> to compose from scratch.  Edit
                channels and events inline; Apply uploads everything in
                this list to the device.
            </div>
        {/if}

        {#each cfg.activePrograms as slot, ai (ai)}
            {@const errs       = programErrors(slot.program)}
            {@const isPlaying  = programIsPlaying(ai, playing)}
            {@const canPlay    = programHasPlayable(slot)}
            <div class="active-row" class:invalid={errs.length > 0}>
                <div class="active-head">
                    <span class="ap-idx">#{ai + 1}</span>
                    <input class="field-input ap-name" type="text"
                           value={slot.name}
                           on:change={(e) => renameActive(ai, strValue(e))}
                           title="Rename — becomes /lightfx/programs/<name>.yaml on Apply" />
                    {#if errs.length > 0}
                        <span class="row-err-tag" title={errs.join('\n')}>⚠ {errs.length} error{errs.length > 1 ? 's' : ''}</span>
                    {/if}
                    <!-- Per-program preview cluster (Rule 48 .op-cluster).
                         Stops every channel this program owns; doesn't
                         touch the device's persisted programs[].  Hub-
                         local channels only (same gap as per-channel
                         preview — expander LEDs need a Topology forward
                         of LED_QUEUE_LOAD). -->
                    <div class="op-cluster row-preview">
                        {#if isPlaying}
                            <button class="oc-btn oc-danger row-action"
                                    on:click={() => stopProgram(ai)}
                                    title="Stop every channel in this program">■ Stop</button>
                        {:else}
                            <button class="oc-btn oc-primary row-action"
                                    on:click={() => playProgram(ai)}
                                    disabled={!canPlay}
                                    title={canPlay
                                        ? 'Preview: fires every channel that has a port + events.  No upload, no Save needed.'
                                        : 'No playable channels — assign a port and at least one event first.'}>▶ Play</button>
                        {/if}
                    </div>
                    <button class="small row-action {expanded[ai] ? 'state-toggle state-on' : ''}"
                            on:click={() => expanded = { ...expanded, [ai]: !expanded[ai] }}
                            title="Show / hide the channel + event editor for this program">
                        {expanded[ai] ? '▾ Hide' : '✎ Edit'}
                    </button>
                    <button class="small row-action" on:click={() => openSaveAs(ai)}
                            title="Save the current draft as a new user template (so you can re-use it on other devices)">💾 Save As…</button>
                    <button class="small danger row-action btn-slot" on:click={() => removeActive(ai)}
                            title="Remove this program from the active list (template copy stays in the Templates menu)">× Remove</button>
                </div>

                {#if expanded[ai]}
                    <!-- ─── Inline program editor ─────────────────── -->
                    <div class="active-body">
                        <!-- Tracks — each one references an LED channel by NAME -->
                        <div class="subsection-head" class:section-warn={availableChannels.length === 0}>
                            Tracks
                            <span class="hint">each track plays an event list on a named LED channel; unlisted channels stay off</span>
                            {#if availableChannels.length === 0}
                                <span class="section-warn-tag"
                                      title="No LedAnimator ports have an operator label.  Open the IO tab and give each LED port a friendly name (e.g. 'Red beacon'); those names become the channel handles every program references.">
                                    no labelled LED channels — label LedAnimator ports on the IO tab
                                </span>
                            {:else if unlabelledLedCount > 0}
                                <span class="section-warn-tag" title="More LedAnimator ports exist but have no name yet — label them on the IO tab to expose them here.">
                                    {unlabelledLedCount} unlabelled LED port{unlabelledLedCount > 1 ? 's' : ''}
                                </span>
                            {/if}
                        </div>

                        {#if slot.program.tracks.length === 0}
                            <div class="empty-state subtle">
                                No tracks yet — click <strong>+ Add track</strong> to drive a
                                named LED channel.  Channels are defined once on the IO tab
                                (port → role → label); every program references the same names.
                            </div>
                        {/if}

                        {#each slot.program.tracks as t, ti (ti)}
                            {@const channelOpts = channelsForTrack(ai, ti, t.channel)}
                            {@const channelMissing = !!t.channel && !availableChannels.some(c => c.name === t.channel)}
                            <div class="channel-card" class:invalid={!t.channel || t.events.length === 0 || channelMissing}>
                                <div class="channel-head">
                                    <span class="ch-idx">#{ti + 1}</span>
                                    <select class="field-input wide"
                                            value={t.channel}
                                            on:change={(e) => setTrack(ai, ti, { channel: selValue(e) })}
                                            disabled={availableChannels.length === 0}
                                            title={availableChannels.length === 0
                                                ? 'Label an LedAnimator port on the IO tab first.'
                                                : 'Pick the named LED channel this track drives'}>
                                        <option value="">— Pick LED channel —</option>
                                        {#each channelOpts as opt}
                                            <option value={opt.name} title={opt.portLabel}>{opt.name}</option>
                                        {/each}
                                        {#if channelMissing}
                                            <option value={t.channel}>{t.channel} (not labelled on this device)</option>
                                        {/if}
                                    </select>
                                    <span class="field-label">brightness</span>
                                    <input class="field-input narrow" type="number" min="0" max="100"
                                           value={t.brightnessPct}
                                           on:change={(e) => setTrack(ai, ti, { brightnessPct: numValue(e) })} />
                                    <span class="unit">%</span>
                                    <label class="loop-toggle"
                                           title="Phase-locked repeating pattern — the events list loops as a cycle (period = sum of all event durations).  Sibling tracks with the same period stay in lock-step.">
                                        <input type="checkbox" checked={t.loop}
                                               on:change={(e) => setTrack(ai, ti, { loop: checkedValue(e) })} />
                                        loop
                                    </label>
                                    <select class="field-input preset-select"
                                            on:change={(e) => onPickChannelPreset(ai, ti, e)}
                                            title="Replace this track's events with a canonical pattern (FAA strobe, beacon, nav, …).">
                                        <option value="">↺ Preset…</option>
                                        {#each PRESET_GROUP_ORDER as g}
                                            {@const grp = LIGHT_PRESETS.filter(p => p.group === g && !p.label.startsWith('(alias)'))}
                                            {#if grp.length > 0}
                                                <optgroup label={g}>
                                                    {#each grp as preset}
                                                        <option value={preset.id} title={preset.note}>{preset.label}</option>
                                                    {/each}
                                                </optgroup>
                                            {/if}
                                        {/each}
                                    </select>
                                    <div class="op-cluster ch-preview">
                                        {#if playing[playKey(ai, ti)] !== undefined}
                                            <button class="oc-btn oc-danger"
                                                    on:click={() => stopTrack(ai, ti)} title="Stop this track preview">■</button>
                                        {:else}
                                            <button class="oc-btn oc-primary"
                                                    on:click={() => playTrack(ai, ti)}
                                                    disabled={!t.channel || t.events.length === 0 || channelMissing}
                                                    title={!t.channel
                                                        ? 'Pick a channel first'
                                                        : channelMissing
                                                            ? 'Channel not labelled on this device — fix on the IO tab'
                                                            : t.events.length === 0
                                                                ? 'No events to play'
                                                                : 'Preview this track — no save required'}>▶</button>
                                        {/if}
                                    </div>
                                    <button class="small danger ch-remove" on:click={() => removeTrack(ai, ti)} title="Remove this track">×</button>
                                </div>

                                <table class="events-table">
                                    <thead>
                                        <tr>
                                            <th class="evt-idx">#</th>
                                            <th>kind</th>
                                            <th class="param">brightness %</th>
                                            <th class="param">min %</th>
                                            <th class="param">max %</th>
                                            <th class="param">flash %</th>
                                            <th class="param">cycle ms</th>
                                            <th class="param">duration ms</th>
                                            <th class="evt-rm"></th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        {#each t.events as ev, ei (ei)}
                                            {@const used = FIELD_MAP[ev.kind] ?? []}
                                            <tr>
                                                <td class="evt-idx">{ei + 1}</td>
                                                <td>
                                                    <select class="field-input compact"
                                                            value={ev.kind}
                                                            on:change={(e) => onPickEventKind(ai, ti, ei, selValue(e))}>
                                                        {#each KIND_OPTIONS as k}<option value={k.id}>{k.label}</option>{/each}
                                                    </select>
                                                </td>
                                                <td class="param" class:dim={!used.includes('brightnessPct')}>
                                                    <input class="field-input compact num" type="number" min="0" max="100"
                                                           value={ev.brightnessPct} disabled={!used.includes('brightnessPct')}
                                                           on:change={(e) => setEvent(ai, ti, ei, { brightnessPct: numValue(e) })} />
                                                </td>
                                                <td class="param" class:dim={!used.includes('minPct')}>
                                                    <input class="field-input compact num" type="number" min="0" max="100"
                                                           value={ev.minPct} disabled={!used.includes('minPct')}
                                                           on:change={(e) => setEvent(ai, ti, ei, { minPct: numValue(e) })} />
                                                </td>
                                                <td class="param" class:dim={!used.includes('maxPct')}>
                                                    <input class="field-input compact num" type="number" min="0" max="100"
                                                           value={ev.maxPct} disabled={!used.includes('maxPct')}
                                                           on:change={(e) => setEvent(ai, ti, ei, { maxPct: numValue(e) })} />
                                                </td>
                                                <td class="param" class:dim={!used.includes('flashPct')}>
                                                    <input class="field-input compact num" type="number" min="0" max="100"
                                                           value={ev.flashPct} disabled={!used.includes('flashPct')}
                                                           on:change={(e) => setEvent(ai, ti, ei, { flashPct: numValue(e) })} />
                                                </td>
                                                <td class="param" class:dim={!used.includes('cycleMs')}>
                                                    <input class="field-input compact num" type="number" min="0" max="65535"
                                                           value={ev.cycleMs} disabled={!used.includes('cycleMs')}
                                                           on:change={(e) => setEvent(ai, ti, ei, { cycleMs: numValue(e) })} />
                                                </td>
                                                <td class="param" class:dim={!used.includes('durationMs')}>
                                                    <input class="field-input compact num" type="number" min="0" max="65535"
                                                           value={ev.durationMs} disabled={!used.includes('durationMs')}
                                                           on:change={(e) => setEvent(ai, ti, ei, { durationMs: numValue(e) })} />
                                                </td>
                                                <td class="evt-rm">
                                                    <button class="small danger" on:click={() => removeEvent(ai, ti, ei)} title="Remove this event">×</button>
                                                </td>
                                            </tr>
                                        {/each}
                                    </tbody>
                                </table>
                                <div class="form-row">
                                    <button class="small" on:click={() => addEvent(ai, ti)}>+ Add event</button>
                                </div>
                            </div>
                        {/each}

                        <div class="form-row">
                            <button class="small" on:click={() => addTrack(ai)}
                                    disabled={availableChannels.length === 0}
                                    title={availableChannels.length === 0
                                        ? 'No labelled LED channels — go to the IO tab and label an LedAnimator port'
                                        : 'Add a track driving a named LED channel'}>+ Add track</button>
                        </div>

                        <!-- Landing bindings -->
                        <div class="subsection-head">
                            Landing bindings
                            <span class="hint">when this program becomes active, each binding's state is sent to its landing-light id</span>
                        </div>
                        {#if slot.program.landingBindings.length === 0}
                            <div class="empty-state subtle">No bindings — this program won't move landing lights when it becomes active.</div>
                        {/if}
                        {#each slot.program.landingBindings as b, bi (bi)}
                            <div class="form-row binding-row">
                                <span class="field-label">Landing id</span>
                                <select class="field-input narrow"
                                        value={b.id}
                                        on:change={(e) => setBindingId(ai, bi, Number(selValue(e)))}>
                                    {#each ($landingDraft?.lights ?? []) as l}
                                        <option value={l.id}>#{l.id} {l.name ? '· ' + l.name : ''}</option>
                                    {/each}
                                    {#if !($landingDraft?.lights ?? []).some(l => l.id === b.id)}
                                        <option value={b.id}>#{b.id} (not in /landing.yaml)</option>
                                    {/if}
                                </select>
                                <span class="field-label">State</span>
                                <select class="field-input narrow"
                                        value={b.state}
                                        on:change={(e) => setBindingState(ai, bi, selValue(e) === 'off' ? 'off' : 'on')}>
                                    <option value="on">on  (deploy)</option>
                                    <option value="off">off (retract)</option>
                                </select>
                                <button class="small danger btn-slot" on:click={() => removeLandingBinding(ai, bi)}>× Remove</button>
                            </div>
                        {/each}
                        <div class="form-row">
                            <button class="small" on:click={() => addLandingBinding(ai)}
                                    disabled={($landingDraft?.lights ?? []).length === 0}
                                    title={($landingDraft?.lights ?? []).length === 0
                                        ? 'Add a landing-light group on the right column first'
                                        : 'Add a binding for this program'}>+ Add binding</button>
                        </div>

                        {#if errs.length > 0}
                            <ul class="ch-issues">
                                {#each errs as msg}<li class="ch-issue err">⚠ {msg}</li>{/each}
                            </ul>
                        {/if}
                    </div>
                {/if}
            </div>
        {/each}

        <!-- ─── Add-template picker ────────────────────────────────── -->
        <div class="form-row add-row">
            <button class="small {addOpen ? 'state-toggle state-on' : ''}"
                    on:click={() => addOpen = !addOpen}>
                {addOpen ? '▾ Close templates' : '+ Template…'}
            </button>
            <button class="small" on:click={pickBlank}
                    title="Add an empty slot — assign ports and build events from scratch">+ Blank</button>
            <button class="small" on:click={() => refreshPresetLibrary()}
                    title="Re-read the templates catalog from disk (factory + user dir)">↻</button>
        </div>

        {#if addOpen}
            <div class="templates-grid">
                {#each groupedTemplates as [cat, items]}
                    <div class="tpl-group">
                        <div class="tpl-group-head">{cat}</div>
                        {#each items as entry}
                            <div class="tpl-row">
                                <button class="small tpl-pick" on:click={() => pickTemplate(entry.name)}
                                        title={entry.note}>
                                    + {entry.name}
                                </button>
                                <span class="tpl-note">{entry.note.slice(0, 80)}</span>
                                {#if entry.source === 'user'}
                                    <button class="small danger tpl-del"
                                            on:click|stopPropagation={() => onDeleteTemplate(entry.name)}
                                            title="Delete this user template (does not affect active slots)">×</button>
                                {/if}
                            </div>
                        {/each}
                    </div>
                {/each}
                {#if groupedTemplates.length === 0}
                    <div class="empty-state subtle">
                        Every template is already in the active list — remove one
                        or click <strong>+ Blank</strong> to add another slot.
                    </div>
                {/if}
            </div>
        {/if}

        <!-- ─── Save-As inline dialog ──────────────────────────────── -->
        {#if saveAsFor !== null}
            <div class="save-as-row">
                <span class="field-label">Save as</span>
                <input class="field-input wide" type="text" bind:value={saveAsName}
                       on:keydown={(e) => { if (e.key === 'Enter') confirmSaveAs(); if (e.key === 'Escape') closeSaveAs() }}
                       autofocus />
                <button class="small primary" on:click={confirmSaveAs}>Save</button>
                <button class="small" on:click={closeSaveAs}>Cancel</button>
                {#if saveAsError}<span class="save-as-err">⚠ {saveAsError}</span>{/if}
            </div>
        {/if}

        <!-- ─── Program selector (Rule 38) ─────────────────────────── -->
        <div class="section-head">
            Program selector
            <span class="hint">drive program switching from an RC channel — one µs band per program</span>
        </div>
        <ChannelBandCluster
            channelLabel="Selector channel"
            emptyOption="— none (use first program always) —"
            options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
            inputId={cfg.programSelector.input}
            bands={buildSelectorBands(cfg.programSelector.ranges,
                liveUsFor(cfg.programSelector.input)?.us ?? null,
                liveUsFor(cfg.programSelector.input)?.valid ?? false)}
            overlapIndices={detectRangeOverlaps(cfg.programSelector.ranges)}
            liveUs={liveUsFor(cfg.programSelector.input)?.us ?? null}
            liveValid={liveUsFor(cfg.programSelector.input)?.valid ?? false}
            busy={busy}
            onInputChange={(v) => setSelectorInput(v)} />

        {#if cfg.programSelector.input}
            <div class="form-row">
                <span class="field-label">Hysteresis</span>
                <input class="field-input narrow" type="number" min="0" max="500" step="5"
                       value={cfg.programSelector.hysteresisUs}
                       on:change={(e) => setSelectorHysteresis(numValue(e))} disabled={busy} />
                <span class="unit">µs</span>
                <span class="hint">prevents the selector flipping when the stick sits on a range boundary</span>
            </div>
            {#each cfg.programSelector.ranges as r, i (i)}
                <div class="form-row range-row">
                    <span class="rng-idx">#{i + 1}</span>
                    <span class="field-label">µs band</span>
                    <input class="field-input narrow" type="number" min="800" max="2200" step="10"
                           value={r.fromUs}
                           on:change={(e) => setSelectorRange(i, { fromUs: numValue(e) })} disabled={busy} />
                    <span class="trigger-pm">–</span>
                    <input class="field-input narrow" type="number" min="800" max="2200" step="10"
                           value={r.toUs}
                           on:change={(e) => setSelectorRange(i, { toUs: numValue(e) })} disabled={busy} />
                    <span class="field-label">Program</span>
                    <select class="field-input" value={r.program}
                            on:change={(e) => setSelectorRange(i, { program: selValue(e) })} disabled={busy}>
                        <option value="">— pick —</option>
                        {#each activeNames as n}<option value={n}>{n}</option>{/each}
                    </select>
                    <button class="small danger btn-slot" on:click={() => removeSelectorRange(i)} disabled={busy}>× Remove</button>
                </div>
            {/each}
            <div class="form-row">
                <button class="small" on:click={onAddSelectorRange}
                        disabled={busy || cfg.activePrograms.length === 0}>+ Add range</button>
                <span class="hint">add one range per program; ranges MUST NOT overlap</span>
            </div>
            {#if selectorErrs.length > 0}
                <ul class="sel-issues">
                    {#each selectorErrs as msg}<li class="sel-issue err">⚠ {msg}</li>{/each}
                </ul>
            {/if}
        {/if}
    {/if}
</div>

<style>
    .lightfx-card { margin-bottom: 14px; }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .header-actions button { height: 28px; box-sizing: border-box; }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    .section-head .hint { font-size: 9px; font-weight: 400; text-transform: none; letter-spacing: 0; color: var(--text-dim); font-style: italic; }
    .section-head.section-error { color: var(--error); border-bottom-color: var(--error); }
    .section-err-tag { font-size: 9px; font-weight: 700; color: var(--error); padding: 1px 6px; border: 1px solid var(--error); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }
    .subsection-head { font-size: 10px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin: 10px 0 6px; padding-bottom: 3px; border-bottom: 1px dashed var(--border); display: flex; align-items: baseline; gap: 8px; }
    .subsection-head .hint { font-size: 9px; font-weight: 400; text-transform: none; letter-spacing: 0; color: var(--text-dim); font-style: italic; }
    .subsection-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
    .section-warn-tag { font-size: 9px; font-weight: 700; color: var(--warning); padding: 1px 6px; border: 1px solid var(--warning); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .unit        { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
    .trigger-pm  { margin: 0 4px; color: var(--text-dim); font-family: var(--font-mono); font-weight: 700; }
    .hint        { font-size: 10px; color: var(--text-dim); font-style: italic; }

    /* ── Active-list row ── */
    .active-row { background: var(--bg-raised); border: 1px solid var(--border); border-radius: 5px; margin-bottom: 8px; }
    .active-row.invalid { border-color: var(--error); background: rgba(255,80,80,0.05); }
    .active-head { display: flex; align-items: center; gap: 8px; padding: 6px 10px; flex-wrap: wrap; }
    .ap-idx { font-family: var(--font-mono); font-size: 11px; color: var(--text); font-weight: 600; min-width: 28px; }
    .ap-name { min-width: 160px; max-width: 280px; }
    .row-err-tag { font-size: 9px; font-weight: 700; color: var(--error); padding: 1px 6px; border: 1px solid var(--error); border-radius: 3px; }

    .active-body { padding: 4px 10px 10px; border-top: 1px dashed var(--border); }

    /* ── Channel card (was in ProgramEditorDialog) ── */
    .channel-card { background: var(--bg); border: 1px solid var(--border); border-radius: 4px; padding: 6px 8px; margin: 6px 0 8px; }
    .channel-card.invalid { border-color: var(--error); background: rgba(255,80,80,0.04); }
    .channel-head { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; flex-wrap: wrap; }
    .ch-idx { font-family: var(--font-mono); font-size: 11px; font-weight: 600; color: var(--text); min-width: 28px; }
    .ch-remove { width: 28px; padding: 0; }
    .ch-preview { height: 24px; }
    .ch-preview .oc-btn { font-size: 12px; padding: 0 8px; min-width: 28px; }
    .ch-name { min-width: 140px; max-width: 220px; }

    /* Equal-width action buttons in the active-row header — Play,
       Edit, Save As, Remove all sit on the same baseline at the same
       width so the row reads as a tidy action strip instead of a
       ragged collection of differently-sized chips.  The .op-cluster
       Play sits inside .row-preview so we constrain via its inner
       .oc-btn, while the standalone .small variants get sized
       directly.  Width was tuned for "💾 Save As…" — the widest label. */
    .row-preview { height: 26px; }
    .row-preview .oc-btn { width: 88px; height: 26px; font-size: 11px; padding: 0 8px; box-sizing: border-box; }
    .row-action          { width: 88px; height: 26px; padding: 0; box-sizing: border-box; text-align: center; }
    .preset-select { min-width: 160px; max-width: 240px; }
    .loop-toggle { display: inline-flex; align-items: center; gap: 4px; font-size: 11px; color: var(--text-dim); }
    .loop-toggle input { margin: 0; }

    .events-table { width: 100%; border-collapse: collapse; font-size: 11px; margin-top: 4px; }
    .events-table th { text-align: left; font-weight: 600; font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); padding: 2px 4px; border-bottom: 1px solid var(--border); }
    .events-table td { padding: 2px 4px; }
    .events-table .evt-idx { width: 22px; color: var(--text-dim); font-family: var(--font-mono); text-align: right; }
    .events-table .evt-rm  { width: 28px; }
    .events-table .param   { width: 70px; }
    .events-table .param.dim input { opacity: 0.3; }
    :global(.field-input.compact)     { height: 22px; padding: 0 4px; font-size: 11px; }
    :global(.field-input.compact.num) { width: 60px; text-align: right; }

    .ch-issues { list-style: none; margin: 8px 0 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
    .ch-issue.err { color: var(--error); font-size: 11px; padding-left: 4px; }

    .binding-row { background: var(--bg); padding: 4px 8px; border-radius: 3px; margin-bottom: 4px; }

    .empty-state.subtle { font-size: 11px; padding: 6px 0; color: var(--text-dim); }

    /* ── Templates picker ── */
    .add-row { gap: 6px; margin: 8px 0 6px; }
    .templates-grid {
        display: flex; flex-direction: column; gap: 6px;
        background: var(--bg); border: 1px dashed var(--border);
        border-radius: 4px; padding: 8px; margin-bottom: 8px;
    }
    .tpl-group-head { font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-bright); margin: 6px 0 2px; }
    .tpl-row { display: flex; align-items: center; gap: 6px; padding: 1px 0; }
    .tpl-pick { min-width: 180px; text-align: left; }
    .tpl-note { font-size: 10px; color: var(--text-dim); flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .tpl-del { width: 24px; padding: 0; }

    /* ── Save-As ── */
    .save-as-row { display: flex; align-items: center; gap: 6px; background: var(--bg); padding: 6px 10px; border: 1px solid var(--accent); border-radius: 4px; margin: 6px 0; }
    .save-as-err { font-size: 11px; color: var(--error); }

    /* ── Selector ── */
    .range-row { background: var(--bg-raised); padding: 4px 8px; border-radius: 3px; margin-bottom: 4px; }
    .rng-idx { font-family: var(--font-mono); font-size: 11px; color: var(--text); font-weight: 600; min-width: 28px; }
    .sel-issues { list-style: none; margin: 6px 0 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
    .sel-issue { font-size: 11px; line-height: 1.35; padding-left: 4px; }
    .sel-issue.err { color: var(--error); }

    .banner { display: flex; align-items: center; gap: 8px; }
    .banner .dismiss { background: transparent; border: 0; color: inherit; cursor: pointer; margin-left: auto; }
</style>
