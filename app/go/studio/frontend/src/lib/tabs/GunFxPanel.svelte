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
        gunStartFiringWithRof,
        gunStopFiring, gunSmokeArm,
        gunVerboseSubscribe, installVerboseListener, uninstallVerboseListener,
        detectBandOverlaps,
        type GunT, type GunFxConfigT, type RofItemT, type PortRefT,
    } from '../gunfx'
    import ServoWidget from '../components/ServoWidget.svelte'
    // SetPortProfile + markHubDirty now flow through ServoCalibrationDialog;
    // GunFxPanel only READS the device-model profile to feed ServoWidget.
    import {
        deviceModel, type Port, formatPortRail,
        liveChannels, liveChannelKey, usToPct,
        RoleKind, claimsForPort,
    } from '../devicemodel'
    import { effectClaims } from '../effect-claims'
    import { pickFile } from '../filepicker'
    import SoundRow from '../components/SoundRow.svelte'
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'
    import ChannelBandCluster from '../components/ChannelBandCluster.svelte'

    // BandItem mirrors the shape declared inside ChannelBandCluster.
    // Inlined here because Svelte 4 doesn't cleanly export named types
    // from .svelte files — a structural match is enough.
    type BandItem = {
        loUs: number; hiUs: number; name: string;
        meta?: string; color: string; armed: boolean
    }

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

    // Per-gun "test ROF" dropdown selection — drives Fire / Auto-Fire
    // buttons in the status row.  0xFF (255) means "use the firmware's
    // currently-armed ROF" (the band the RC selector is in).  Any
    // other value forces the firmware to pull THIS shot from
    // `cfg.guns[g].rof.items[selectedRofByGun[g.id]]`, bypassing the
    // selector channel — fixes the operator-reported bug where Fire
    // does nothing when an RC selector channel is bound but the
    // stick sits between bands.  Per-gun map so two guns can be
    // tested with different programs independently.
    const ROF_ARMED: number = 0xFF
    let selectedRofByGun: Record<number, number> = {}
    function pickRofForGun(g: GunT): number {
        const cur = selectedRofByGun[g.id]
        if (cur === undefined || cur === null) return ROF_ARMED
        if (cur === ROF_ARMED) return ROF_ARMED
        if (cur < 0 || cur >= g.rof.items.length) return ROF_ARMED
        return cur
    }
    function setRofPick(gunId: number, val: number) {
        selectedRofByGun = { ...selectedRofByGun, [gunId]: val }
    }

    let cfg: GunFxConfigT
    const unsub = gunfxDraft.subscribe(c => { cfg = c })

    // Rule 46: panel doesn't register with the dirty-registry — the
    // domain module (gunfx.ts) owns `gunfxConfigSource` (incl. its
    // own `gunfxHasErrors` derived store covering ROF band overlaps)
    // and App.svelte registers it once at startup.
    // Live status — verbose broadcast subscription (Phase 4d).  The
    // firmware streams GUN_VERBOSE_STATUS at ~10 Hz once subscribed,
    // landing in the Wails `gun:verbose` event → `gunfxVerbose` store
    // (id → state).  The old 1 Hz polling path is kept ONLY as the
    // initial seeder so the firing pill renders before the first
    // broadcast arrives (~100 ms) — every later update flows through
    // the broadcast, so the operator sees firing / smoke / fan state
    // change live without hitting Refresh.
    let subscribedIds = new Set<number>()
    function syncVerboseSubscriptions(ids: number[]): void {
        const wanted = new Set(ids)
        // Subscribe to any gun the panel now displays
        for (const id of wanted) {
            if (!subscribedIds.has(id)) {
                gunVerboseSubscribe(id, true).catch(() => {})
                subscribedIds.add(id)
            }
        }
        // Unsubscribe from guns that disappeared (e.g. operator
        // removed one) — without this the firmware keeps broadcasting
        // for ghost ids that nothing on the GUI reads.
        for (const id of [...subscribedIds]) {
            if (!wanted.has(id)) {
                gunVerboseSubscribe(id, false).catch(() => {})
                subscribedIds.delete(id)
            }
        }
    }
    // Re-sync when the configured gun list changes (add / remove / id renumber).
    $: if (cfg) syncVerboseSubscriptions(cfg.guns.map(g => g.id))

    onMount(() => {
        installVerboseListener()
        loadGunFxConfig().catch(e => { error = String(e) })
        // One-shot refresh so the firing pill is populated immediately;
        // verbose broadcasts take over from there.
        refreshGunFxStatus().catch(() => {})
        return () => {
            // Unsubscribe every gun we were tailing so the firmware
            // stops broadcasting (saves cycles + bus bandwidth when
            // the operator switches tabs).  Leave the listener alive
            // — it's a global Wails handler, cheap to keep wired.
            for (const id of subscribedIds) {
                gunVerboseSubscribe(id, false).catch(() => {})
            }
            subscribedIds.clear()
            unsub()
        }
    })
    onDestroy(() => {
        for (const id of subscribedIds) {
            gunVerboseSubscribe(id, false).catch(() => {})
        }
        subscribedIds.clear()
        unsub()
    })

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
    // Prefer the verbose broadcast (10 Hz, no polling) when it has
    // landed for this gun; fall back to the polled snapshot for the
    // first ~100 ms after panel mount before the verbose stream warms
    // up.  Returns a uniform shape so consumers don't have to branch.
    function statusFor(id: number): { firing: boolean; smokeArmed: boolean } | undefined {
        const v = $gunfxVerbose[id]
        if (v) return { firing: v.firing, smokeArmed: v.smokeArmed }
        const s = $gunfxStatus.find(x => x.id === id)
        return s ? { firing: s.firing, smokeArmed: s.smokeArmed } : undefined
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

    // Speaker-routing helpers — see the top-level import block.
    function setMuzzleField(id: number, key: keyof GunT['muzzleFlash'], val: any) {
        updateGun(id, g => ({ ...g, muzzleFlash: { ...g.muzzleFlash, [key]: val } }))
    }
    function setRecoilField(id: number, key: keyof GunT['recoil'], val: any) {
        updateGun(id, g => ({ ...g, recoil: { ...g.recoil, [key]: val } }))
    }
    function setHeaterField(id: number, key: keyof GunT['smoke']['heater'], val: any) {
        updateGun(id, g => ({ ...g, smoke: { ...g.smoke, heater: { ...g.smoke.heater, [key]: val } } }))
    }
    // Rule 43 activation block lives one level deeper than the rest of
    // the heater fields.  Separate mutator keeps the call sites readable
    // (`setHeaterActivationField(id, 'thresholdUs', 1500)`).
    function setHeaterActivationField(id: number,
                                      key: keyof GunT['smoke']['heater']['activation'],
                                      val: any) {
        updateGun(id, g => ({
            ...g,
            smoke: {
                ...g.smoke,
                heater: {
                    ...g.smoke.heater,
                    activation: { ...g.smoke.heater.activation, [key]: val },
                },
            },
        }))
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
        // Show the operator-assigned alias (IO tab "name" field) FIRST
        // when set — that's the label the operator actually thinks in.
        // Falls back to the silkscreen "hardwareName" (CH3, IN1, …)
        // when no alias is configured.  Reactive on $deviceModel so
        // renaming a port on the IO tab propagates through every
        // picker (muzzle flash, recoil servo, smoke heater, …)
        // without the operator having to reload the GunFx tab.
        const rail  = formatPortRail(p.voltageMv)
        const alias = p.name && p.name.trim()
        const hw    = p.hardwareName
        const head  = alias ? `${alias} (${hw})` : hw
        return `${p.boardName ?? 'Hub'} · ${head}${rail ? ` · ${rail}` : ''}`
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
    // lives in /hubfx.yaml's ports[] block (canonical store).  Reading
    // is via $deviceModel.ports[i].profile (overlay); writing is via
    // the ServoCalibrationDialog (popup) which calls SetPortProfile +
    // markHubDirty internally.  The Phase-1 inline schedulePushProfile
    // helper was retired when the dialog replaced ServoProfileEditor
    // (Rule 44 refresh, 2026-05-24).
    type ProfileT = { minUs: number; maxUs: number; centerUs: number; reversed: boolean; maxSpeedUsPerSec: number; maxAccelUsPerSec2: number; maxJerkUsPerSec3: number }
    function profileForPort(port: PortRefT): ProfileT | null {
        if (!port || !port.kind) return null
        const k = `${port.guid}|${port.kind}|${port.idx}`
        for (const p of $deviceModel.ports) {
            if (`${p.ref.guid}|${p.kindName}|${p.ref.index}` === k && p.profile) {
                return { ...p.profile }
            }
        }
        return null
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
    /** Build the BandItem[] for ChannelBandCluster.  Kept as a function
     *  rather than an inline `{@const}` map because Svelte's mustache
     *  parser rejects TypeScript type annotations on arrow params. */
    function buildRofBands(items: RofItemT[], liveUs: number | null, liveValid: boolean): BandItem[] {
        return items.map((it, i) => ({
            loUs: it.bandLoUs,
            hiUs: it.bandHiUs,
            name: it.name || `rof${i + 1}`,
            meta: `${it.rpm} rpm`,
            color: bandPalette(i),
            armed: liveValid && liveUs != null
                && (it.bandLoUs === 0 || liveUs >= it.bandLoUs)
                && (it.bandHiUs === 0 || liveUs <= it.bandHiUs),
        }))
    }

    // ── Per-ROF-row validation (Rule 35) ─────────────────────────────
    // Each row exposes its issues as a short list of short strings;
    // the panel renders them under the row + flags the row with
    // class `invalid` so the design-system pink-border kicks in.
    // Section-level error chips aggregate the per-row state.
    type RofRowIssue = { sev: 'err' | 'warn'; text: string }
    function rofItemIssues(items: RofItemT[], idx: number): RofRowIssue[] {
        const it = items[idx]
        const out: RofRowIssue[] = []
        // 1. Overlap with a sibling — hard error.  Detected once at the
        //    panel level (via detectBandOverlaps) so this row inherits
        //    the verdict instead of recomputing.
        if (items.length > 1 && detectBandOverlaps(items).includes(idx)) {
            out.push({ sev: 'err',
                text: 'Band overlaps another item — the selector arms two ROFs at once; tighten the range.' })
        }
        // 2. Inverted band (hi <= lo) when both are explicit — operator
        //    typo; renders as a zero-width zone on the bar, never arms.
        if (it.bandLoUs > 0 && it.bandHiUs > 0 && it.bandHiUs <= it.bandLoUs) {
            out.push({ sev: 'err',
                text: `Band high (${it.bandHiUs} µs) must be greater than low (${it.bandLoUs} µs).` })
        }
        // 3. Unbounded band [0,0] when other items have explicit bands
        //    — usually a leftover default from + Add ROF; harmless if
        //    intentional (catch-all), warning rather than error.
        const otherIdxWithExplicit = items.some((o, i) => i !== idx
            && (o.bandLoUs > 0 || o.bandHiUs > 0))
        if (it.bandLoUs === 0 && it.bandHiUs === 0 && items.length > 1 && otherIdxWithExplicit) {
            out.push({ sev: 'warn',
                text: 'Both band limits are 0 — this ROF catches every pulse (only the FIRST such item ever arms; later catch-alls never fire).' })
        }
        // 4. RPM out of plausible range — soft warning, fired weapons
        //    don't realistically run >3000 rpm, and 0 rpm is "single shot
        //    per trigger event" which is rare-but-valid; fire a warn at
        //    the extremes so the operator can confirm intent.
        if (it.rpm < 0 || it.rpm > 3000) {
            out.push({ sev: 'err',
                text: `RPM out of range (0–3000); ${it.rpm} would either lock the loop or run at the 50 ms hard cap.` })
        }
        // 5. Empty name — cosmetic but the bar overlay falls back to
        //    "rofN" which is harder to scan than a real label.
        if (!it.name || !it.name.trim()) {
            out.push({ sev: 'warn',
                text: 'Item has no name — the bar overlay will fall back to a generic rofN label.' })
        }
        return out
    }
    function rofItemHasErrors(items: RofItemT[], idx: number): boolean {
        return rofItemIssues(items, idx).some(x => x.sev === 'err')
    }
    function rofGroupErrorCount(items: RofItemT[]): number {
        let n = 0
        for (let i = 0; i < items.length; ++i) if (rofItemHasErrors(items, i)) n++
        return n
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
            // Exclude ports claimed by ANY other effect — hard claims
            // (Domains tab) AND soft claims synthesized from sibling
            // effect drafts (LightFx programs, Landing servos/LEDs,
            // OTHER guns in this draft).  $effectClaims is the merged
            // view; the `otherKeys.has(k)` check above already covers
            // intra-gun overlap so we just exclude the cross-effect
            // half here.  See lib/effect-claims.ts.
            if (claimsForPort($effectClaims, p.ref).filter(c => c.domain !== 'gunfx').length > 0) return false
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
        <!-- Two-column layout: each gun flows as [gun-core | smoke].
             The {#each} renders exactly two sibling cards per gun
             (.gun-card then .smoke-card; the {@const} between them emit
             no DOM), so a 1fr/1fr grid auto-flows them into per-gun
             rows.  Needs the full-width GunFX tab to breathe. -->
        <div class="guns-grid">
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
                            <span class="state-pill" class:smoke={st.smokeArmed}>{st.smokeArmed ? 'smoke on' : 'smoke off'}</span>
                        {/if}
                        <!-- Single Fire/Stop/Picker cluster (Rule 35
                             follow-on, 2026-05-24).  "Fire" starts
                             auto-fire at the picked ROF's RPM —
                             single-shot was dropped on operator
                             request (one button beats two).  Stop
                             sits in the SAME bar so the emergency
                             cutoff is right next to the action that
                             produced it; Stop is always enabled (no
                             dirty / errors gate) — it's the safety
                             switch.  Picker stays narrow (RC / #N);
                             full program name + rpm live in option
                             tooltips so the closed state is tight. -->
                        <div class="op-cluster">
                            <button class="oc-btn oc-primary"
                                    on:click={() => gunStartFiringWithRof(gun.id, 0, pickRofForGun(gun))}
                                    disabled={busy || $gunfxDirty || gun.rof.items.length === 0}
                                    title={$gunfxDirty ? 'Apply changes before firing — tests the loaded firmware config' : 'Start auto-fire at the picked ROF (or RC-armed)'}>▶ Fire</button>
                            <select class="oc-picker"
                                    value={pickRofForGun(gun)}
                                    on:change={(e) => setRofPick(gun.id, Number(selValue(e)))}
                                    disabled={busy || gun.rof.items.length === 0}
                                    title="Test-fire program — overrides the RC selector channel for the next Fire press.&#10;'RC' uses whichever ROF the selector stick has currently armed.&#10;Currently: {pickRofForGun(gun) === ROF_ARMED ? 'RC-armed' : `#${pickRofForGun(gun) + 1} ${gun.rof.items[pickRofForGun(gun)]?.name || ''} · ${gun.rof.items[pickRofForGun(gun)]?.rpm || 0} rpm`}">
                                <option value={ROF_ARMED} title="Use whichever ROF the RC selector stick currently arms">RC</option>
                                {#each gun.rof.items as item, i}
                                    <option value={i} title="{item.name || `rof${i + 1}`} · {item.rpm} rpm">#{i + 1}</option>
                                {/each}
                            </select>
                            <button class="oc-btn oc-danger"
                                    on:click={() => gunStopFiring(gun.id)}
                                    disabled={busy}
                                    title="Stop auto-fire — always enabled (emergency cutoff, no dirty gate)">■ Stop</button>
                        </div>
                        <!-- Smoke On/Off button removed 2026-05-26 — it
                             lives in the per-gun "Smoke generator" sibling
                             card below as the simulate cluster.  Gun-header
                             only carries firing controls + the destructive
                             Remove button now. -->
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

                <!-- TRIGGER ─ shared ChannelToggleCluster widget
                     (Rule 36).  Drives the gun unit's TriggerInput
                     boolean dispatch — same widget the engine on/off
                     uses, just bound to gun.trigger.* instead of
                     cfg.toggle.*.  Rule 43 named-channel sourcing. -->
                <div class="section-head">Trigger (fire on/off)
                    <span class="hint">named channel from the IO tab's <b>inputs[]</b> block</span>
                </div>
                <ChannelToggleCluster
                    channelLabel="Channel"
                    emptyOption="— none (manual only) —"
                    options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
                    inputId={gun.trigger.input}
                    thresholdUs={gun.trigger.thresholdUs}
                    hysteresisUs={gun.trigger.hysteresisUs}
                    liveUs={liveUsFor(gun.trigger.input)?.us ?? null}
                    liveValid={liveUsFor(gun.trigger.input)?.valid ?? false}
                    busy={busy}
                    actionVerb="Fires"
                    onChange={(n) => {
                        setTriggerField(gun.id, 'input', n.inputId)
                        setTriggerField(gun.id, 'thresholdUs', n.thresholdUs)
                        setTriggerField(gun.id, 'hysteresisUs', n.hysteresisUs)
                    }} />

                <!-- ROF ─ Rule 43: pick from named channels ─────────────
                     Rule 35: red header when ANY blocker exists:
                       - enabled gun with zero items (unfireable)
                       - bands overlap between items
                       - any per-row hard error (inverted band, RPM out
                         of range, etc.) — counted via rofGroupErrorCount.
                     Yellow soft-warn header in the catch-all case (one
                     [0,0] item among siblings — Rule 39 / non-blocking).
                     Inline calls (no @const) — Svelte 4 only accepts
                     @const as the immediate child of {#if}/{#each}/<Component>. -->
                <div class="section-head"
                     class:section-warn-err={(cfg?.enabled && gun.rof.items.length === 0)
                                              || (gun.rof.items.length > 1 && detectBandOverlaps(gun.rof.items).length > 0)
                                              || rofGroupErrorCount(gun.rof.items) > 0}>
                    Rate of fire
                    {#if cfg?.enabled && gun.rof.items.length === 0}
                        <span class="section-warn-err-tag" title="An enabled gun must have at least one ROF item — the gun has nothing to fire otherwise.">
                            ⚠ no ROF items — gun cannot fire
                        </span>
                    {:else if gun.rof.items.length > 1 && detectBandOverlaps(gun.rof.items).length > 0}
                        <span class="section-warn-err-tag" title="Two or more bands intersect — the selector channel falling in the overlap arms multiple items at once.">
                            ⚠ bands overlap
                        </span>
                    {:else if rofGroupErrorCount(gun.rof.items) > 0}
                        {@const rofRowErrs = rofGroupErrorCount(gun.rof.items)}
                        <span class="section-warn-err-tag" title="One or more items have field-level errors (inverted band, RPM out of range, …).">
                            ⚠ {rofRowErrs} item{rofRowErrs > 1 ? 's' : ''} need{rofRowErrs > 1 ? '' : 's'} attention
                        </span>
                    {/if}
                    <span class="hint">selector channel + multi-band item table — operator stick picks the armed item</span>
                </div>
                <!-- Shared ChannelBandCluster (Rule 36 + 38).  Owns the
                     selector dropdown + the multi-band live bar + the
                     overlap-error overlay + the legend.  Bands are
                     reverse-painted inside the widget so a [0,0]
                     unbounded item #1 always shows up at the leading
                     edge instead of being hidden behind a sibling.
                     The {@const} chain lives inside this <Component>
                     wrapper because Svelte 4 only accepts @const as
                     an immediate child of {#if}/{#each}/<Component>. -->
                <ChannelBandCluster
                    channelLabel="Selector channel"
                    emptyOption="— none (use item #1 always) —"
                    options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
                    inputId={gun.rof.input}
                    bands={buildRofBands(gun.rof.items,
                        liveUsFor(gun.rof.input)?.us ?? null,
                        liveUsFor(gun.rof.input)?.valid ?? false)}
                    overlapIndices={detectBandOverlaps(gun.rof.items)}
                    liveUs={liveUsFor(gun.rof.input)?.us ?? null}
                    liveValid={liveUsFor(gun.rof.input)?.valid ?? false}
                    busy={busy}
                    onInputChange={(v) => setRofField(gun.id, 'input', v)} />
                {#if detectBandOverlaps(gun.rof.items).length > 0}
                    {@const _overlaps = detectBandOverlaps(gun.rof.items)}
                    <div class="row-err">⚠ ROF bands overlap (items #{_overlaps.map(i => i + 1).join(', #')}) — the operator stick crossing the overlap will jitter between items; tighten the bands or re-order so they don't intersect.</div>
                {/if}

                {#each gun.rof.items as item, i (i)}
                    {@const rowIssues = rofItemIssues(gun.rof.items, i)}
                    {@const rowErr = rowIssues.some(x => x.sev === 'err')}
                    {@const rowWarn = !rowErr && rowIssues.some(x => x.sev === 'warn')}
                    <div class="rof-item" class:invalid={rowErr} class:soft-warn={rowWarn}
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
                        <!-- Row 2 — shared SoundRow component (matches
                             EnginePanel's sound-row layout exactly).  The
                             leading slot carries an empty rof-idx-pill so
                             this row column-aligns with the #N badge on
                             Row 1 above. -->
                        <SoundRow
                            label="sound"
                            placeholder="/sounds/gun/burst.wav  (optional)"
                            value={item.soundPath}
                            outputMask={item.outputMask || 3}
                            busy={busy}
                            onPathChange={(v) => setRofItem(gun.id, i, 'soundPath', v)}
                            onMaskChange={(m) => setRofItem(gun.id, i, 'outputMask', m)}
                            onBrowse={() => browseRofSound(gun.id, i)}
                            onClear={() => clearRofSound(gun.id, i)}>
                            <span slot="lead" class="rof-idx-pill placeholder" aria-hidden="true"></span>
                        </SoundRow>
                        <!-- Per-row issues — verb-led explanations so the
                             operator knows WHAT to fix without scrolling
                             back to the section header.  Mixed sev list:
                             err rows render red, warn rows amber. -->
                        {#if rowIssues.length > 0}
                            <ul class="rof-issues">
                                {#each rowIssues as iss}
                                    <li class="rof-issue" class:err={iss.sev === 'err'} class:warn={iss.sev === 'warn'}>
                                        {iss.sev === 'err' ? '⚠' : '⚐'} {iss.text}
                                    </li>
                                {/each}
                            </ul>
                        {/if}
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

                            <!-- Rule 44: per-servo profile editing,
                                 popup-style (2026-05-24).  The inline
                                 ServoProfileEditor was retired — the
                                 form row stays compact; ServoWidget
                                 carries a one-click ↔ Reversed toggle
                                 and a ⚙ Calibrate… button that opens
                                 the live-jog popup
                                 (ServoCalibrationDialog).  Save in the
                                 popup persists via SetPortProfile (live
                                 push + /hubfx.yaml dirty) — same wire
                                 path as before. -->
                            <div class="form-row">
                                <span class="field-label">Motion profile</span>
                                <ServoWidget
                                    port={axis.servoPort}
                                    portLabel="{which.charAt(0).toUpperCase() + which.slice(1)} servo"
                                    profile={profileForPort(axis.servoPort)}
                                    busy={busy} />
                            </div>
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

                <!-- Smoke generator moved to its own sibling card below
                     (Phase 4 polish 2026-05-26).  Rationale: smoke is a
                     conceptually independent subsystem with its own
                     simulate path + activation gate — giving it its own
                     card surface lets the operator scan / configure it
                     without competing for visual real estate with the
                     gun's firing controls. -->

                <!-- Yaw / pitch are now inside the Turret control section above. -->

                <!-- Manual / puppet panel removed (Phase 4 polish 2026-05-23):
                     redundant with the per-gun test row in the card
                     header (▶ Fire / ▶▶ Auto / ■ Stop / smoke toggle).
                     If a live verbose-status mirror is ever needed
                     again it should be its own debug overlay, not part
                     of the configuration flow. -->
            </div>

            <!-- ═══ GUN SMOKE · sibling card per gun ═══════════════════════
                 Lives directly after the gun card so the gun ↔ smoke
                 pair stays visually grouped (Phase 4 polish 2026-05-26).
                 Named "Gun smoke" explicitly to differentiate from a
                 possible future "Engine smoke" subsystem.

                 Header carries:
                   - title + activation gate channel reading + status pills
                   - Simulate op-cluster (▶ ON / ■ OFF) — mirrors the
                     gun-card header pattern (Rule 48).
                 Body carries:
                   1. Activation channel cluster (Rule 43 — RC gate)
                   2. Heater (port + element_mv)
                   3. Fan    (port + element_mv + mode + puff_ms)

                 No heater mode / target / hysteresis fields — HubFX has
                 no temp sensor wired to the smoke heater, so the role's
                 open-loop path (drive at scaleDuty result) fully
                 describes the behaviour.

                 `verb` reactive declares the live mirror state from the
                 GUN_VERBOSE_STATUS broadcast: smokeArmed, heaterActive
                 (Rule 43 gate), smokeFanRunning.  Pills + the activation
                 µs readout in the header refresh at the verbose-status
                 cadence (10 Hz). -->
            {@const verb         = $gunfxVerbose[gun.id]}
            {@const armed        = verb?.smokeArmed ?? statusFor(gun.id)?.smokeArmed ?? false}
            {@const heating      = (verb?.heaterDutyPct ?? 0) > 0}
            {@const gatedOff     = armed && !heating && gun.smoke.heater.activation.input !== ''}
            {@const fanRunning   = verb?.smokeFanRunning ?? false}
            {@const actLive      = liveUsFor(gun.smoke.heater.activation.input)}
            <div class="card smoke-card">
                <div class="card-header inner">
                    <h4>Gun smoke · {gun.id}{gun.name ? ` · ${gun.name}` : ''}</h4>
                    <div class="header-actions">
                        <!-- Live status pills (Phase 4 polish 2026-05-26).
                             Render priority — "heating" beats "armed"
                             alone (the heater drawing wins the visual
                             over the bare arm state); "gated off" reads
                             when the operator armed smoke but the
                             activation channel is below threshold. -->
                        {#if heating}
                            <span class="state-pill smoke heating" title="Heater is being driven at element-scaled duty">🔥 heating</span>
                        {:else if gatedOff}
                            <span class="state-pill smoke gated"   title="Smoke armed, but the Rule 43 activation channel is below its threshold — heater forced off">⏸ gated off</span>
                        {:else if armed}
                            <span class="state-pill smoke"         title="Smoke armed — heater allowed to drive when the activation gate (if any) goes high">smoke armed</span>
                        {/if}
                        {#if fanRunning}
                            <span class="state-pill fan"           title="Fan is currently being driven (continuous or puff in progress)">💨 fan</span>
                        {/if}
                        <!-- Activation live µs readout — only when a
                             channel is bound.  Compact mono format so
                             the operator sees the gate's instant value
                             without opening the channel cluster body. -->
                        {#if gun.smoke.heater.activation.input}
                            {@const us = actLive?.us ?? null}
                            {@const ok = actLive?.valid && us !== null && us >= gun.smoke.heater.activation.thresholdUs}
                            <span class="act-readout" class:above={ok} class:below={!ok}
                                  title="Activation channel `{gun.smoke.heater.activation.input}` live µs vs threshold {gun.smoke.heater.activation.thresholdUs}µs">
                                ACT: {us === null ? '—' : `${us}µs`} {ok ? '✓' : '✗'}
                            </span>
                        {/if}
                        <!-- Section-warn chip when role(s) missing on IO tab. -->
                        {#if heaterMissing || fanMissing}
                            <span class="section-warn-tag" title="Open the IO tab and attach the missing role(s) to a free PWM port — then return here to bind them.">
                                no PWM port with {
                                    heaterMissing && fanMissing ? 'Heater + DcMotor'
                                  : heaterMissing ? 'Heater'
                                  : 'DcMotor'
                                } role attached
                            </span>
                        {/if}
                        <!-- Simulate cluster moved into the header
                             (mirrors gun-card pattern).  Stop sits to
                             the right + always enabled (Rule 48 b). -->
                        <div class="op-cluster">
                            <button class="oc-btn oc-primary"
                                    on:click={() => gunSmokeArm(gun.id, true)}
                                    disabled={busy || $gunfxDirty || gun.smoke.heater.port.guid === ''}
                                    title={$gunfxDirty
                                        ? 'Apply changes before testing — runs the loaded firmware config'
                                        : (gun.smoke.heater.port.guid === ''
                                            ? 'Pick a heater port below first — there is nothing to drive yet'
                                            : 'Arm smoke: drives heater at element-scaled duty.  Fan follows when trigger fires.')}>
                                ▶ Smoke ON
                            </button>
                            <button class="oc-btn oc-danger"
                                    on:click={() => gunSmokeArm(gun.id, false)}
                                    disabled={busy}
                                    title="Smoke OFF: cuts heater + fan immediately (safety button, always enabled — firmware re-sends OFF even if state was already off).">
                                ■ Smoke OFF
                            </button>
                        </div>
                    </div>
                </div>

                <!-- 1. Activation channel (Rule 43 — RC gate). Same
                     shared widget the engine on/off uses (Rule 36).
                     Empty = "no gating, heater always allowed when
                     smoke is armed". -->
                <div class="subsection-head">Activation channel
                    <span class="hint">RC gate — leave empty for "always allowed when smoke armed"</span>
                </div>
                <ChannelToggleCluster
                    channelLabel="Channel"
                    emptyOption="— none (always allowed) —"
                    options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
                    inputId={gun.smoke.heater.activation.input}
                    thresholdUs={gun.smoke.heater.activation.thresholdUs}
                    hysteresisUs={gun.smoke.heater.activation.hysteresisUs}
                    liveUs={liveUsFor(gun.smoke.heater.activation.input)?.us ?? null}
                    liveValid={liveUsFor(gun.smoke.heater.activation.input)?.valid ?? false}
                    busy={busy}
                    actionVerb="Heats"
                    onChange={(n) => {
                        setHeaterActivationField(gun.id, 'input', n.inputId)
                        setHeaterActivationField(gun.id, 'thresholdUs', n.thresholdUs)
                        setHeaterActivationField(gun.id, 'hysteresisUs', n.hysteresisUs)
                    }} />

                <!-- 2 + 3. Heater + Fan in side-by-side framed boxes
                     (Phase 4 polish 2026-05-26).  Each frame is its own
                     bordered container so the operator can scan one
                     subsystem without the other's fields competing for
                     attention.  On narrower windows the grid collapses
                     to stacked rows (see .smoke-frame-grid below). -->
                <div class="smoke-frame-grid">
                    <!-- ── Heater frame ─────────────────────────────── -->
                    <div class="smoke-frame">
                        <div class="frame-head">Heater
                            <span class="hint">on whenever smoke is armed AND activation channel is high</span>
                        </div>
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
                        <!-- Element voltage (Rule 42 — gun owns the spec;
                             firmware ships it to HeaterRole at configure
                             time so scaleDuty() drives the right duty
                             against the port rail).  Default 6 V matches
                             the common smoke-cartridge element. -->
                        <div class="form-row">
                            <span class="field-label">Element</span>
                            <input class="field-input narrow" type="number" min="1000" max="24000" step="100"
                                   value={gun.smoke.heater.elementMv}
                                   on:change={(e) => setHeaterField(gun.id, 'elementMv', numValue(e))} disabled={busy}
                                   title="Rated voltage of the heating element (default 6 V smoke cartridge).  Firmware scales duty against the port rail so a 6 V element on an 8 V rail never sees more than its rated average." />
                            <span class="unit">mV</span>
                        </div>
                        <!-- Heater mode (Phase 4 polish 2026-05-26).
                             continuous = drive whenever activated.
                             cycle = power-conservation duty cycle —
                             gun layer toggles HEATER_SET_TARGET between
                             on + off on cycleOnMs / cycleOffMs intervals
                             while smoke is armed.  Replaces bang_bang
                             which required a temp sensor we don't have. -->
                        <div class="form-row">
                            <span class="field-label">Mode</span>
                            <select class="field-input" style="flex:1" value={gun.smoke.heater.mode}
                                    on:change={(e) => setHeaterField(gun.id, 'mode', selValue(e))} disabled={busy}
                                    title="continuous = drive the heater at element-scaled duty whenever activated.
cycle = pulse the heater on for `cycle_on_ms` then off for `cycle_off_ms`, repeating while smoke is armed.  Use for power conservation or to limit cartridge temperature without a thermistor.">
                                <option value="continuous">continuous</option>
                                <option value="cycle">cycle</option>
                            </select>
                        </div>
                        <!-- Cycle on/off ms — only shown in cycle mode.
                             continuous mode ignores these fields entirely. -->
                        {#if gun.smoke.heater.mode === 'cycle'}
                            <div class="form-row">
                                <span class="field-label">On</span>
                                <input class="field-input narrow" type="number" min="100" max="60000" step="100"
                                       value={gun.smoke.heater.cycleOnMs}
                                       on:change={(e) => setHeaterField(gun.id, 'cycleOnMs', numValue(e))} disabled={busy}
                                       title="ON-phase duration in ms.  Heater drives at element-scaled duty for this long, then enters the OFF-phase." />
                                <span class="unit">ms</span>
                                <span class="hint compact">({(gun.smoke.heater.cycleOnMs/1000).toFixed(1)} s)</span>
                            </div>
                            <div class="form-row">
                                <span class="field-label">Off</span>
                                <input class="field-input narrow" type="number" min="100" max="60000" step="100"
                                       value={gun.smoke.heater.cycleOffMs}
                                       on:change={(e) => setHeaterField(gun.id, 'cycleOffMs', numValue(e))} disabled={busy}
                                       title="OFF-phase duration in ms.  Heater is cut for this long before the next ON-phase." />
                                <span class="unit">ms</span>
                                <span class="hint compact">({(gun.smoke.heater.cycleOffMs/1000).toFixed(1)} s)</span>
                            </div>
                            {@const total = gun.smoke.heater.cycleOnMs + gun.smoke.heater.cycleOffMs}
                            {@const dutyPct = total > 0 ? Math.round(100 * gun.smoke.heater.cycleOnMs / total) : 0}
                            <div class="form-row">
                                <span class="hint">average duty: <strong>{dutyPct}%</strong> ({((gun.smoke.heater.cycleOnMs + gun.smoke.heater.cycleOffMs)/1000).toFixed(1)} s period)</span>
                            </div>
                        {/if}
                    </div>

                    <!-- ── Fan frame ────────────────────────────────── -->
                    <div class="smoke-frame">
                        <div class="frame-head">Fan
                            <span class="hint">on when smoke armed AND trigger active</span>
                        </div>
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
                            <span class="field-label">Element</span>
                            <input class="field-input narrow" type="number" min="1000" max="24000" step="100"
                                   value={gun.smoke.fan.elementMv}
                                   on:change={(e) => setFanField(gun.id, 'elementMv', numValue(e))} disabled={busy}
                                   title="Rated voltage of the fan motor (default 6 V smoke fan).  Firmware scales duty against the port rail." />
                            <span class="unit">mV</span>
                        </div>
                        <div class="form-row">
                            <span class="field-label">Mode</span>
                            <select class="field-input" style="flex:1" value={gun.smoke.fan.mode}
                                    on:change={(e) => setFanField(gun.id, 'mode', selValue(e))} disabled={busy}
                                    title="continuous = fan at 100 % of element-rated voltage while firing and smoke armed.
pulse = sinusoidal envelope per shot — fan idles at 50 % base while firing+armed, ramps up to 100 % then back to 50 % over `pulse_duration_ms` on each shot.">
                                <option value="continuous">continuous (always on while firing)</option>
                                <option value="pulse">pulse (sinusoidal — 50% base, peaks per shot)</option>
                            </select>
                        </div>
                        {#if gun.smoke.fan.mode === 'pulse'}
                            <div class="form-row">
                                <span class="field-label">Duration</span>
                                <input class="field-input narrow" type="number" min="20" max="2000" step="10"
                                       value={gun.smoke.fan.pulseDurationMs}
                                       on:change={(e) => setFanField(gun.id, 'pulseDurationMs', numValue(e))} disabled={busy}
                                       title="One sinusoid period in ms.  Fan rises from 50 % → 100 % over the first half and falls back to 50 % over the second.  Default 100 ms matches the default 600 RPM firing rate so the envelope completes once per shot." />
                                <span class="unit">ms</span>
                                <span class="hint compact">({(gun.smoke.fan.pulseDurationMs/1000).toFixed(2)} s sinusoid)</span>
                            </div>
                            <div class="form-row">
                                <span class="hint">envelope: 50% base → 100% peak (mid-duration) → 50% base; resets on every shot</span>
                            </div>
                        {/if}
                    </div>
                </div>
            </div>
        {/each}
        </div>
    {/if}

</div>  <!-- /.card.gunfx-card -->


<style>
    .banner.err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); padding: 7px 10px; border-radius: 4px; margin: 6px 0; font-size: 12px; }
    .enable-toggle { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--text-dim); cursor: pointer; }
    .enable-toggle.inline { display: inline-flex; }
    .enable-toggle input { accent-color: var(--accent); }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .header-actions button { height: 28px; box-sizing: border-box; }

    /* .op-cluster + .oc-btn / .oc-picker / .oc-danger live in global
       style.css so EnginePanel + future operational panels share the
       same visual treatment.  Local overrides go below if any panel
       needs a tweak (none today). */

    /* Per-gun two-column grid: col 1 = gun core (trigger/ROF/muzzle/
       turret/recoil), col 2 = smoke (activation/heater/fan).  Each gun
       is one grid row; align-items:start keeps the shorter smoke card
       pinned to the top instead of stretching to the gun card's height. */
    .guns-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 0 14px;
        align-items: start;
    }
    /* Below ~900px the two columns would crush — stack to one. */
    @media (max-width: 900px) {
        .guns-grid { grid-template-columns: 1fr; }
    }

    .gun-card { margin-bottom: 14px; }
    .card-header.inner { padding: 4px 0 8px; border-bottom: 1px dashed var(--border); margin-bottom: 8px; }
    .card-header.inner h4 { font-size: 13px; font-weight: 600; color: var(--text-bright); }

    .state-pill { font-family: var(--font-mono); font-size: 10px; padding: 2px 8px; border-radius: 3px; background: var(--bg-input); color: var(--text-dim); border: 1px solid var(--border); text-transform: uppercase; letter-spacing: 0.4px; }
    .state-pill.firing { background: rgba(255,80,80,0.22); color: #ff8a85; border-color: rgba(255,80,80,0.5); }
    .state-pill.smoke { background: rgba(255,180,0,0.18); color: var(--warning); border-color: rgba(255,180,0,0.5); }
    /* Gun-smoke header pills (Phase 4 polish 2026-05-26).
       `.heating` = orange-red, signals "heater is being driven".
       `.gated`   = muted yellow with dashed border, signals "armed but
                    Rule 43 activation gate is blocking the heater".
       `.fan`     = cyan/accent, separate from the heater pills since
                    the fan can run independently of the heater state
                    (smoke armed + trigger firing → fan on regardless
                    of activation channel — only the heater is gated). */
    .state-pill.smoke.heating { background: rgba(255,120,40,0.25); color: #ffaa55; border-color: rgba(255,120,40,0.6); }
    .state-pill.smoke.gated   { background: rgba(255,180,0,0.10); color: color-mix(in srgb, var(--warning) 70%, var(--text-dim)); border-style: dashed; }
    .state-pill.fan           { background: color-mix(in srgb, var(--accent) 18%, var(--bg-input)); color: var(--accent); border-color: color-mix(in srgb, var(--accent) 45%, var(--border)); }

    /* Activation channel live µs readout in the smoke-card header.
       Tight mono format; colour follows above/below threshold so the
       operator sees gate state at a glance. */
    .act-readout { font-family: var(--font-mono); font-size: 10px; padding: 2px 6px; border-radius: 3px; border: 1px solid var(--border); background: var(--bg-input); letter-spacing: 0.3px; }
    .act-readout.above { color: var(--success); border-color: color-mix(in srgb, var(--success) 45%, var(--border)); }
    .act-readout.below { color: var(--text-dim); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    /* `.section-head .hint` reset lives in global style.css (Rule 50). */
    /* Rule 39 — yellow warning on an optional section that's enabled
       but has no candidate port available.  Non-fatal: doesn't gate
       Apply, but flags the issue to the operator. */
    .section-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
    .section-warn-tag { font-size: 9px; font-weight: 700; color: var(--warning); padding: 1px 6px; border: 1px solid var(--warning); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; }
    /* Rule 35 hard-error variant: red header + red tag, gates global
       Apply via the source's gunfxHasErrors derived store. */
    .section-head.section-warn-err { color: var(--error); border-bottom-color: var(--error); }
    .section-warn-err-tag { font-size: 9px; font-weight: 700; color: var(--error); padding: 1px 6px; border: 1px solid var(--error); border-radius: 3px; letter-spacing: 0.5px; text-transform: uppercase; background: color-mix(in srgb, var(--error) 12%, transparent); }

    /* `.unit`, `.hint` / `.hint.compact/warn/err`, `.help-text` all live
       in global style.css (Rule 50 typography taxonomy).  Don't
       redefine here.  `.field-label` keeps the local override below
       because GunFx + LightFx are dense panels — 10 px fits more rows
       in view; the global 12 px is for simpler tabs. */
    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .trigger-pm { margin: 0 4px; color: var(--text-dim); font-family: var(--font-mono); font-weight: 700; }

    /* ROF item — two-row card per item, with a coloured side-stripe
       matching the band palette so the operator can visually pair a
       row with its zone on the overlay bar above. */
    .rof-item { display: flex; flex-direction: column; gap: 4px; padding: 8px 10px; margin: 6px 0; background: var(--bg-raised); border: 1px solid var(--border); border-left: 3px solid var(--band-color, var(--accent)); border-radius: 4px; }
    .rof-item.invalid { border-color: var(--error); border-left-color: var(--error); background: rgba(255,80,80,0.05); }
    .rof-item.soft-warn { border-color: var(--warning); border-left-color: var(--warning); background: rgba(255,180,0,0.04); }

    /* Per-row issue list — each line carries its own colour cue so a
       mixed-severity row reads clearly (red errors first, amber warns
       below).  Compact margin keeps the row from getting noisy. */
    .rof-issues { list-style: none; margin: 4px 0 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
    .rof-issue { font-size: 11px; line-height: 1.35; padding-left: 4px; }
    .rof-issue.err  { color: var(--error); }
    .rof-issue.warn { color: var(--warning); }
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
    .rof-output-field { flex: 0 0 90px; }
    .rof-output-field select { width: 100%; }
    .rof-field:not(.rof-band-field):not(.rof-rpm-field):not(.rof-sound-field) { flex: 1; min-width: 120px; }

    /* Shared SoundRow owns the row chrome; we only need the lead-slot
       pill to remain hidden but width-locked so the row column-aligns
       with the #N badge on Row 1 above. */
    .rof-idx-pill.placeholder { flex-shrink: 0; }

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

    /* Smoke generator sibling card (Phase 4 polish 2026-05-26).
       Lives directly after each gun card via the {#each} block, so it
       inherits the standard `.card` chrome (border, background, padding).
       Small visual tweaks below to signal "this is paired with the gun
       card above" without cloning the gun's emphasis:
         - margin-top pulled in tight (4 px) so the pair reads as one unit
         - margin-bottom remains the card default for spacing to the next gun
         - subtle left-border accent in the warning colour family signals
           "this depends on the gun card above" without shouting */
    .smoke-card { margin-top: 4px; margin-bottom: 14px;
        border-left: 3px solid color-mix(in srgb, var(--accent) 30%, var(--border)); }
    .smoke-card .subsection-head:first-of-type { margin-top: 4px; }

    /* Heater + Fan framed boxes, side-by-side (Phase 4 polish 2026-05-26).
       2-col grid collapses to 1-col when the gun-smoke card narrows
       below 520 px (≈ left+right padding + two frames).  Each frame
       gets a hairline border + raised background so the operator
       reads them as two distinct subsystems, not one continuous
       form. */
    .smoke-frame-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
        gap: 10px;
        margin-top: 6px;
    }
    .smoke-frame {
        background: var(--bg-raised);
        border: 1px solid var(--border);
        border-radius: 4px;
        padding: 8px 10px;
    }
    .smoke-frame .frame-head {
        font-size: 11px;
        font-weight: 700;
        text-transform: uppercase;
        letter-spacing: 0.5px;
        color: var(--text-bright);
        margin: 2px 0 6px;
        padding-bottom: 4px;
        border-bottom: 1px solid color-mix(in srgb, var(--accent) 25%, var(--border));
        display: flex; align-items: baseline; gap: 8px; flex-wrap: wrap;
    }
    /* Heading-context hint reset (mirrors the global rule for
       .section-head/.subsection-head — Rule 50): undo the bold +
       uppercase + letter-spacing that .frame-head sets, restore the
       11 px italic dim description look. */
    .smoke-frame .frame-head .hint {
        font-size: 9px;
        font-weight: 400;
        text-transform: none;
        letter-spacing: 0;
        color: var(--text-dim);
        font-style: italic;
    }

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
