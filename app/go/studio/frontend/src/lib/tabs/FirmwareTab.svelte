<!-- ScaleFX Studio — Firmware Tab -->
<!-- Displays connected board info, available GitHub releases, and flash controls. -->
<script lang="ts">
    import { connectionInfo, slaveInfo, firmwareTargets, firmwareRunning, firmwareLogs, boardState, connectPopupOpen, availableReleases, showFlashProgress, flashResult } from '../stores'
    import type { FirmwareTarget, ReleaseInfo } from '../stores'
    import { GetFirmwareTargets, GetReleases, FlashFromRelease } from '../../../wailsjs/go/main/App'
    import { onMount, tick } from 'svelte'

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
                <tr><td class="label">Free RAM</td><td>{$connectionInfo.freeRAM ? formatBytes($connectionInfo.freeRAM) : '—'}</td></tr>
                <tr><td class="label">Port</td><td class="mono">{$connectionInfo.port || '—'}</td></tr>
            </table>
        {:else}
            <p class="no-data">No board connected</p>
        {/if}
    </section>

    {#if $connectionInfo.controllerType === 'hubfx' && $slaveInfo.length > 0}
        <section class="info-section">
            <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="1" y="4" width="22" height="16" rx="2" ry="2"/><line x1="1" y1="10" x2="23" y2="10"/></svg> Slave Controllers</h3>
            <table class="info-table">
                <thead>
                    <tr>
                        <th>Type</th>
                        <th>Name</th>
                        <th>Status</th>
                    </tr>
                </thead>
                <tbody>
                    {#each $slaveInfo as slave}
                        <tr>
                            <td>{slave.type}</td>
                            <td class="mono">{slave.name || '—'}</td>
                            <td>
                                {#if slave.ready}
                                    <span class="status ready">● Ready</span>
                                {:else if slave.connected}
                                    <span class="status connected">● Connected</span>
                                {:else}
                                    <span class="status disconnected">○ Not Connected</span>
                                {/if}
                            </td>
                        </tr>
                    {/each}
                </tbody>
            </table>
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

    .status {
        font-size: 12px;
    }

    .status.ready {
        color: var(--success);
    }

    .status.connected {
        color: var(--warning);
    }

    .status.disconnected {
        color: var(--text-dim);
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

    .text-dim {
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
</style>
