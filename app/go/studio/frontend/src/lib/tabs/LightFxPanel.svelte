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
        // Phase 1 (2026-05-27) — instance channel pool + per-program-track helpers
        addChannel, removeChannel, updateChannel, setChannelPort, setChannelBrightness,
        programHasTrackFor, trackFor, setProgramChannelActive, patchProgramTrack,
        LIGHT_PRESETS, PRESET_GROUP_ORDER,
        defaultTrack, defaultEvent,
        type LightFxConfigT, type LightFxChannelT, type ProgramSelectorRangeT,
        type PresetLibraryEntryT, type ActiveProgramT,
        type ProgramT, type TrackT, type ProgramEventT, type LightEventKindT,
    } from '../lightfx'
    import { deviceModel, liveChannels, liveChannelKey, RoleKind, formatPortRail } from '../devicemodel'
    import type { PortRefT } from '../landing'
    import { landingDraft } from '../landing'
    import { freePortPoolFiltered } from '../components/port_pool'
    import { PreviewLightChannel, StopLightChannel } from '../../../wailsjs/go/main/App'
    import ChannelBandCluster from '../components/ChannelBandCluster.svelte'
    import LightTimelineTrack from '../components/LightTimelineTrack.svelte'

    let busy = false
    let error = ''

    let cfg: LightFxConfigT
    const unsubCfg = lightfxDraft.subscribe(c => { cfg = c })

    // Which active slots are EXPANDED for inline editing.  Closed by
    // default so the panel doesn't dump 5+ huge editors at once on
    // re-connect; operator clicks Edit on the rows they care about.
    // Two-column layout (2026-06-02): the LEFT column lists programs + the RC
    // selector; the RIGHT column edits the ONE selected program.  `selAi` is the
    // selected active-program index (clamped when the list shrinks).  `poolOpen`
    // toggles the compact LED-channel pool section.
    let selAi = 0
    let poolOpen = false
    $: if (cfg && selAi >= cfg.activePrograms.length) selAi = Math.max(0, cfg.activePrograms.length - 1)

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
    // Reactive closure rebuilt on every live-stream / device-model change so
    // call sites re-evaluate per frame AND on reassignment (a plain function
    // reading $liveChannels internally freezes — the gun-pill / input-bar trap).
    function makeLiveUsFor(dm: typeof $deviceModel, lc: Record<string, { us: number; valid: boolean }>) {
        return (fnId: string): { us: number; valid: boolean } | null => {
            if (!fnId) return null
            for (const inp of dm.inputs) {
                for (const c of inp.channels) {
                    if (c.function !== fnId) continue
                    const v = lc[liveChannelKey(
                        { guid: inp.port.guid, kind: 4, index: inp.port.index }, c.channel)]
                    if (!v) return { us: 1500, valid: false }
                    return { us: v.us, valid: v.valid }
                }
            }
            return null
        }
    }
    $: liveUsFor = makeLiveUsFor($deviceModel, $liveChannels)

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

    // ─── Phase 1 (2026-05-27): instance-level channel pool helpers ───
    //
    // The Channels card's port picker uses `freePortPoolFiltered` so
    // each row only sees LED-animator ports not already claimed by
    // sibling LightFx channels (and the row's own current port stays
    // visible while editing).  Channel pool emptiness shows as a
    // Rule 39 yellow warning at the section head.
    //
    // `portKeyOf` produces the dropdown <option value=> key matching
    // a PortRefT we'll round-trip through string → object on change.

    function portKeyOf(p: PortRefT | null | undefined): string {
        if (!p) return ''
        return `${p.guid}|${p.kind}|${p.idx}`
    }
    function portFromKey(key: string): PortRefT | null {
        if (!key) return null
        const [guid, kind, idxStr] = key.split('|')
        return { board: '', guid, kind, idx: Number(idxStr) || 0 }
    }
    /** Ports available to LightFx channel row `idx`: every LedAnimator
     *  output port EXCEPT those already bound to a SIBLING channel.
     *  The row's own current port stays in the pool for editing. */
    function portsForChannelRow(idx: number) {
        const mine = cfg?.channels[idx]?.port
        const sibling = new Set<string>()
        if (cfg?.channels) {
            for (let i = 0; i < cfg.channels.length; ++i) {
                if (i === idx) continue
                const p = cfg.channels[i]?.port
                if (p) sibling.add(`${p.guid}|${p.kind}|${p.idx}`)
            }
        }
        return freePortPoolFiltered(
            $deviceModel.ports,
            $deviceModel.claims,
            'pwm',
            RoleKind.LedAnimator,
            (p) => {
                const k = `${p.ref.guid}|${p.kindName}|${p.ref.index}`
                if (sibling.has(k)) return false      // exclude sibling-claimed
                if (mine && k === portKeyOf(mine)) return true   // keep own
                return true                            // include unclaimed
            },
        )
    }
    /** Validation: channel pool has issues that should surface in red.
     *  Duplicate names, missing ports (yellow not red — operator can
     *  still apply), empty names (RED — won't apply). */
    $: channelPoolErrors = computeChannelPoolErrors(cfg?.channels ?? [])
    function computeChannelPoolErrors(chs: LightFxChannelT[]): { idx: number, msg: string }[] {
        const out: { idx: number, msg: string }[] = []
        const seen = new Map<string, number>()
        for (let i = 0; i < chs.length; ++i) {
            const ch = chs[i]
            const name = (ch.name ?? '').trim()
            if (!name) {
                out.push({ idx: i, msg: 'name required' })
                continue
            }
            const dupIdx = seen.get(name)
            if (dupIdx !== undefined) {
                out.push({ idx: i, msg: `duplicate of channel #${dupIdx + 1}` })
            } else {
                seen.set(name, i)
            }
        }
        return out
    }
    function errMsgForChannel(idx: number): string {
        return channelPoolErrors.find(e => e.idx === idx)?.msg ?? ''
    }

    /** Resolve a channel name to its index in this program's tracks[]
     *  array (or -1 if the program doesn't drive that channel).
     *  Used by the new channel-row editor so existing event-table
     *  edit helpers (setEvent / removeEvent / addEvent) keep working
     *  with their (ai, ti, ei) signatures — we just derive ti from
     *  the channel name on the fly. */
    function trackIdxForCh(ai: number, channelName: string): number {
        const tracks = cfg?.activePrograms[ai]?.program.tracks
        if (!tracks) return -1
        return tracks.findIndex(t => t.channel === channelName)
    }

    /** "Ghost" tracks: in this program but referring to a channel
     *  that is NOT in the instance pool.  Surfaced in the editor so
     *  the operator can either promote the channel into the pool or
     *  drop the track.  Without this, an out-of-band edit (or a
     *  pool removal that didn't cascade for some reason) would
     *  silently strand tracks. */
    function ghostTracksFor(ai: number): { track: TrackT, idx: number }[] {
        const slot = cfg?.activePrograms[ai]
        if (!slot) return []
        const names = new Set((cfg.channels ?? []).map(c => c.name))
        const out: { track: TrackT, idx: number }[] = []
        slot.program.tracks.forEach((t, idx) => {
            if (!names.has(t.channel)) out.push({ track: t, idx })
        })
        return out
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

    // Per-event editing (kind options + field map) moved into the shared
    // LightTimelineTrack component — the panel just passes events + mutators.

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
        // Select the just-added program so its editor shows on the right.
        selAi = (cfg?.activePrograms.length ?? 1) - 1
    }
    function pickBlank() {
        addBlankActive()
        addOpen = false
        selAi = (cfg?.activePrograms.length ?? 1) - 1
    }

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    function checkedValue(e: Event): boolean { return (e.target as HTMLInputElement).checked }
    /** Svelte 4 attribute parser rejects TS `as` casts — this helper
     *  pulls the trimmed text out of the rename input.  Inline lambda
     *  in the template parses past the `as` cast as a JS comparison. */
    function strValue(e: Event): string { return (e.target as HTMLInputElement).value.trim() }
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
      <div class="lfx-cols">
        <!-- ════ LEFT: programs + selector + LED-channel pool ════ -->
        <div class="lfx-left">
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

        <!-- ─── LED channel pool (collapsible + compact, 2026-06-02) ───
             Instance-wide LED set; every program drives this same pool.
             Collapsed by default to save space — port pickers list only
             UNCLAIMED led-animator ports (freePortPoolFiltered, Rule 49). -->
        <div class="section-head pool-head" class:section-warn={cfg.channels.length === 0 && availableChannels.length === 0}>
            <button class="pool-toggle" on:click={() => poolOpen = !poolOpen}
                    title="The LED channels every program in this instance can drive.">
                {poolOpen ? '▾' : '▸'} LED channels ({cfg.channels.length})
            </button>
            {#if cfg.channels.length === 0 && availableChannels.length === 0}
                <span class="section-warn-tag">no LedAnimator ports on IO tab</span>
            {/if}
            <div class="header-actions">
                <button class="small" on:click={() => { addChannel(); poolOpen = true }} disabled={busy}
                        title="Add an LED channel (port + name); every program can drive it.">+ Add</button>
            </div>
        </div>
        {#if poolOpen}
            {#if cfg.channels.length === 0}
                <div class="empty-state subtle">No channels. Add one to bind an LED port (led-animator role) to a name like “Red beacon”.</div>
            {/if}
            {#each cfg.channels as ch, ci (ci)}
                {@const rowErr = errMsgForChannel(ci)}
                {@const portOptions = portsForChannelRow(ci)}
                {@const noPort = ch.port === null || ch.port === undefined}
                <div class="pool-row" class:verify-error={!!rowErr} class:verify-warn={noPort && !rowErr}>
                    <input class="field-input pool-name" type="text" placeholder="name"
                           bind:value={ch.name} on:change={() => updateChannel(ci, { name: ch.name })}
                           disabled={busy} title="Channel name — programs reference it." />
                    <select class="field-input pool-port" disabled={busy} value={portKeyOf(ch.port)}
                            on:change={(e) => setChannelPort(ci, portFromKey(selValue(e)))}
                            title="Unclaimed PWM port with role led-animator.">
                        <option value="">— port —</option>
                        {#each portOptions as p (p.ref.guid + ':' + p.ref.index)}
                            {@const rail = formatPortRail(p.voltageMv)}
                            <option value={portKeyOf({ board: '', guid: p.ref.guid, kind: p.kindName, idx: p.ref.index })} title={p.hardwareName}>
                                {p.hardwareName}{rail ? ` (${rail})` : ''}{p.name ? ` · ${p.name}` : ''}
                            </option>
                        {/each}
                    </select>
                    <input class="field-input pool-bright" type="number" min="0" max="100"
                           bind:value={ch.defaultBrightnessPct}
                           on:change={() => setChannelBrightness(ci, Math.max(0, Math.min(100, ch.defaultBrightnessPct | 0)))}
                           disabled={busy} title="Default brightness %" />
                    <span class="unit">%</span>
                    <button class="small danger pool-x" on:click={() => removeChannel(ci)} disabled={busy}
                            title="Remove channel (drops it from all programs)">×</button>
                    {#if rowErr}<span class="row-err">⚠ {rowErr}</span>{/if}
                    {#if noPort && !rowErr}<span class="row-warn">no port</span>{/if}
                </div>
            {/each}
        {/if}

        <!-- ─── Program selector (Rule 38, instance-level RC input) ──
             Promoted ABOVE "Active programs" 2026-05-26 (LightFx layout
             reorder).  Rationale: this is the LightFx-instance's ONE
             RC input — operator decides selector wiring before diving
             into per-program editing.  Empty-state ("none — first
             program always") handles the "no programs yet" case
             gracefully so the order is safe even on a fresh config. -->
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

        <!-- ─── Programs (selectable list) ─────────────────────────── -->
        <div class="section-head" class:section-error={cfg.activePrograms.length === 0}>
            Programs
            {#if cfg.activePrograms.length === 0}<span class="section-err-tag">none — LightFx idle</span>{/if}
        </div>
        {#if cfg.activePrograms.length === 0}
            <div class="empty-state subtle">No programs. Add a Template or + Blank below, then edit it on the right.</div>
        {/if}
        <div class="prog-list">
            {#each cfg.activePrograms as slot, ai (ai)}
                {@const errs = programErrors(slot.program)}
                {@const isPlaying = programIsPlaying(ai, playing)}
                <div class="prog-row" class:sel={selAi === ai} class:invalid={errs.length > 0}>
                    <button class="prog-pick" on:click={() => selAi = ai} title="Edit this program">
                        <span class="ap-idx">#{ai + 1}</span>
                        <span class="prog-name">{slot.name}</span>
                        {#if isPlaying}<span class="prog-live" title="previewing">●</span>{/if}
                        {#if errs.length > 0}<span class="prog-err" title={errs.join('\n')}>⚠{errs.length}</span>{/if}
                    </button>
                    <button class="small danger prog-x" on:click={() => removeActive(ai)} title="Remove program">×</button>
                </div>
            {/each}
        </div>
        <!-- add / templates / save-as -->
        <div class="form-row add-row">
            <button class="small {addOpen ? 'state-toggle state-on' : ''}" on:click={() => addOpen = !addOpen}>{addOpen ? '▾ Close' : '+ Template…'}</button>
            <button class="small" on:click={pickBlank} title="Add an empty program">+ Blank</button>
            <button class="small" on:click={() => refreshPresetLibrary()} title="Re-read templates from disk">↻</button>
        </div>
        {#if addOpen}
            <div class="templates-grid">
                {#each groupedTemplates as [cat, items]}
                    <div class="tpl-group">
                        <div class="tpl-group-head">{cat}</div>
                        {#each items as entry}
                            <div class="tpl-row">
                                <button class="small tpl-pick" on:click={() => pickTemplate(entry.name)} title={entry.note}>+ {entry.name}</button>
                                <span class="tpl-note">{entry.note.slice(0, 80)}</span>
                                {#if entry.source === 'user'}
                                    <button class="small danger tpl-del" on:click|stopPropagation={() => onDeleteTemplate(entry.name)} title="Delete this user template">×</button>
                                {/if}
                            </div>
                        {/each}
                    </div>
                {/each}
                {#if groupedTemplates.length === 0}
                    <div class="empty-state subtle">Every template is already active — remove one or click + Blank.</div>
                {/if}
            </div>
        {/if}
        {#if saveAsFor !== null}
            <div class="save-as-row">
                <span class="field-label">Save as</span>
                <input class="field-input wide" type="text" bind:value={saveAsName}
                       on:keydown={(e) => { if (e.key === 'Enter') confirmSaveAs(); if (e.key === 'Escape') closeSaveAs() }} />
                <button class="small primary" on:click={confirmSaveAs}>Save</button>
                <button class="small" on:click={closeSaveAs}>Cancel</button>
                {#if saveAsError}<span class="save-as-err">⚠ {saveAsError}</span>{/if}
            </div>
        {/if}
        </div><!-- /.lfx-left -->

        <!-- ════ RIGHT: selected program editor ════ -->
        <div class="lfx-right">
        {#if cfg.activePrograms.length === 0}
            <div class="empty-state">Add a program on the left to start editing its channels and events.</div>
        {:else if cfg.activePrograms[selAi]}
            {@const slot      = cfg.activePrograms[selAi]}
            {@const ai        = selAi}
            {@const errs      = programErrors(slot.program)}
            {@const isPlaying = programIsPlaying(ai, playing)}
            {@const canPlay   = programHasPlayable(slot)}
            <div class="prog-editor-head">
                <span class="ap-idx">#{ai + 1}</span>
                <input class="field-input ap-name" type="text" value={slot.name}
                       on:change={(e) => renameActive(ai, strValue(e))}
                       title="Rename — becomes /lightfx/programs/<name>.yaml on Apply" />
                {#if errs.length > 0}
                    <span class="row-err-tag" title={errs.join('\n')}>⚠ {errs.length} error{errs.length > 1 ? 's' : ''}</span>
                {/if}
                <button class="small state-toggle row-action" class:danger={isPlaying}
                        on:click={() => isPlaying ? stopProgram(ai) : playProgram(ai)}
                        disabled={isPlaying ? false : !canPlay}
                        title={isPlaying ? 'Stop every channel in this program'
                             : canPlay ? 'Preview every channel that has a port + events. No upload needed.'
                             : 'No playable channels — assign a port and at least one event first.'}>
                    {isPlaying ? 'Stop' : 'Preview'}
                </button>
                <button class="small row-action" on:click={() => openSaveAs(ai)}
                        title="Save this draft as a reusable user template">💾 Save As…</button>
                <button class="small danger row-action" on:click={() => removeActive(ai)}
                        title="Remove this program">× Remove</button>
            </div>

            <!-- ─── Selected-program editor: per-channel timeline tracks ─── -->
            <div class="active-body">
                        <div class="subsection-head" class:section-warn={cfg.channels.length === 0}>
                            Channels in this program
                            <span class="hint">tick to drive · brightness overrides channel default · events list per channel</span>
                            {#if cfg.channels.length === 0}
                                <span class="section-warn-tag"
                                      title="The LightFx instance has no channels in its pool yet. Add at least one channel in the Channels card above.">
                                    no channels in pool — add one above
                                </span>
                            {/if}
                        </div>

                        {#each cfg.channels as ch, ci (ci)}
                            {@const tIdx   = trackIdxForCh(ai, ch.name)}
                            {@const t      = tIdx >= 0 ? slot.program.tracks[tIdx] : null}
                            {@const active = t !== null}
                            {@const noPort = ch.port === null}
                            <div class="channel-card" class:dim={!active} class:verify-warn={active && noPort}>
                                <div class="channel-head">
                                    <span class="ch-idx">#{ci + 1}</span>
                                    <label class="active-toggle"
                                           title={active
                                               ? 'Uncheck to mute this channel for this program (channel stays in pool, events for THIS program discarded).'
                                               : 'Check to drive this channel with this program.  A blank events list is seeded — replace via the Preset dropdown or add events.'}>
                                        <input type="checkbox" checked={active}
                                               on:change={(e) => setProgramChannelActive(ai, ch.name, checkedValue(e))} />
                                        active
                                    </label>
                                    <span class="ch-name" class:muted={!active}>{ch.name}</span>
                                    {#if noPort && active}
                                        <span class="row-warn" title="This channel has no physical port assigned in the Channels card. Program will play but nothing will light.">port unassigned</span>
                                    {/if}
                                    {#if active && t !== null}
                                        <span class="field-label">brightness</span>
                                        <input class="field-input narrow" type="number" min="0" max="100"
                                               value={t.brightnessPct}
                                               on:change={(e) => patchProgramTrack(ai, ch.name, { brightnessPct: Math.max(0, Math.min(100, numValue(e) | 0)) })}
                                               title={`Override channel default (${ch.defaultBrightnessPct} %) for this program only.  Leave at the default to inherit.`} />
                                        <span class="unit">%</span>
                                        {#if t.brightnessPct === ch.defaultBrightnessPct}
                                            <span class="hint inline">(default)</span>
                                        {/if}
                                        <label class="loop-toggle"
                                               title="Phase-locked repeating pattern — the events list loops as a cycle (period = sum of all event durations).  Sibling tracks with the same period stay in lock-step.">
                                            <input type="checkbox" checked={t.loop}
                                                   on:change={(e) => patchProgramTrack(ai, ch.name, { loop: checkedValue(e) })} />
                                            loop
                                        </label>
                                        <select class="field-input preset-select"
                                                on:change={(e) => onPickChannelPreset(ai, tIdx, e)}
                                                title="Replace this channel's events with a canonical pattern (FAA strobe, beacon, nav, …).">
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
                                            {#if playing[playKey(ai, tIdx)] !== undefined}
                                                <button class="oc-btn oc-danger"
                                                        on:click={() => stopTrack(ai, tIdx)} title="Stop this channel preview">■</button>
                                            {:else}
                                                <button class="oc-btn oc-primary"
                                                        on:click={() => playTrack(ai, tIdx)}
                                                        disabled={t.events.length === 0 || noPort}
                                                        title={noPort
                                                            ? 'Assign a port to this channel in the Channels card first'
                                                            : t.events.length === 0
                                                                ? 'No events to play'
                                                                : 'Preview this channel — no save required'}>▶</button>
                                            {/if}
                                        </div>
                                    {/if}
                                </div>

                                {#if active && t !== null}
                                    <LightTimelineTrack
                                        events={t.events}
                                        loop={t.loop}
                                        onSet={(ei, patch) => setEvent(ai, tIdx, ei, patch)}
                                        onAdd={() => addEvent(ai, tIdx)}
                                        onRemove={(ei) => removeEvent(ai, tIdx, ei)} />
                                {/if}
                            </div>
                        {/each}

                        <!-- Ghost tracks: program drives a channel that
                             isn't in the pool.  Surface so the operator
                             can either promote (add to pool) or drop. -->
                        {#each ghostTracksFor(ai) as gt (gt.idx)}
                            <div class="channel-card verify-warn ghost-track">
                                <div class="channel-head">
                                    <span class="ch-name">⚠ {gt.track.channel}</span>
                                    <span class="row-warn">
                                        track references a channel not in the LightFx pool
                                    </span>
                                    <button class="small" on:click={() => addChannel(gt.track.channel)}
                                            title="Add this channel name to the instance pool (you'll need to pick a port in the Channels card).">
                                        + Add to pool
                                    </button>
                                    <button class="small danger" on:click={() => removeTrack(ai, gt.idx)}
                                            title="Drop this track from the program.">
                                        × Drop track
                                    </button>
                                </div>
                            </div>
                        {/each}

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
            </div><!-- /.active-body -->
        {/if}
        </div><!-- /.lfx-right -->
      </div><!-- /.lfx-cols -->
    {/if}
</div>

<style>
    .lightfx-card { margin-bottom: 14px; }

    /* ── Two-column layout (2026-06-02): left = programs + selector + pool;
          right = selected-program editor. ── */
    .lfx-cols { display: flex; gap: 16px; align-items: flex-start; }
    .lfx-left { flex: 0 0 340px; min-width: 0; }
    .lfx-right { flex: 1; min-width: 0; border-left: 1px solid var(--border); padding-left: 16px; }

    /* Programs list (left) */
    .prog-list { display: flex; flex-direction: column; gap: 3px; margin-bottom: 6px; }
    .prog-row { display: flex; align-items: stretch; gap: 4px; }
    .prog-pick { flex: 1; min-width: 0; display: flex; align-items: center; gap: 6px;
                 text-align: left; padding: 4px 8px; font-size: 12px;
                 background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px; cursor: pointer; }
    .prog-pick:hover { border-color: var(--accent); }
    .prog-row.sel .prog-pick { background: color-mix(in srgb, var(--accent) 18%, var(--bg-input)); border-color: var(--accent); }
    .prog-row.invalid .prog-pick { border-left: 3px solid var(--error); }
    .prog-name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--text-bright); }
    .prog-live { color: var(--success); font-size: 10px; }
    .prog-err { color: var(--error); font-size: 10px; font-family: var(--font-mono); }
    .prog-x { flex: 0 0 26px; width: 26px; padding: 0; }

    /* Selected-program editor header (right) */
    .prog-editor-head { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin-bottom: 10px;
                        padding-bottom: 8px; border-bottom: 1px solid var(--border); }
    .prog-editor-head .ap-name { flex: 1; min-width: 140px; font-weight: 600; }

    /* Collapsible compact LED-channel pool (left) */
    .pool-head { cursor: default; }
    .pool-toggle { background: none; border: none; padding: 0; font: inherit; color: var(--text-bright);
                   text-transform: uppercase; letter-spacing: 0.6px; font-weight: 700; font-size: 11px; cursor: pointer; }
    .pool-toggle:hover { color: var(--accent); }
    .pool-row { display: flex; align-items: center; gap: 4px; margin: 2px 0; }
    .pool-row.verify-error { background: color-mix(in srgb, var(--error) 8%, transparent); }
    .pool-name { flex: 1; min-width: 0; }
    .pool-port { flex: 1.4; min-width: 0; }
    .pool-bright { width: 52px; flex: 0 0 52px; }
    .pool-x { width: 24px; flex: 0 0 24px; padding: 0; }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .header-actions button { height: 28px; box-sizing: border-box; }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    /* `.section-head .hint` reset lives in global style.css (Rule 50). */
    .section-head.section-error { color: var(--error); border-bottom-color: var(--error); }
    .section-err-tag { font-size: 9px; font-weight: 700; color: var(--error); padding: 1px 6px; border: 1px solid var(--error); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }
    .subsection-head { font-size: 10px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin: 10px 0 6px; padding-bottom: 3px; border-bottom: 1px dashed var(--border); display: flex; align-items: baseline; gap: 8px; }
    /* `.subsection-head .hint` reset lives in global style.css (Rule 50). */
    .subsection-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
    .section-warn-tag { font-size: 9px; font-weight: 700; color: var(--warning); padding: 1px 6px; border: 1px solid var(--warning); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }

    /* `.unit`, `.hint` / `.hint.compact/warn/err`, `.help-text` live in
       global style.css (Rule 50 typography).  `.field-label` keeps the
       10 px local override — dense panel choice. */
    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .trigger-pm  { margin: 0 4px; color: var(--text-dim); font-family: var(--font-mono); font-weight: 700; }

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
