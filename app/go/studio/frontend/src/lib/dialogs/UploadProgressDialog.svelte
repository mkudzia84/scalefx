<!-- ScaleFX Studio — Upload Progress Dialog -->
<!-- Modal popup subscribed to "fs:progress" Wails events. Used for generic
     File Manager uploads AND config uploads (Save button on board tabs). -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { EventsOn } from '../../../wailsjs/runtime/runtime'

    /** Human-readable operation label shown in the header ("Upload", "Save
     *  Configuration", …). Caller sets this when opening the dialog. */
    export let title: string = 'Upload'
    /** Controls visibility. Bindable from the parent. */
    export let open: boolean = false
    /** Fired once the final "done" or "error" event lands. The parent can
     *  close itself, refresh lists, etc. */
    export let onFinished: ((ok: boolean, msg: string) => void) | null = null

    type Phase = '' | 'uploading' | 'downloading' | 'done' | 'error'

    let phase: Phase = ''
    let path = ''
    let sent = 0
    let total = 0
    let md5Match: boolean | null = null
    let speedKBs = 0
    let errorMsg = ''

    // Batch counters (populated only by FsUploadBatch; single-file ops
    // leave batchTotal=0, which hides the overall bar.)
    let batchIndex = 0
    let batchTotal = 0
    let batchBytesSent = 0
    let batchBytesTotal = 0

    let unsub: (() => void) | null = null

    $: pct = total > 0 ? Math.min(100, (sent / total) * 100) : 0
    $: batchPct = batchBytesTotal > 0
        ? Math.min(100, (batchBytesSent / batchBytesTotal) * 100)
        : 0
    $: isBatch = batchTotal > 1
    $: isDone = phase === 'done'
    $: isError = phase === 'error'
    $: isRunning = phase === 'uploading' || phase === 'downloading'

    function reset() {
        phase = ''
        path = ''
        sent = 0
        total = 0
        md5Match = null
        speedKBs = 0
        errorMsg = ''
        batchIndex = 0
        batchTotal = 0
        batchBytesSent = 0
        batchBytesTotal = 0
    }

    // Re-arm subscription every time the dialog is opened so it catches
    // events emitted immediately after the caller sets `open = true`.
    let _wasOpen = false
    $: if (open && !_wasOpen) { _wasOpen = true; reset(); subscribe() }
    $: if (!open && _wasOpen) { _wasOpen = false; unsub?.(); unsub = null }

    function subscribe() {
        unsub?.()
        unsub = EventsOn('fs:progress', (p: any) => {
            phase = (p.phase || '') as Phase
            if (p.path) path = p.path
            if (typeof p.sent === 'number') sent = p.sent
            if (typeof p.total === 'number') total = p.total
            if (typeof p.md5Match === 'boolean') md5Match = p.md5Match
            if (typeof p.speedKBs === 'number') speedKBs = p.speedKBs
            if (typeof p.batchIndex === 'number') batchIndex = p.batchIndex
            if (typeof p.batchTotal === 'number') batchTotal = p.batchTotal
            if (typeof p.batchBytesSent === 'number') batchBytesSent = p.batchBytesSent
            if (typeof p.batchBytesTotal === 'number') batchBytesTotal = p.batchBytesTotal
            if (phase === 'error') {
                errorMsg = p.error || 'Upload failed'
                onFinished?.(false, errorMsg)
            } else if (phase === 'done') {
                onFinished?.(true, '')
            }
        })
    }

    onMount(() => { if (open) subscribe() })
    onDestroy(() => { unsub?.() })

    function fmtBytes(n: number): string {
        if (n < 1024) return n + ' B'
        if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB'
        if (n < 1024 * 1024 * 1024) return (n / 1048576).toFixed(1) + ' MB'
        return (n / 1073741824).toFixed(2) + ' GB'
    }

    function close() {
        if (isRunning) return
        open = false
    }

    function handleKeydown(e: KeyboardEvent) {
        if (!open) return
        if (e.key === 'Escape' && !isRunning) close()
    }

    function verbFor(p: Phase): string {
        switch (p) {
            case 'uploading':   return 'Uploading'
            case 'downloading': return 'Downloading'
            case 'done':        return 'Complete'
            case 'error':       return 'Failed'
            default:            return 'Preparing…'
        }
    }
</script>

<svelte:window on:keydown={handleKeydown} />

{#if open}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="up-backdrop" on:click|self={close}>
        <div class="up-dialog">
            <header class="up-header">
                <h2>
                    {#if isDone}<span class="up-icon ok">✓</span>{title} — Complete
                    {:else if isError}<span class="up-icon err">✗</span>{title} — Failed
                    {:else}<span class="up-icon active">⟳</span>{title}
                    {/if}
                </h2>
                {#if !isRunning}
                    <button class="up-close" on:click={close} title="Close">✕</button>
                {/if}
            </header>

            <div class="up-body">
                {#if isBatch}
                    <div class="up-batch-label">
                        Batch — {batchIndex} of {batchTotal} file{batchTotal !== 1 ? 's' : ''}
                    </div>
                    <div class="up-bar">
                        <div class="up-fill up-fill-batch"
                             class:ok={isDone}
                             class:err={isError}
                             style={batchBytesTotal > 0 ? `width: ${batchPct}%` : ''}></div>
                    </div>
                    <div class="up-stats">
                        <span class="up-bytes">{fmtBytes(batchBytesSent)} / {fmtBytes(batchBytesTotal)}</span>
                        <span class="up-pct">{batchPct.toFixed(0)}%</span>
                    </div>
                {/if}

                <div class="up-phase">{verbFor(phase)}{path ? ` · ${path}` : ''}</div>

                <div class="up-bar">
                    <div class="up-fill"
                         class:indeterminate={isRunning && total === 0}
                         class:ok={isDone}
                         class:err={isError}
                         style={total > 0 ? `width: ${pct}%` : ''}></div>
                </div>

                <div class="up-stats">
                    <span class="up-bytes">{fmtBytes(sent)} / {fmtBytes(total)}</span>
                    <span class="up-pct">{pct.toFixed(0)}%</span>
                </div>

                {#if isDone}
                    <div class="up-summary ok">
                        {#if speedKBs > 0}<span>{speedKBs.toFixed(1)} KB/s</span>{/if}
                        {#if md5Match === true}<span>✓ MD5 match</span>
                        {:else if md5Match === false}<span class="warn">⚠ MD5 mismatch</span>{/if}
                    </div>
                {:else if isError}
                    <div class="up-summary err">✗ {errorMsg}</div>
                {/if}
            </div>

            <footer class="up-footer">
                {#if isRunning}
                    <span class="up-hint">Transfer in progress — do not disconnect</span>
                {:else}
                    <button class="up-primary" on:click={close}>Close</button>
                {/if}
            </footer>
        </div>
    </div>
{/if}

<style>
    .up-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.6);
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 220;
    }

    .up-dialog {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 8px;
        box-shadow: 0 12px 48px var(--shadow);
        width: 520px;
        max-width: 90vw;
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    .up-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 16px 20px 10px;
    }
    .up-header h2 {
        margin: 0;
        font-size: 15px;
        font-weight: 600;
        color: var(--text-bright);
        display: flex;
        align-items: center;
        gap: 10px;
    }

    .up-icon.ok     { color: var(--success); }
    .up-icon.err    { color: var(--error); }
    .up-icon.active { color: var(--accent); display: inline-block; animation: up-spin 1.2s linear infinite; }
    @keyframes up-spin { to { transform: rotate(360deg); } }

    .up-close {
        background: none;
        border: none;
        color: var(--text-dim);
        font-size: 16px;
        cursor: pointer;
        padding: 4px 8px;
        border-radius: 4px;
    }
    .up-close:hover { color: var(--text); background: var(--bg-raised); }

    .up-body {
        padding: 4px 20px 16px;
        display: flex;
        flex-direction: column;
        gap: 8px;
    }

    .up-phase {
        font-size: 12px;
        color: var(--text-dim);
        font-family: var(--font-mono);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }

    .up-bar {
        height: 8px;
        background: var(--bg-base);
        border: 1px solid var(--border);
        border-radius: 4px;
        overflow: hidden;
    }
    .up-fill {
        height: 100%;
        background: var(--accent);
        transition: width 0.18s;
    }
    .up-fill.indeterminate {
        width: 30%;
        animation: up-slide 1.5s ease-in-out infinite;
    }
    @keyframes up-slide {
        0%   { margin-left: 0; }
        50%  { margin-left: 70%; }
        100% { margin-left: 0; }
    }
    .up-fill.ok  { background: var(--success); width: 100%; }
    .up-fill.err { background: var(--error);   width: 100%; }
    .up-fill-batch { background: var(--accent-alt, var(--accent)); opacity: 0.9; }

    .up-batch-label {
        font-size: 12px;
        font-weight: 600;
        color: var(--text-bright);
        margin-bottom: 2px;
    }

    .up-stats {
        display: flex;
        justify-content: space-between;
        font-size: 11px;
        font-family: var(--font-mono);
        color: var(--text-dim);
    }

    .up-summary {
        display: flex;
        gap: 14px;
        padding: 8px 10px;
        border-radius: 4px;
        font-size: 12px;
        font-weight: 500;
    }
    .up-summary.ok  { background: color-mix(in srgb, var(--success) 12%, transparent); color: var(--success); }
    .up-summary.err { background: color-mix(in srgb, var(--error)   12%, transparent); color: var(--error); }
    .up-summary .warn { color: var(--warning); }

    .up-footer {
        display: flex;
        align-items: center;
        justify-content: flex-end;
        padding: 12px 20px;
        border-top: 1px solid var(--border);
    }
    .up-hint {
        font-size: 12px;
        color: var(--warning);
        font-style: italic;
    }
    .up-primary {
        padding: 6px 18px;
        font-size: 13px;
        font-weight: 600;
        background: var(--accent);
        color: var(--bg-base);
        border: none;
        border-radius: 4px;
        cursor: pointer;
    }
    .up-primary:hover { opacity: 0.88; }
</style>
