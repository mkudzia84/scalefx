<!-- ScaleFX Studio — Firmware Tab -->
<!-- Displays connected board info, available GitHub releases, and flash controls. -->
<script lang="ts">
    import { connectionInfo, firmwareTargets, firmwareRunning, firmwareLogs, boardState, connectPopupOpen, availableReleases, showFlashProgress, flashResult } from '../stores'
    import type { FirmwareTarget, ReleaseInfo } from '../stores'
    import { GetFirmwareTargets, GetReleases, FlashFromRelease, DeviceSystemInfo, AudioPreloads, QueryDeviceStatus } from '../../../wailsjs/go/main/App'
    import { boardKindOf } from '../devicemodel'
    import { onMount, onDestroy, tick } from 'svelte'

    // ── Board capability + topology view (DeviceSystemInfo) ──────────
    // One-shot fetched after connect: decoded capability flags plus — on a
    // hub — the full port roster + every connected expander (incl. battery).
    let deviceInfo: any = null
    let deviceInfoErr = ''
    let lastInfoKey = ''

    // Audio asset cache is meaningful only on a board that advertises AUDIO
    // (the HubFX master). An expander (gearcontrol) has no audio — its
    // AudioPreloads() comes back with entries=null, so gate the section off
    // rather than rendering an empty cache (and crashing on entries.length).
    $: hasAudio = !!deviceInfo?.capabilityNames?.includes?.('AUDIO')

    async function loadDeviceInfo() {
        if (!$connectionInfo.connected) { deviceInfo = null; deviceInfoErr = ''; return }
        try {
            deviceInfo = await DeviceSystemInfo()
            deviceInfoErr = ''
        } catch (e: any) {
            deviceInfoErr = e?.message || String(e)
            deviceInfo = null
        }
    }

    // Refetch once per connect / disconnect / port change.
    $: {
        const k = $connectionInfo.connected ? $connectionInfo.port : ''
        if (k !== lastInfoKey) {
            lastInfoKey = k
            loadDeviceInfo()
            loadPreloads()
            loadLiveStatus()
            scheduleLiveStatus()
        }
    }

    // ── Live memory poll ────────────────────────────────────────────
    // STATUS broadcast already runs in the firmware at ~1 Hz, but we
    // poll it from a timer here so the GUI numbers refresh even when
    // verbose-status broadcasts aren't enabled (default).  Cheap —
    // ~50 bytes wire round-trip.
    let liveStatus: any = null
    let liveStatusTimer: any = null

    async function loadLiveStatus() {
        if (!$connectionInfo.connected) { liveStatus = null; return }
        try {
            liveStatus = await QueryDeviceStatus()
        } catch {
            liveStatus = null
        }
    }
    function scheduleLiveStatus() {
        if (liveStatusTimer) clearInterval(liveStatusTimer)
        if (!$connectionInfo.connected) return
        liveStatusTimer = setInterval(loadLiveStatus, 2000)
    }
    onDestroy(() => { if (liveStatusTimer) clearInterval(liveStatusTimer) })

    // ── Audio asset-preload snapshot ────────────────────────────────
    // Mirrors `AudioAssetCache::logPreloadStatus()` — per-path
    // residency + owner tags pushed by alerts/enginefx/gunfx config
    // apply.  Refetched on connect + via the Refresh button; cheap
    // (single wire query, ~50 bytes per entry).
    let preloads: any = null
    let preloadsErr = ''
    let preloadsLoading = false

    async function loadPreloads() {
        if (!$connectionInfo.connected) { preloads = null; preloadsErr = ''; return }
        preloadsLoading = true
        try {
            preloads = await AudioPreloads()
            preloadsErr = ''
        } catch (e: any) {
            preloadsErr = e?.message || String(e)
            preloads = null
        } finally {
            preloadsLoading = false
        }
    }

    function fmtKB(bytes: number): string {
        if (!bytes) return '0 KB'
        return (bytes / 1024).toFixed(bytes < 102400 ? 1 : 0) + ' KB'
    }
    function fmtMB(bytes: number): string {
        return (bytes / 1024 / 1024).toFixed(1) + ' MB'
    }
    function preloadPct(e: any): number {
        if (!e.totalBytes) return 0
        return Math.min(100, Math.round((e.loadedBytes * 100) / e.totalBytes))
    }
    function statusClass(s: number): string {
        switch (s) {
            case 2: return 'preload-ready'
            case 1: return 'preload-loading'
            case 3: return 'preload-failed'
            default: return ''
        }
    }
    function statusLabel(s: number): string {
        switch (s) { case 2: return 'ready'; case 1: return 'loading'; case 3: return 'failed' }
        return '?'
    }

    // Find the cached battery reading for a board GUID (expanders only).
    function batteryFor(guid: string): any {
        const exps = deviceInfo?.system?.expanders
        if (!exps || !guid) return null
        for (const e of exps) {
            if (e.guid === guid && e.battery && e.battery.valid) return e.battery
        }
        return null
    }

    function fmtBattery(b: any): string {
        if (!b?.present) return 'no battery'
        let s = `${(b.voltage_mV / 1000).toFixed(2)} V`
        if (b.cellCount > 0) s += ` · ${b.cellCount}S`
        s += ` · ${b.pct}%`
        if (b.flags & 0x02) s += ' · CUTOFF'
        else if (b.flags & 0x01) s += ' · LOW'
        return s
    }

    // Compact per-kind port counts for a board's PortList.
    function portRows(p: any): string[] {
        if (!p) return []
        const rows: string[] = []
        if (p.Servos?.length) rows.push(`servo ×${p.Servos.length}`)
        if (p.Pwms?.length) rows.push(`pwm ×${p.Pwms.length}`)
        if (p.HBridges?.length) rows.push(`hbridge ×${p.HBridges.length}`)
        if (p.Inputs?.length) rows.push(`input ×${p.Inputs.length}`)
        return rows
    }

    // Flash controls
    let selectedController = ''
    let selectedRelease = ''
    let flashPort = ''
    let skipVerify = false

    // Release fetching
    let fetchingReleases = false
    let fetchError = ''

    // Releases filtered to selected controller
    $: controllerReleases = $availableReleases.filter(r => r.controller === selectedController)

    // Auto-select first release when controller changes
    $: if (selectedController && controllerReleases.length > 0) {
        selectedRelease = controllerReleases[0].tag
    } else {
        selectedRelease = ''
    }

    // Get the currently selected release object
    $: currentRelease = $availableReleases.find(r => r.tag === selectedRelease)

    // Connected board's firmware version (for comparison)
    $: connectedVersion = $connectionInfo.firmwareVersion || ''
    $: connectedController = $connectionInfo.controllerType || ''

    // Version comparison: is the selected release newer than running firmware?
    $: versionStatus = getVersionStatus(currentRelease, connectedController, connectedVersion)

    // Group releases by controller for the summary table
    $: releasesByController = groupByController($availableReleases)

    onMount(async () => {
        // Load firmware targets for the controller list
        const targets = await GetFirmwareTargets()
        firmwareTargets.set(targets)

        // Auto-select connected controller
        if ($connectionInfo.controllerType) {
            selectedController = $connectionInfo.controllerType
        }

        // Fetch releases
        fetchAllReleases()
    })

    async function fetchAllReleases() {
        fetchingReleases = true
        fetchError = ''
        try {
            const releases = await GetReleases('')
            availableReleases.set(releases || [])
        } catch (e: any) {
            fetchError = e?.message || 'Failed to fetch releases'
            availableReleases.set([])
        } finally {
            fetchingReleases = false
        }
    }

    function getVersionStatus(release: ReleaseInfo | undefined, ctrlType: string, currentVer: string): 'newer' | 'same' | 'older' | 'unknown' {
        if (!release || !currentVer || ctrlType !== release.controller) return 'unknown'
        const cmp = compareVersions(release.version, currentVer)
        if (cmp > 0) return 'newer'
        if (cmp === 0) return 'same'
        return 'older'
    }

    function compareVersions(a: string, b: string): number {
        const ap = a.split('.').map(Number)
        const bp = b.split('.').map(Number)
        const len = Math.max(ap.length, bp.length)
        for (let i = 0; i < len; i++) {
            const ai = ap[i] || 0
            const bi = bp[i] || 0
            if (ai !== bi) return ai - bi
        }
        return 0
    }

    function groupByController(releases: ReleaseInfo[]): Map<string, ReleaseInfo> {
        const map = new Map<string, ReleaseInfo>()
        for (const r of releases) {
            if (!map.has(r.controller)) {
                map.set(r.controller, r) // first = newest (sorted by backend)
            }
        }
        return map
    }

    // ── Per-board firmware status (hub + every connected expander) ──────
    //
    // Firmware versions on the wire carry a controller suffix
    // ("2.22.1-hubfx"); GitHub release versions don't ("2.22.1").  normVer
    // strips the suffix so the naive numeric compareVersions doesn't choke
    // on "1-hubfx" → NaN (which silently misreported "newer on device").
    function normVer(v: string): string {
        return (v || '').split('-')[0].replace(/^v/, '')
    }

    type FwStatus = 'current' | 'update' | 'newer' | 'unknown'
    interface BoardFw {
        name: string; guid: string; controller: string
        version: string; build: number | string
        latest: string; latestTag: string; prerelease: boolean
        status: FwStatus; isHub: boolean
    }

    function fwStatusOf(version: string, latest: string): FwStatus {
        if (!latest || !version) return 'unknown'
        const c = compareVersions(normVer(latest), normVer(version))
        return c > 0 ? 'update' : c === 0 ? 'current' : 'newer'
    }

    // The connected hub + each identified expander, joined to the latest
    // release for that board kind.  Drives the "Board Firmware" overview.
    $: boardFwList = buildBoardFwList($connectionInfo, deviceInfo, releasesByController)
    function buildBoardFwList(conn: any, info: any, latestMap: Map<string, ReleaseInfo>): BoardFw[] {
        const out: BoardFw[] = []
        const mk = (name: string, guid: string, controller: string, version: string, build: number | string, isHub: boolean): BoardFw => {
            const rel = latestMap.get(controller)
            return {
                name: name || controller || '—', guid, controller,
                version: version || '?', build: (build ?? '?'),
                latest: rel?.version ?? '', latestTag: rel?.tag ?? '', prerelease: rel?.prerelease ?? false,
                status: fwStatusOf(version, rel?.version ?? ''), isHub,
            }
        }
        if (conn?.connected) {
            const hubGuid = info?.system?.hub?.guid ?? ''
            out.push(mk(conn.controllerName, hubGuid, conn.controllerType || '', conn.firmwareVersion, conn.build, true))
        }
        const exps = info?.system?.expanders ?? []
        for (const e of exps) {
            if (!e?.identified) continue
            const controller = boardKindOf(e.kindName || e.deviceName || '')
            out.push(mk(e.deviceName || e.kindName || controller, e.guid || '', controller, e.firmwareVersion, e.buildNumber, false))
        }
        return out
    }
    // Boards that are behind their latest release — the headline "issue".
    $: outdatedBoards = boardFwList.filter(b => b.status === 'update')

    // Firmware status for one board GUID (joins ports↔system) — used to
    // annotate the System Topology rows.  "" guid ⇒ the hub.
    function fwForGuid(guid: string): BoardFw | undefined {
        if (guid) return boardFwList.find(b => b.guid === guid)
        return boardFwList.find(b => b.isHub) // empty guid in the topology list = hub
    }
    function fwBadgeLabel(s: FwStatus): string {
        switch (s) {
            case 'current': return 'up to date'
            case 'update':  return 'update available'
            case 'newer':   return 'dev build'
            default:        return 'no release info'
        }
    }

    // "Update" affordance: pre-select this board's controller + latest
    // release in the flash controls below (then the operator hits Flash).
    function selectForFlash(b: BoardFw): void {
        if (!b.controller) return
        selectedController = b.controller
    }

    async function startFlash() {
        if (!selectedRelease || $firmwareRunning) return
        boardState.set('flashing')
        connectPopupOpen.set(false)
        firmwareRunning.set(true)
        firmwareLogs.set([])
        flashResult.set(null)
        showFlashProgress.set(true)
        // Ensure FlashProgressDialog is mounted and its event listener registered
        await tick()
        try {
            await FlashFromRelease(selectedController, selectedRelease, flashPort, skipVerify)
        } catch (e: any) {
            // Binding error — Go goroutine handles its own errors via events,
            // so this only fires if the Wails call itself fails.
            firmwareRunning.set(false)
            flashResult.set({ success: false, message: e?.message || 'Flash failed' })
            boardState.set('disconnected')
        }
    }
</script>

<div class="firmware-tab">
    <h2>Firmware</h2>

    <!-- Connected board info -->
    <section class="info-section">
        <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg> Connected Board</h3>
        {#if $connectionInfo.connected}
            <table class="info-table">
                <tr><td class="label">Name</td><td>{$connectionInfo.controllerName || '—'}</td></tr>
                <tr><td class="label">Type</td><td>{$connectionInfo.controllerType || '—'}</td></tr>
                <tr><td class="label">Firmware</td><td>{$connectionInfo.firmwareVersion || '—'}</td></tr>
                <tr><td class="label">Build</td><td>{$connectionInfo.build || '—'}</td></tr>
                <tr><td class="label">Platform</td><td>{$connectionInfo.platform || '—'}</td></tr>
                <tr><td class="label">CPU</td><td>{$connectionInfo.cpuMHz ? `${$connectionInfo.cpuMHz} MHz` : '—'}</td></tr>
                <tr><td class="label">Free RAM (total)</td><td>{liveStatus?.freeRamBytes ? formatBytes(liveStatus.freeRamBytes) : ($connectionInfo.freeRAM ? formatBytes($connectionInfo.freeRAM) : '—')}</td></tr>
                {#if liveStatus?.hasMemExtension}
                    <tr><td class="label">Free DRAM</td><td>{formatBytes(liveStatus.freeDramBytes)} <span class="mem-hint">internal SRAM</span></td></tr>
                    <tr><td class="label">Free PSRAM</td><td>{liveStatus.freePsramBytes ? formatBytes(liveStatus.freePsramBytes) : '—'} <span class="mem-hint">external octal RAM</span></td></tr>
                {/if}
                <tr><td class="label">Port</td><td class="mono">{$connectionInfo.port || '—'}</td></tr>
            </table>
        {:else}
            <p class="no-data">No board connected</p>
        {/if}
    </section>

    <!-- Board Firmware — per-board version + up-to-date status (hub + every
         connected expander), checked against the latest GitHub release for
         each board kind.  This is the at-a-glance "is anything out of date?". -->
    {#if $connectionInfo.connected && boardFwList.length}
        <section class="info-section">
            <div class="section-header">
                <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83"/></svg> Board Firmware</h3>
                <button class="btn-refresh" on:click={() => { loadDeviceInfo(); fetchAllReleases() }} disabled={fetchingReleases}>
                    {fetchingReleases ? 'Checking…' : 'Refresh'}
                </button>
            </div>

            {#if outdatedBoards.length}
                <div class="fw-issues">
                    <strong>{outdatedBoards.length} board{outdatedBoards.length > 1 ? 's' : ''}</strong>
                    {outdatedBoards.length > 1 ? 'have' : 'has'} a newer release available:
                    {outdatedBoards.map(b => `${b.name} (v${normVer(b.version)} → v${b.latest})`).join(', ')}.
                </div>
            {/if}

            <table class="info-table fw-table">
                <thead>
                    <tr><th>Board</th><th>GUID</th><th>Version</th><th>Build</th><th>Latest</th><th>Status</th><th></th></tr>
                </thead>
                <tbody>
                    {#each boardFwList as b (b.isHub ? 'hub' : b.guid)}
                        <tr class:fw-stale={b.status === 'update'}>
                            <td>
                                {b.name}
                                {#if b.isHub}<span class="role-tag">hub</span>{:else}<span class="role-tag exp">expander</span>{/if}
                            </td>
                            <td class="mono text-dim">{b.guid || '—'}</td>
                            <td class="mono">v{normVer(b.version)}</td>
                            <td class="mono text-dim">{b.build}</td>
                            <td class="mono">{b.latest ? `v${b.latest}${b.prerelease ? ' (pre)' : ''}` : '—'}</td>
                            <td>
                                <span class="badge badge-{b.status}">{fwBadgeLabel(b.status)}</span>
                            </td>
                            <td>
                                {#if b.status === 'update'}
                                    {#if b.isHub}
                                        <button class="btn-update small" on:click={() => selectForFlash(b)}
                                                title="Select {b.controller} + its latest release in the flash controls below">⚡ Update…</button>
                                    {:else}
                                        <span class="text-dim small" title="An expander is flashed by connecting it directly to this PC, not through the hub">flash directly</span>
                                    {/if}
                                {/if}
                            </td>
                        </tr>
                    {/each}
                </tbody>
            </table>
            {#if fetchError}<p class="fetch-error">Release check failed: {fetchError}</p>{/if}
        </section>
    {/if}

    <!-- Board Capabilities (from IDENTIFY) -->
    {#if $connectionInfo.connected}
        <section class="info-section">
            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2L2 7l10 5 10-5-10-5z"/><path d="M2 17l10 5 10-5"/><path d="M2 12l10 5 10-5"/></svg> Capabilities</h3>
            {#if deviceInfoErr}
                <p class="fetch-error">{deviceInfoErr}</p>
            {:else if deviceInfo?.capabilityNames?.length}
                <div class="cap-chips">
                    {#each deviceInfo.capabilityNames as cap}
                        <span class="cap-chip">{cap}</span>
                    {/each}
                </div>
            {:else}
                <p class="no-data">No capabilities advertised</p>
            {/if}
        </section>

        <!-- System Topology — hub + expanders + per-board port roster -->
        {#if deviceInfo?.hasTopology}
            <section class="info-section">
                <div class="section-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><line x1="12" y1="3" x2="12" y2="9"/><line x1="12" y1="15" x2="12" y2="21"/><line x1="3" y1="12" x2="9" y2="12"/><line x1="15" y1="12" x2="21" y2="12"/></svg> System Topology</h3>
                    <button class="btn-refresh" on:click={loadDeviceInfo}>Refresh</button>
                </div>
                {#if deviceInfo.ports?.length}
                    {#each deviceInfo.ports as board}
                        {@const bat = batteryFor(board.guid)}
                        {@const fw = fwForGuid(board.guid)}
                        <div class="board-block">
                            <div class="board-head">
                                <span class="board-name">{board.deviceName || board.guid || 'hub'}</span>
                                <span class="mono text-dim">[{board.guid || 'hub'}]</span>
                                {#if fw}
                                    <span class="fw-chip mono" title="Firmware version (build {fw.build})">v{normVer(fw.version)}</span>
                                    <span class="badge badge-{fw.status}" title={fw.latest ? `latest release v${fw.latest}` : 'no GitHub release for this board kind'}>{fwBadgeLabel(fw.status)}</span>
                                {/if}
                                {#if bat}<span class="batt-pill" class:warn={bat.flags & 0x03}>{fmtBattery(bat)}</span>{/if}
                            </div>
                            <div class="port-rows">
                                {#each portRows(board.ports) as row}
                                    <span class="port-pill">{row}</span>
                                {:else}
                                    <span class="text-dim">no ports</span>
                                {/each}
                            </div>
                        </div>
                    {/each}
                {:else}
                    <p class="no-data">No ports reported</p>
                {/if}
            </section>
        {/if}
    {/if}

    <!-- Audio asset preload cache (PSRAM residency view) — AUDIO boards only -->
    {#if $connectionInfo.connected && hasAudio}
        <section class="info-section">
            <div class="section-header">
                <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12h18M3 6h18M3 18h18"/></svg> Audio Asset Cache</h3>
                <button class="btn-refresh" on:click={loadPreloads} disabled={preloadsLoading}>
                    {preloadsLoading ? 'Querying...' : 'Refresh'}
                </button>
            </div>

            {#if preloadsErr}
                <p class="fetch-error">{preloadsErr}</p>
            {:else if !preloads}
                <p class="no-data">No data yet</p>
            {:else if !preloads.entries?.length}
                <p class="no-data">Cache empty — no audio assets registered</p>
            {:else}
                <div class="preload-summary">
                    <div class="preload-bar-track">
                        <div class="preload-bar-fill" style="width: {Math.min(100, Math.round((preloads.residentBytes * 100) / (preloads.budgetBytes || 1)))}%"></div>
                    </div>
                    <div class="preload-summary-text">
                        {fmtMB(preloads.residentBytes)} / {fmtMB(preloads.budgetBytes)}
                        ·
                        <span class="preload-ready">{preloads.ready} ready</span>
                        {#if preloads.loading > 0} · <span class="preload-loading">{preloads.loading} loading</span>{/if}
                        {#if preloads.failed > 0} · <span class="preload-failed">{preloads.failed} failed</span>{/if}
                        · {preloads.pinned} pinned
                    </div>
                </div>
                <table class="info-table preload-table">
                    <thead>
                        <tr>
                            <th>Path</th>
                            <th>Format</th>
                            <th>Size</th>
                            <th>Status</th>
                            <th>Owners</th>
                        </tr>
                    </thead>
                    <tbody>
                        {#each preloads.entries as e}
                            <tr>
                                <td class="mono">{e.path}</td>
                                <td>{e.formatName}</td>
                                <td>{fmtKB(e.totalBytes)}</td>
                                <td class={statusClass(e.status)}>
                                    {statusLabel(e.status)}
                                    {#if e.status === 1}
                                        ({preloadPct(e)}%)
                                    {/if}
                                </td>
                                <td>
                                    {#if e.owners && e.owners.length}
                                        {#each e.owners as o, i}
                                            <span class="owner-chip">{o}</span>{#if i < e.owners.length - 1} {/if}
                                        {/each}
                                    {:else}
                                        <span class="owner-lru">lru</span>
                                    {/if}
                                </td>
                            </tr>
                        {/each}
                    </tbody>
                </table>
            {/if}
        </section>
    {/if}

    <!-- Available Releases -->
    <section class="info-section">
        <div class="section-header">
            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20.59 13.41l-7.17 7.17a2 2 0 0 1-2.83 0L2 12V2h10l8.59 8.59a2 2 0 0 1 0 2.82z"/><line x1="7" y1="7" x2="7.01" y2="7"/></svg> Available Releases</h3>
            <button class="btn-refresh" on:click={fetchAllReleases} disabled={fetchingReleases}>
                {fetchingReleases ? 'Checking...' : 'Refresh'}
            </button>
        </div>

        {#if fetchError}
            <p class="fetch-error">{fetchError}</p>
        {:else if fetchingReleases && $availableReleases.length === 0}
            <p class="no-data">Fetching releases from GitHub...</p>
        {:else if $availableReleases.length === 0}
            <p class="no-data">No releases found</p>
        {:else}
            <table class="info-table releases-table">
                <thead>
                    <tr>
                        <th>Controller</th>
                        <th>Latest</th>
                        <th>Running</th>
                        <th>Status</th>
                    </tr>
                </thead>
                <tbody>
                    {#each [...releasesByController.entries()] as [ctrl, latest]}
                        {@const running = connectedController === ctrl ? connectedVersion : ''}
                        {@const status = running ? (compareVersions(latest.version, running) > 0 ? 'update' : compareVersions(latest.version, running) === 0 ? 'current' : 'older') : ''}
                        <tr class:highlight={ctrl === connectedController}>
                            <td>{ctrl}</td>
                            <td>v{latest.version}{latest.prerelease ? ' (pre)' : ''}</td>
                            <td>{running ? `v${running}` : '—'}</td>
                            <td>
                                {#if status === 'update'}
                                    <span class="badge badge-update">Update available</span>
                                {:else if status === 'current'}
                                    <span class="badge badge-current">Up to date</span>
                                {:else if status === 'older'}
                                    <span class="badge badge-older">Newer on device</span>
                                {:else}
                                    <span class="text-dim">—</span>
                                {/if}
                            </td>
                        </tr>
                    {/each}
                </tbody>
            </table>
        {/if}
    </section>

    <!-- Flash from Release -->
    <section class="info-section">
        <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/></svg> Flash Firmware</h3>

        <div class="flash-controls">
            <div class="control-row">
                <label for="ctrl-select">Controller</label>
                <select id="ctrl-select" bind:value={selectedController} disabled={$firmwareRunning}>
                    <option value="">— Select controller —</option>
                    {#each $firmwareTargets as t}
                        <option value={t.name}>{t.name} ({t.platform})</option>
                    {/each}
                </select>
            </div>

            <div class="control-row">
                <label for="release-select">Release</label>
                <select id="release-select" bind:value={selectedRelease} disabled={$firmwareRunning || controllerReleases.length === 0}>
                    {#if controllerReleases.length === 0}
                        <option value="">— No releases —</option>
                    {:else}
                        {#each controllerReleases as r}
                            <option value={r.tag}>v{r.version}{r.prerelease ? ' (pre-release)' : ''} — {formatDate(r.published)}</option>
                        {/each}
                    {/if}
                </select>
            </div>

            {#if currentRelease}
                <div class="release-detail">
                    <span class="detail-label">Asset:</span>
                    <span class="mono">{currentRelease.assetName}</span>
                    <span class="detail-sep">|</span>
                    <span>{formatBytes(currentRelease.assetSize)}</span>
                    {#if versionStatus === 'newer'}
                        <span class="detail-sep">|</span>
                        <span class="badge badge-update">Update</span>
                    {:else if versionStatus === 'same'}
                        <span class="detail-sep">|</span>
                        <span class="badge badge-current">Same version</span>
                    {:else if versionStatus === 'older'}
                        <span class="detail-sep">|</span>
                        <span class="badge badge-older">Downgrade</span>
                    {/if}
                </div>

                <!-- Release Notes -->
                {#if currentRelease.body}
                    <details class="release-notes" open>
                        <summary>Release Notes</summary>
                        <div class="release-notes-body">
                            {#each currentRelease.body.split('\n') as line}
                                {#if line.startsWith('## ')}
                                    <p class="rn-heading">{line.replace(/^##\s*/, '')}</p>
                                {:else if line.startsWith('### ')}
                                    <p class="rn-subheading">{line.replace(/^###\s*/, '')}</p>
                                {:else if line.startsWith('- ') || line.startsWith('* ')}
                                    <p class="rn-bullet">{line}</p>
                                {:else if line.startsWith('|')}
                                    <p class="rn-table">{line}</p>
                                {:else if line.trim() === ''}
                                    <br/>
                                {:else}
                                    <p class="rn-text">{line}</p>
                                {/if}
                            {/each}
                        </div>
                    </details>
                {/if}
            {/if}

            <div class="control-row">
                <label for="flash-port">Port</label>
                <input id="flash-port" type="text" bind:value={flashPort} placeholder="auto-detect" disabled={$firmwareRunning} />
            </div>

            <div class="option-row">
                <label><input type="checkbox" bind:checked={skipVerify} disabled={$firmwareRunning} /> Skip verify</label>
            </div>

            <div class="action-row">
                <button class="btn-flash" on:click={startFlash} disabled={!selectedRelease || $firmwareRunning}>
                    {$firmwareRunning ? 'Flashing...' : 'Flash'}
                </button>
            </div>
        </div>
    </section>
</div>

<script context="module" lang="ts">
    function formatBytes(bytes: number): string {
        if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(1)} MB`
        if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`
        return `${bytes} B`
    }

    function formatDate(iso: string): string {
        try {
            const d = new Date(iso)
            return d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' })
        } catch {
            return iso
        }
    }
</script>

<style>
    .firmware-tab {
        padding: 24px;
        max-width: 760px;
    }

    h2 {
        font-size: 18px;
        font-weight: 600;
        color: var(--text-bright);
        margin-bottom: 20px;
    }

    .info-section {
        margin-bottom: 24px;
    }

    h3 {
        font-size: 13px;
        font-weight: 600;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.5px;
        margin-bottom: 10px;
        display: flex;
        align-items: center;
        gap: 6px;
    }

    h3 svg {
        opacity: 0.7;
        flex-shrink: 0;
    }

    .section-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 10px;
    }

    .section-header h3 {
        margin-bottom: 0;
    }

    .btn-refresh {
        padding: 4px 12px;
        font-size: 11px;
        color: var(--text-dim);
        background: var(--bg-secondary);
        border: 1px solid var(--border);
        border-radius: 3px;
        cursor: pointer;
        transition: all 0.15s;
    }

    .btn-refresh:hover:not(:disabled) {
        color: var(--text);
        border-color: var(--text-dim);
    }

    .btn-refresh:disabled {
        opacity: 0.5;
        cursor: not-allowed;
    }

    .no-data {
        font-size: 13px;
        color: var(--text-dim);
        font-style: italic;
    }

    .fetch-error {
        font-size: 13px;
        color: var(--error);
    }

    .info-table {
        width: 100%;
        border-collapse: collapse;
    }

    .info-table td,
    .info-table th {
        padding: 6px 12px;
        text-align: left;
        border-bottom: 1px solid var(--border);
        font-size: 13px;
    }

    .info-table th {
        color: var(--text-dim);
        font-weight: 600;
    }

    .label {
        color: var(--text-dim);
        width: 120px;
        white-space: nowrap;
    }

    .mono {
        font-family: var(--font-mono);
    }

    /* ─── Releases Table ─── */

    .releases-table tr.highlight {
        background: var(--highlight-row);
    }

    .badge {
        display: inline-block;
        padding: 2px 8px;
        border-radius: 3px;
        font-size: 11px;
        font-weight: 600;
        white-space: nowrap;
    }

    .badge-update {
        background: var(--badge-update-bg);
        color: var(--badge-update-fg);
    }

    .badge-current {
        background: var(--badge-current-bg);
        color: var(--badge-current-fg);
    }

    .badge-older {
        background: var(--badge-older-bg);
        color: var(--badge-older-fg);
    }

    /* Board-firmware status palette (overview + topology rows). */
    .badge-update  { background: var(--badge-update-bg); color: var(--badge-update-fg); }
    .badge-current { background: var(--badge-current-bg); color: var(--badge-current-fg); }
    .badge-newer   { background: color-mix(in srgb, var(--accent) 18%, transparent); color: var(--accent); }
    .badge-unknown { background: var(--bg-raised); color: var(--text-dim); }

    /* Board Firmware overview */
    .fw-issues { background: color-mix(in srgb, var(--warning) 12%, transparent); border: 1px solid var(--warning); color: var(--text); border-radius: 4px; padding: 7px 10px; font-size: 12px; margin-bottom: 10px; }
    .fw-table th, .fw-table td { vertical-align: middle; }
    .fw-table tr.fw-stale td { background: color-mix(in srgb, var(--warning) 7%, transparent); }
    .role-tag { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; padding: 1px 5px; border-radius: 3px; margin-left: 6px; background: var(--bg-raised); color: var(--text-dim); border: 1px solid var(--border); }
    .role-tag.exp { color: var(--accent); border-color: color-mix(in srgb, var(--accent) 40%, var(--border)); }
    .fw-chip { font-size: 11px; padding: 1px 6px; border-radius: 3px; background: var(--bg-raised); color: var(--text); border: 1px solid var(--border); }
    .btn-update { background: var(--badge-update-bg); color: var(--badge-update-fg); border: 1px solid var(--warning); border-radius: 4px; padding: 2px 8px; cursor: pointer; font-size: 11px; font-weight: 600; }
    .btn-update:hover { filter: brightness(1.1); }
    .small { font-size: 11px; }

    .text-dim {
        color: var(--text-dim);
    }

    /* ─── Capabilities + Topology ─── */

    .cap-chips {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
    }

    .cap-chip {
        font-family: var(--font-mono);
        font-size: 11px;
        padding: 3px 9px;
        border-radius: 12px;
        background: var(--badge-current-bg, var(--bg-secondary));
        color: var(--badge-current-fg, var(--text));
        border: 1px solid var(--border);
        white-space: nowrap;
    }

    .board-block {
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 8px 12px;
        margin-bottom: 8px;
    }

    .board-head {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 6px;
    }

    .board-name {
        font-weight: 600;
        color: var(--text-bright);
    }

    .batt-pill {
        margin-left: auto;
        font-family: var(--font-mono);
        font-size: 11px;
        color: var(--success);
    }

    .batt-pill.warn {
        color: var(--error);
    }

    .port-rows {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
    }

    .port-pill {
        font-family: var(--font-mono);
        font-size: 11px;
        padding: 2px 8px;
        border-radius: 3px;
        background: var(--bg-secondary);
        color: var(--text-dim);
    }

    /* ─── Flash Controls ─── */

    .flash-controls {
        display: flex;
        flex-direction: column;
        gap: 10px;
    }

    .control-row {
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .control-row label {
        width: 72px;
        font-size: 13px;
        color: var(--text-dim);
        flex-shrink: 0;
    }

    .control-row select,
    .control-row input[type="text"] {
        flex: 1;
        padding: 6px 10px;
        font-size: 13px;
        font-family: var(--font-mono);
        background: var(--bg-secondary);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        outline: none;
    }

    .control-row select:focus,
    .control-row input[type="text"]:focus {
        border-color: var(--accent);
    }

    .release-detail {
        font-size: 12px;
        color: var(--text-dim);
        padding-left: 82px;
        display: flex;
        align-items: center;
        gap: 6px;
    }

    .detail-label {
        color: var(--text-dim);
        opacity: 0.7;
    }

    .detail-sep {
        opacity: 0.3;
    }

    /* ─── Release Notes ─── */

    .release-notes {
        margin-top: 8px;
        margin-left: 82px;
        border: 1px solid var(--border);
        border-radius: 4px;
        background: var(--bg-secondary);
    }

    .release-notes summary {
        padding: 6px 12px;
        font-size: 12px;
        font-weight: 600;
        color: var(--text-dim);
        cursor: pointer;
        user-select: none;
    }

    .release-notes summary:hover {
        color: var(--text);
    }

    .release-notes[open] summary {
        border-bottom: 1px solid var(--border);
    }

    .release-notes-body {
        padding: 8px 12px;
        font-size: 12px;
        line-height: 1.6;
        max-height: 240px;
        overflow-y: auto;
        color: var(--text);
    }

    .rn-heading {
        font-size: 13px;
        font-weight: 600;
        color: var(--text-bright);
        margin: 4px 0 2px;
    }

    .rn-subheading {
        font-size: 12px;
        font-weight: 600;
        color: var(--accent);
        margin: 4px 0 2px;
    }

    .rn-bullet {
        padding-left: 8px;
        margin: 1px 0;
    }

    .rn-table {
        font-family: var(--font-mono);
        font-size: 11px;
        color: var(--text-dim);
        margin: 0;
    }

    .rn-text {
        margin: 2px 0;
    }

    .release-notes-body br {
        display: block;
        content: "";
        margin: 2px 0;
    }

    .option-row {
        display: flex;
        gap: 16px;
        padding-left: 82px;
    }

    .option-row label {
        font-size: 12px;
        color: var(--text);
        display: flex;
        align-items: center;
        gap: 4px;
        cursor: pointer;
        user-select: none;
    }

    .option-row input[type="checkbox"] {
        accent-color: var(--accent);
    }

    .action-row {
        padding-left: 82px;
        padding-top: 4px;
    }

    .btn-flash {
        padding: 8px 24px;
        font-size: 13px;
        font-weight: 600;
        background: var(--accent);
        color: var(--bg);
        border: none;
        border-radius: 4px;
        cursor: pointer;
        transition: opacity 0.15s;
    }

    .btn-flash:hover:not(:disabled) {
        opacity: 0.85;
    }

    .btn-flash:disabled {
        opacity: 0.4;
        cursor: not-allowed;
    }

    /* ── Audio asset cache table ─────────────────────────────────── */
    .preload-summary {
        margin: 6px 0 10px;
    }
    .preload-bar-track {
        height: 6px;
        background: var(--bg-elev);
        border: 1px solid var(--border);
        border-radius: 3px;
        overflow: hidden;
        margin-bottom: 4px;
    }
    .preload-bar-fill {
        height: 100%;
        background: var(--accent);
        transition: width 0.3s ease;
    }
    .preload-summary-text {
        font-size: 11px;
        color: var(--text-dim);
    }
    .preload-table {
        font-size: 11px;
    }
    .preload-table th {
        font-weight: 600;
        text-align: left;
        padding: 3px 8px;
        color: var(--text-dim);
        border-bottom: 1px solid var(--border);
    }
    .preload-table td {
        padding: 2px 8px;
    }
    .preload-table td.mono {
        font-family: var(--font-mono);
    }
    .preload-ready   { color: var(--success); }
    .preload-loading { color: var(--warning); }
    .preload-failed  { color: var(--error); font-weight: 600; }
    .owner-chip {
        display: inline-block;
        padding: 1px 6px;
        margin-right: 4px;
        background: var(--bg-elev);
        border: 1px solid var(--border);
        border-radius: 3px;
        font-size: 10px;
        color: var(--text);
    }
    .owner-lru {
        font-style: italic;
        color: var(--text-dim);
    }

    .mem-hint {
        font-size: 10px;
        color: var(--text-dim);
        margin-left: 8px;
        font-style: italic;
    }
</style>
