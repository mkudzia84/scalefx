<!-- ScaleFX Studio — File Manager Dialog -->
<!-- Navigate / upload / download / delete / mkdir on flash or SD storage.
     Uploads and downloads drive the shared UploadProgressDialog. -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { showFileManager } from '../stores'
    import {
        FsStorageStatus, FsList, FsMkdir, FsDelete,
        FsDownloadToDisk, FsUploadBatch,
    } from '../../../wailsjs/go/main/App'
    import { OnFileDrop, OnFileDropOff } from '../../../wailsjs/runtime/runtime'
    import UploadProgressDialog from './UploadProgressDialog.svelte'

    type Target = 'flash' | 'sd'

    interface Status {
        flashAvailable: boolean
        flashTotal: number; flashUsed: number; flashFree: number
        sdAvailable: boolean
        sdCardMB: number; sdTotalMB: number; sdUsedMB: number; sdFreeMB: number
        sdCardType: string; sdBusMode: string
    }

    interface Entry { name: string; isDir: boolean; size: number }

    let status: Status = {
        flashAvailable: false, flashTotal: 0, flashUsed: 0, flashFree: 0,
        sdAvailable: false, sdCardMB: 0, sdTotalMB: 0, sdUsedMB: 0, sdFreeMB: 0,
        sdCardType: '', sdBusMode: '',
    }

    let target: Target = 'flash'
    let cwd = '/'
    let entries: Entry[] = []
    let loading = false
    let error = ''
    // Upload mode is auto-picked per file by the Go backend
    // (api.PickUploadMode): flash → sync, SD → batch above 64 KB.

    // Progress dialog — shared UploadProgressDialog subscribes to fs:progress.
    let progressOpen = false
    let progressTitle = 'Upload'

    let selected: Entry | null = null
    let showMkdir = false
    let newName = ''

    // ── Drag & drop ──
    let dragOver = false
    let dragCounter = 0

    onMount(async () => {
        await loadStatus()
        await refresh()
        // useDropTarget=true → drops only register on elements with CSS
        // `--wails-drop-target: drop`. Scoped to the file listing body below.
        OnFileDrop((_x, _y, paths) => {
            dragOver = false
            dragCounter = 0
            if (!paths || paths.length === 0) return
            if (!targetAvailable) {
                error = `Drop rejected — ${target} not available.`
                return
            }
            if (progressOpen) {
                error = 'Another transfer is in progress.'
                return
            }
            startBatchUpload(paths)
        }, true)
    })

    onDestroy(() => {
        OnFileDropOff()
    })

    async function startBatchUpload(paths: string[]) {
        progressTitle = paths.length === 1
            ? `Upload to ${cwd}`
            : `Upload ${paths.length} items to ${cwd}`
        progressOpen = true
        try {
            await FsUploadBatch(target, cwd, paths)
        } catch (e: any) {
            if (!/cancelled/i.test(toMsg(e))) error = toMsg(e)
            progressOpen = false
        }
    }

    async function loadStatus() {
        try {
            status = await FsStorageStatus() as Status
            if (target === 'flash' && !status.flashAvailable && status.sdAvailable) target = 'sd'
            if (target === 'sd' && !status.sdAvailable && status.flashAvailable) target = 'flash'
        } catch (e: any) {
            error = toMsg(e)
        }
    }

    async function refresh() {
        if (!isTargetAvailable(target)) { entries = []; return }
        loading = true; error = ''; selected = null
        try {
            const raw = await FsList(target, cwd)
            const list = (raw as Entry[] | null) ?? []
            list.sort((a, b) => {
                if (a.isDir !== b.isDir) return a.isDir ? -1 : 1
                return a.name.localeCompare(b.name)
            })
            entries = list
        } catch (e: any) {
            entries = []
            error = toMsg(e)
        } finally {
            loading = false
        }
    }

    function isTargetAvailable(t: Target): boolean {
        return t === 'flash' ? status.flashAvailable : status.sdAvailable
    }

    async function switchTarget(t: Target) {
        if (t === target) return
        target = t
        cwd = '/'
        await refresh()
    }

    function joinPath(dir: string, name: string): string {
        if (dir === '/' || dir === '') return '/' + name
        return dir.replace(/\/+$/, '') + '/' + name
    }

    async function enter(e: Entry) {
        if (!e.isDir) { selected = e; return }
        cwd = joinPath(cwd, e.name)
        await refresh()
    }

    async function goRoot() { cwd = '/'; await refresh() }

    async function goUp() {
        if (cwd === '/' || cwd === '') return
        const i = cwd.lastIndexOf('/')
        cwd = i <= 0 ? '/' : cwd.slice(0, i)
        await refresh()
    }

    async function goBreadcrumb(i: number) {
        const parts = cwd.split('/').filter(Boolean)
        cwd = '/' + parts.slice(0, i + 1).join('/')
        await refresh()
    }

    async function doMkdir() {
        const n = newName.trim()
        if (!n) return
        try {
            await FsMkdir(target, joinPath(cwd, n))
            showMkdir = false; newName = ''
            await refresh()
        } catch (e: any) {
            error = toMsg(e)
        }
    }

    async function doDelete(entry: Entry = selected as Entry) {
        if (!entry) return
        const label = entry.isDir ? `directory "${entry.name}" and all its contents` : `file "${entry.name}"`
        if (!confirm(`Delete ${label}?`)) return
        try {
            await FsDelete(target, joinPath(cwd, entry.name))
            await refresh()
        } catch (e: any) {
            error = toMsg(e)
        }
    }

    async function doDownload(entry: Entry = selected as Entry) {
        if (!entry || entry.isDir) return
        progressTitle = `Download ${entry.name}`
        progressOpen = true
        try {
            await FsDownloadToDisk(target, joinPath(cwd, entry.name), '')
        } catch (e: any) {
            if (!/cancelled/i.test(toMsg(e))) error = toMsg(e)
            progressOpen = false
        }
    }

    function close() { $showFileManager = false }

    function handleKeydown(e: KeyboardEvent) {
        if (!$showFileManager) return
        if (progressOpen) return
        if (showMkdir) return
        if (e.key === 'Escape') close()
        if (e.key === 'Backspace') goUp()
        if ((e.key === 'Delete' || e.key === 'Del') && selected) doDelete()
        if (e.key === 'F5') { e.preventDefault(); refresh() }
    }

    function fmtBytes(n: number): string {
        if (n < 1024) return n + ' B'
        if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB'
        if (n < 1024 * 1024 * 1024) return (n / 1048576).toFixed(1) + ' MB'
        return (n / 1073741824).toFixed(2) + ' GB'
    }

    function toMsg(e: any): string {
        return (typeof e === 'string' && e) || e?.message || String(e)
    }

    $: breadcrumbs = cwd === '/' ? [] : cwd.split('/').filter(Boolean)
    $: totalCount = entries.length
    $: fileCount = entries.filter(e => !e.isDir).length
    $: dirCount = entries.filter(e => e.isDir).length
    $: bytesUsed = entries.reduce((a, e) => a + (e.isDir ? 0 : e.size), 0)
    // Reactive availability — Svelte template expressions can't see inside
    // isTargetAvailable(), so using the function in markup makes them track
    // only `target` and miss updates when `status` reassigns. These
    // declarations depend on both, guaranteeing re-evaluation.
    $: flashAvailable = status.flashAvailable
    $: sdAvailable = status.sdAvailable
    $: targetAvailable = target === 'flash' ? flashAvailable : sdAvailable
</script>

<svelte:window on:keydown={handleKeydown} />

{#if $showFileManager}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="modal-backdrop fm-backdrop" on:click|self={close}>
        <div class="fm-dialog">

            <!-- ─── Header ─── -->
            <header class="fm-header">
                <h2>📂 File Manager</h2>
                <button class="fm-close" on:click={close} title="Close (Esc)">✕</button>
            </header>

            <!-- ─── Target tabs + storage info ─── -->
            <div class="fm-targets">
                <button class="target" class:active={target === 'flash'}
                        disabled={!status.flashAvailable}
                        on:click={() => switchTarget('flash')}
                        title={status.flashAvailable ? 'Onboard LittleFS flash' : 'Not available'}>
                    <span class="t-icon">⚡</span>
                    <span class="t-name">Flash</span>
                    {#if status.flashAvailable}
                        <span class="t-cap">{fmtBytes(status.flashFree)} free · {fmtBytes(status.flashTotal)} total</span>
                    {:else}
                        <span class="t-na">not available</span>
                    {/if}
                </button>
                <button class="target" class:active={target === 'sd'}
                        disabled={!status.sdAvailable}
                        on:click={() => switchTarget('sd')}
                        title={status.sdAvailable ? `${status.sdCardType} via ${status.sdBusMode}` : 'Not available on this board'}>
                    <span class="t-icon">💾</span>
                    <span class="t-name">SD</span>
                    {#if status.sdAvailable}
                        <span class="t-cap">{status.sdFreeMB} MB free · {status.sdTotalMB} MB total</span>
                        {#if status.sdBusMode}<span class="t-bus">{status.sdBusMode}</span>{/if}
                    {:else}
                        <span class="t-na">not available</span>
                    {/if}
                </button>
            </div>

            <!-- ─── Toolbar: breadcrumb + actions ─── -->
            <div class="fm-toolbar">
                <nav class="breadcrumb" aria-label="Path">
                    <button class="crumb" on:click={goRoot} title="Root">/</button>
                    {#each breadcrumbs as b, i}
                        <span class="crumb-sep">›</span>
                        <button class="crumb" on:click={() => goBreadcrumb(i)}>{b}</button>
                    {/each}
                </nav>
                <div class="tb-actions">
                    <button on:click={goUp} disabled={cwd === '/'} title="Parent directory (Backspace)">↑ Up</button>
                    <button on:click={refresh} title="Refresh (F5)">↻ Refresh</button>
                    <button on:click={() => { showMkdir = true; newName = '' }}
                            disabled={!targetAvailable}
                            title="Create new folder">📁 New Folder</button>
                </div>
            </div>

            <!-- ─── File listing ─── -->
            <!-- svelte-ignore a11y-no-static-element-interactions -->
            <div class="fm-body" class:drag-over={dragOver}
                 on:dragenter|preventDefault={() => { dragCounter++; dragOver = true }}
                 on:dragleave|preventDefault={() => { dragCounter = Math.max(0, dragCounter - 1); if (dragCounter === 0) dragOver = false }}
                 on:dragover|preventDefault={() => {}}
                 on:drop|preventDefault={() => { dragOver = false; dragCounter = 0 }}>
                {#if targetAvailable && !dragOver}
                    <div class="drop-watermark">
                        <span class="wm-icon">⬇</span>Drag &amp; drop files or folders here to upload
                    </div>
                {/if}
                {#if dragOver && targetAvailable}
                    <div class="drop-overlay">
                        <div class="drop-hint">
                            <span class="drop-icon">⬇</span>
                            Drop files / folders here to upload to
                            <code>{cwd}</code>
                        </div>
                    </div>
                {/if}
                {#if !targetAvailable}
                    <div class="empty-msg">
                        {target === 'flash' ? 'Flash' : 'SD card'} is not available on this controller.
                    </div>
                {:else if loading}
                    <div class="empty-msg"><span class="spinner">⟳</span> Loading…</div>
                {:else if entries.length === 0}
                    <div class="empty-msg">(empty directory)</div>
                {:else}
                    <table class="fm-table">
                        <thead>
                            <tr>
                                <th class="col-name">Name</th>
                                <th class="col-size">Size</th>
                                <th class="col-actions">Actions</th>
                            </tr>
                        </thead>
                        <tbody>
                            {#each entries as e (e.name)}
                                <tr class:selected={selected?.name === e.name}
                                    on:click={() => selected = e}
                                    on:dblclick={() => enter(e)}>
                                    <td class="col-name">
                                        <span class="row-icon">{e.isDir ? '📁' : '📄'}</span>
                                        {#if e.isDir}
                                            <button class="name-link"
                                                    on:click|stopPropagation={() => enter(e)}>{e.name}</button>
                                        {:else}
                                            <span>{e.name}</span>
                                        {/if}
                                    </td>
                                    <td class="col-size">{e.isDir ? '—' : fmtBytes(e.size)}</td>
                                    <td class="col-actions">
                                        {#if !e.isDir}
                                            <button class="row-btn"
                                                    on:click|stopPropagation={() => doDownload(e)}
                                                    title="Download to disk">⬇</button>
                                        {/if}
                                        <button class="row-btn danger"
                                                on:click|stopPropagation={() => doDelete(e)}
                                                title={e.isDir ? 'Delete directory (recursive)' : 'Delete file'}>🗑</button>
                                    </td>
                                </tr>
                            {/each}
                        </tbody>
                    </table>
                {/if}
            </div>

            <!-- ─── Error bar ─── -->
            {#if error}
                <div class="fm-error">
                    <span>✗ {error}</span>
                    <button class="small" on:click={() => error = ''}>Dismiss</button>
                </div>
            {/if}

            <!-- ─── Footer ─── -->
            <footer class="fm-footer">
                <div class="fm-stats">
                    {#if targetAvailable && !loading}
                        {totalCount} item{totalCount !== 1 ? 's' : ''}
                        · {dirCount} dir · {fileCount} file{fileCount !== 1 ? 's' : ''}
                        · {fmtBytes(bytesUsed)}
                    {/if}
                </div>
                <button on:click={close}>Close</button>
            </footer>

            <!-- ─── Mkdir prompt ─── -->
            {#if showMkdir}
                <!-- svelte-ignore a11y-click-events-have-key-events -->
                <div class="mkdir-backdrop" on:click|self={() => showMkdir = false}>
                    <div class="mkdir-panel">
                        <h3>New folder in {cwd}</h3>
                        <!-- svelte-ignore a11y-autofocus -->
                        <input type="text" placeholder="folder name" bind:value={newName}
                               on:keydown={(e) => { if (e.key === 'Enter') doMkdir(); if (e.key === 'Escape') showMkdir = false }}
                               autofocus />
                        <div class="mkdir-actions">
                            <button on:click={() => showMkdir = false}>Cancel</button>
                            <button class="primary" on:click={doMkdir} disabled={!newName.trim()}>Create</button>
                        </div>
                    </div>
                </div>
            {/if}

        </div>
    </div>
{/if}

<UploadProgressDialog bind:open={progressOpen} title={progressTitle}
                      onFinished={(ok) => { if (ok) refresh() }} />

<style>
    .fm-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.5);
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 160;
    }

    .fm-dialog {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 8px;
        box-shadow: 0 12px 48px var(--shadow);
        width: 920px;
        max-width: 94vw;
        height: 78vh;
        max-height: 78vh;
        display: flex;
        flex-direction: column;
        overflow: hidden;
        position: relative;
    }

    /* ── Header ── */
    .fm-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 14px 20px;
        border-bottom: 1px solid var(--border);
    }
    .fm-header h2 {
        margin: 0;
        font-size: 15px;
        font-weight: 600;
        color: var(--text-bright);
    }
    .fm-close {
        background: none;
        border: none;
        color: var(--text-dim);
        font-size: 16px;
        cursor: pointer;
        padding: 4px 8px;
        border-radius: 4px;
    }
    .fm-close:hover { color: var(--text); background: var(--bg-raised); }

    /* ── Target tabs ── */
    .fm-targets {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 10px 20px;
        border-bottom: 1px solid var(--border);
        background: color-mix(in srgb, var(--bg-raised) 40%, var(--bg-surface));
    }
    .target {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 6px 12px;
        background: var(--bg-raised);
        border: 1px solid var(--border);
        border-radius: 4px;
        color: var(--text);
        font-size: 12px;
        cursor: pointer;
    }
    .target:disabled {
        opacity: 0.45;
        cursor: not-allowed;
    }
    .target.active {
        background: color-mix(in srgb, var(--accent) 20%, var(--bg-raised));
        border-color: var(--accent);
        color: var(--text-bright);
    }
    .t-icon  { font-size: 14px; }
    .t-name  { font-weight: 600; }
    .t-cap   { color: var(--text-dim); font-family: var(--font-mono); font-size: 11px; }
    .t-na    { color: var(--warning); font-style: italic; font-size: 11px; }
    .t-bus   { color: var(--text-dim); font-size: 11px; font-family: var(--font-mono); }


    /* ── Toolbar ── */
    .fm-toolbar {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 8px 20px;
        border-bottom: 1px solid var(--border);
    }
    .breadcrumb {
        display: flex;
        align-items: center;
        gap: 4px;
        flex: 1 1 auto;
        min-width: 0;
        overflow-x: auto;
        white-space: nowrap;
    }
    .crumb {
        background: none;
        border: none;
        padding: 3px 8px;
        font-size: 12px;
        font-family: var(--font-mono);
        color: var(--accent);
        cursor: pointer;
        border-radius: 3px;
    }
    .crumb:hover { background: var(--bg-raised); }
    .crumb-sep   { color: var(--text-dim); font-size: 12px; }

    .tb-actions { display: flex; gap: 6px; flex-shrink: 0; }
    .tb-actions button {
        padding: 5px 10px;
        font-size: 12px;
        background: var(--bg-raised);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        cursor: pointer;
    }
    .tb-actions button:disabled { opacity: 0.5; cursor: not-allowed; }
    .tb-actions button:hover:not(:disabled) {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
    }
    /* Persistent background watermark on the list area — affordance that the
       body is a drop zone. Hidden during drag (overlay takes over) and when
       target is unavailable. */
    .drop-watermark {
        position: absolute;
        left: 0;
        right: 0;
        bottom: 16px;
        text-align: center;
        font-size: 11px;
        font-style: italic;
        letter-spacing: 0.3px;
        color: color-mix(in srgb, var(--accent) 55%, var(--text-dim));
        opacity: 0.7;
        pointer-events: none;
        user-select: none;
        z-index: 0;
    }
    .drop-watermark .wm-icon {
        display: inline-block;
        margin-right: 4px;
        font-size: 12px;
    }

    /* ── Body / listing ── */
    .fm-body {
        flex: 1 1 auto;
        min-height: 0;
        overflow-y: auto;
        background: var(--bg-base);
        position: relative;
        /* Wails scopes OS-level file drops to elements with this CSS var
           (see main.go DragAndDrop options). Drops outside this zone
           are ignored. */
        --wails-drop-target: drop;
    }
    .fm-body.drag-over {
        outline: 2px dashed var(--accent);
        outline-offset: -4px;
    }
    .drop-overlay {
        position: absolute;
        inset: 0;
        background: var(--bg-base);
        opacity: 0.92;
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 5;
        pointer-events: none;
    }
    .drop-hint {
        font-size: 14px;
        color: var(--accent);
        font-weight: 600;
        text-align: center;
        line-height: 1.6;
    }
    .drop-hint code {
        font-family: var(--font-mono);
        background: var(--bg-raised);
        padding: 2px 8px;
        border-radius: 3px;
        margin-left: 4px;
    }
    .drop-icon {
        display: block;
        font-size: 32px;
        margin-bottom: 8px;
    }
    .empty-msg {
        padding: 48px 20px;
        text-align: center;
        color: var(--text-dim);
        font-size: 13px;
    }
    .spinner {
        display: inline-block;
        margin-right: 6px;
        animation: fm-spin 1s linear infinite;
    }
    @keyframes fm-spin { to { transform: rotate(360deg); } }

    .fm-table {
        width: 100%;
        border-collapse: collapse;
        font-size: 13px;
    }
    .fm-table thead th {
        position: sticky;
        top: 0;
        z-index: 1;
        padding: 8px 14px;
        text-align: left;
        font-size: 11px;
        font-weight: 600;
        text-transform: uppercase;
        letter-spacing: 0.5px;
        color: var(--text-dim);
        background: var(--bg-surface);
        border-bottom: 1px solid var(--border);
    }
    .fm-table tbody tr {
        border-bottom: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        cursor: pointer;
    }
    .fm-table tbody tr:hover { background: var(--bg-raised); }
    .fm-table tbody tr.selected {
        background: color-mix(in srgb, var(--accent) 22%, var(--bg-base));
    }
    .fm-table td {
        padding: 6px 14px;
        color: var(--text);
    }
    .col-name    { width: auto; }
    .col-size    { width: 120px; text-align: right; font-family: var(--font-mono); color: var(--text-dim); }
    .col-actions { width: 120px; text-align: right; white-space: nowrap; }

    .row-icon { margin-right: 8px; }
    .name-link {
        background: none;
        border: none;
        color: var(--accent);
        cursor: pointer;
        padding: 0;
        font-size: 13px;
        text-decoration: none;
    }
    .name-link:hover { text-decoration: underline; }

    .row-btn {
        background: transparent;
        border: 1px solid transparent;
        padding: 2px 7px;
        margin-left: 4px;
        border-radius: 3px;
        cursor: pointer;
        font-size: 13px;
        color: var(--text);
    }
    .row-btn:hover {
        background: var(--bg-raised);
        border-color: var(--border);
    }
    .row-btn.danger:hover {
        background: color-mix(in srgb, var(--error) 20%, var(--bg-raised));
        border-color: color-mix(in srgb, var(--error) 50%, var(--border));
        color: var(--error);
    }

    /* ── Error bar ── */
    .fm-error {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 8px 20px;
        background: color-mix(in srgb, var(--error) 10%, var(--bg-raised));
        color: var(--error);
        border-top: 1px solid var(--border);
        font-size: 12px;
    }
    .fm-error button {
        margin-left: auto;
        font-size: 11px;
        padding: 3px 8px;
        background: transparent;
        border: 1px solid currentColor;
        color: inherit;
        border-radius: 3px;
        cursor: pointer;
    }

    /* ── Footer ── */
    .fm-footer {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 10px 20px;
        border-top: 1px solid var(--border);
        flex-shrink: 0;
    }
    .fm-stats {
        flex: 1;
        font-size: 12px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }
    .fm-footer button {
        padding: 6px 16px;
        font-size: 13px;
        background: var(--bg-raised);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        cursor: pointer;
    }
    .fm-footer button:hover {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
    }

    /* ── Mkdir modal ── */
    .mkdir-backdrop {
        position: absolute;
        inset: 0;
        background: rgba(0, 0, 0, 0.5);
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 10;
    }
    .mkdir-panel {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 18px 20px;
        width: 340px;
        max-width: 90%;
    }
    .mkdir-panel h3 {
        margin: 0 0 12px;
        font-size: 13px;
        color: var(--text-bright);
        font-family: var(--font-mono);
    }
    .mkdir-panel input {
        width: 100%;
        padding: 6px 10px;
        background: var(--bg-base);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        font-size: 13px;
        box-sizing: border-box;
    }
    .mkdir-actions {
        display: flex;
        justify-content: flex-end;
        gap: 8px;
        margin-top: 14px;
    }
    .mkdir-actions button {
        padding: 5px 14px;
        font-size: 12px;
        background: var(--bg-raised);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        cursor: pointer;
    }
    .mkdir-actions button.primary {
        background: var(--accent);
        border-color: var(--accent);
        color: var(--bg-base);
        font-weight: 600;
    }
    .mkdir-actions button:disabled { opacity: 0.5; cursor: not-allowed; }

    /* ── Scrollbar ── */
    .fm-body::-webkit-scrollbar { width: 8px; }
    .fm-body::-webkit-scrollbar-track { background: transparent; }
    .fm-body::-webkit-scrollbar-thumb {
        background: var(--scrollbar-thumb);
        border-radius: 4px;
    }
    .fm-body::-webkit-scrollbar-thumb:hover { background: var(--scrollbar-thumb-hover); }
</style>
