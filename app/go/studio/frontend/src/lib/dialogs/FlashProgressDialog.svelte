<!-- ScaleFX Studio — Flash Progress Dialog -->
<!-- Modal popup that shows firmware flashing progress in a large, readable format. -->
<script lang="ts">
    import { onMount, onDestroy, afterUpdate } from 'svelte'
    import {
        showFlashProgress, firmwareRunning, firmwareLogs, flashResult, boardState,
        connectionInfo
    } from '../stores'
    import type { FirmwareProgress } from '../stores'
    import { EventsOn, EventsOff } from '../../../wailsjs/runtime/runtime'

    let logEl: HTMLElement
    let shouldAutoScroll = true
    let reconnecting = false

    // Track current step for the progress header
    $: latestStep = $firmwareLogs.length > 0
        ? $firmwareLogs.filter(l => l.type === 'step').slice(-1)[0]
        : null

    $: progressText = latestStep
        ? `Step ${latestStep.step} of ${latestStep.total}`
        : reconnecting ? 'Reconnecting...'
        : $firmwareRunning ? 'Starting...' : ''

    $: isDone = $flashResult !== null
    $: isSuccess = $flashResult?.success ?? false

    // Subscribe to firmware progress events (after component is mounted)
    onMount(() => {
        EventsOn('firmware:progress', (evt: FirmwareProgress) => {
            firmwareLogs.update(logs => [...logs, evt])
            if (evt.reconnecting) {
                reconnecting = true
            }
            if (evt.done) {
                reconnecting = false
                firmwareRunning.set(false)
                flashResult.set({
                    success: !evt.error,
                    message: evt.message
                })
                // Update boardState based on whether reconnect succeeded
                if (!evt.error && $connectionInfo.connected) {
                    boardState.set('connected')
                } else {
                    boardState.set('disconnected')
                }
            }
        })
    })

    onDestroy(() => {
        EventsOff('firmware:progress')
    })

    afterUpdate(() => {
        if (shouldAutoScroll && logEl) {
            logEl.scrollTop = logEl.scrollHeight
        }
    })

    function onScroll() {
        if (!logEl) return
        const atBottom = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 40
        shouldAutoScroll = atBottom
    }

    function close() {
        if ($firmwareRunning) return // can't close while flashing
        $showFlashProgress = false
        // Reset for next run
        firmwareLogs.set([])
        flashResult.set(null)
    }

    function handleKeydown(e: KeyboardEvent) {
        if (e.key === 'Escape' && !$firmwareRunning) close()
    }

    function icon(type: string): string {
        switch (type) {
            case 'ok':      return '✓'
            case 'error':   return '✗'
            case 'warning': return '⚠'
            case 'step':    return '▶'
            case 'info':
            default:        return 'ℹ'
        }
    }
</script>

<svelte:window on:keydown={handleKeydown} />

<!-- svelte-ignore a11y-click-events-have-key-events -->
<div class="flash-backdrop" on:click|self={() => { if (!$firmwareRunning) close() }}>
    <div class="flash-dialog">
        <!-- Header -->
        <div class="flash-header">
            <div class="flash-title-row">
                <h2>
                    {#if isDone && isSuccess}
                        <span class="header-icon ok">✓</span> Flash Complete
                    {:else if isDone && !isSuccess}
                        <span class="header-icon error">✗</span> Flash Failed
                    {:else if reconnecting}
                        <span class="header-icon active">⟳</span> Reconnecting...
                    {:else}
                        <span class="header-icon active">⟳</span> Flashing Firmware
                    {/if}
                </h2>
                {#if !$firmwareRunning}
                    <button class="close-btn" on:click={close} title="Close">✕</button>
                {/if}
            </div>
            {#if $firmwareRunning && progressText}
                <div class="progress-subtext">{progressText}</div>
            {/if}
        </div>

        <!-- Spinner / status indicator bar -->
        <div class="progress-bar-container">
            {#if $firmwareRunning}
                <div class="progress-bar indeterminate"></div>
            {:else if isDone && isSuccess}
                <div class="progress-bar success"></div>
            {:else if isDone}
                <div class="progress-bar error"></div>
            {/if}
        </div>

        <!-- Log output -->
        <div class="flash-log-container" bind:this={logEl} on:scroll={onScroll}>
            {#if $firmwareLogs.length === 0}
                <div class="log-empty">Waiting for progress...</div>
            {:else}
                {#each $firmwareLogs as log}
                    <div class="log-line {log.type}">
                        <span class="log-icon">{icon(log.type)}</span>
                        <span class="log-msg">
                            {#if log.type === 'step'}
                                <span class="step-badge">[{log.step}/{log.total}]</span>
                            {/if}
                            {log.message}
                        </span>
                    </div>
                {/each}
            {/if}
        </div>

        <!-- Result summary -->
        {#if isDone}
            <div class="flash-result" class:success={isSuccess} class:failure={!isSuccess}>
                <span class="result-icon">{isSuccess ? '✓' : '✗'}</span>
                <span class="result-message">{$flashResult?.message}</span>
            </div>
        {/if}

        <!-- Footer -->
        <div class="flash-footer">
            {#if $firmwareRunning}
                <span class="footer-hint">Do not disconnect the device</span>
            {:else}
                <button class="btn-primary" on:click={close}>Close</button>
            {/if}
        </div>
    </div>
</div>

<style>
    /* ─── Backdrop ─── */
    .flash-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.6);
        display: flex;
        align-items: center;
        justify-content: center;
        z-index: 200;
    }

    /* ─── Dialog ─── */
    .flash-dialog {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 8px;
        box-shadow: 0 12px 48px var(--shadow);
        width: 680px;
        max-width: 90vw;
        max-height: 85vh;
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    /* ─── Header ─── */
    .flash-header {
        padding: 20px 24px 12px;
        flex-shrink: 0;
    }

    .flash-title-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }

    .flash-title-row h2 {
        font-size: 18px;
        font-weight: 600;
        color: var(--text-bright);
        display: flex;
        align-items: center;
        gap: 10px;
        margin: 0;
    }

    .header-icon {
        font-size: 20px;
    }

    .header-icon.ok {
        color: var(--success);
    }

    .header-icon.error {
        color: var(--error);
    }

    .header-icon.active {
        color: var(--accent);
        animation: spin 1.2s linear infinite;
    }

    @keyframes spin {
        from { transform: rotate(0deg); }
        to { transform: rotate(360deg); }
    }

    .close-btn {
        background: none;
        border: none;
        color: var(--text-dim);
        font-size: 18px;
        cursor: pointer;
        padding: 4px 8px;
        border-radius: 4px;
        line-height: 1;
    }

    .close-btn:hover {
        color: var(--text);
        background: var(--bg-raised);
    }

    .progress-subtext {
        font-size: 13px;
        color: var(--text-dim);
        margin-top: 4px;
    }

    /* ─── Progress Bar ─── */
    .progress-bar-container {
        height: 3px;
        background: var(--border);
        flex-shrink: 0;
    }

    .progress-bar {
        height: 100%;
        border-radius: 2px;
    }

    .progress-bar.indeterminate {
        background: var(--accent);
        width: 30%;
        animation: slide 1.5s ease-in-out infinite;
    }

    @keyframes slide {
        0%   { margin-left: 0; width: 30%; }
        50%  { margin-left: 35%; width: 40%; }
        100% { margin-left: 70%; width: 30%; }
    }

    .progress-bar.success {
        background: var(--success);
        width: 100%;
        transition: width 0.3s;
    }

    .progress-bar.error {
        background: var(--error);
        width: 100%;
        transition: width 0.3s;
    }

    /* ─── Log Area ─── */
    .flash-log-container {
        flex: 1;
        min-height: 200px;
        max-height: 420px;
        overflow-y: auto;
        padding: 12px 20px;
        font-family: var(--font-mono);
        font-size: 13px;
        line-height: 1.7;
        background: var(--bg-base);
        border-top: 1px solid var(--border);
        border-bottom: 1px solid var(--border);
    }

    .log-empty {
        color: var(--text-dim);
        font-style: italic;
        padding: 20px 0;
        text-align: center;
    }

    .log-line {
        display: flex;
        align-items: baseline;
        gap: 10px;
        padding: 2px 0;
    }

    .log-icon {
        flex-shrink: 0;
        width: 16px;
        text-align: center;
        font-size: 12px;
    }

    .log-msg {
        word-break: break-word;
    }

    .log-line.ok .log-icon {
        color: var(--success);
    }

    .log-line.ok .log-msg {
        color: var(--success);
    }

    .log-line.error .log-icon {
        color: var(--error);
    }

    .log-line.error .log-msg {
        color: var(--error);
    }

    .log-line.warning .log-icon {
        color: var(--warning);
    }

    .log-line.warning .log-msg {
        color: var(--warning);
    }

    .log-line.step .log-icon {
        color: var(--accent);
    }

    .log-line.step .log-msg {
        color: var(--accent);
        font-weight: 600;
    }

    .log-line.info .log-icon {
        color: var(--text-dim);
    }

    .log-line.info .log-msg {
        color: var(--text);
    }

    .step-badge {
        opacity: 0.6;
        margin-right: 4px;
    }

    /* ─── Result Banner ─── */
    .flash-result {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 12px 24px;
        font-size: 14px;
        font-weight: 600;
        flex-shrink: 0;
    }

    .flash-result.success {
        background: rgba(34, 197, 94, 0.08);
        color: var(--success);
    }

    .flash-result.failure {
        background: rgba(244, 71, 71, 0.08);
        color: var(--error);
    }

    .result-icon {
        font-size: 18px;
    }

    /* ─── Footer ─── */
    .flash-footer {
        padding: 14px 24px;
        display: flex;
        align-items: center;
        justify-content: flex-end;
        flex-shrink: 0;
    }

    .footer-hint {
        font-size: 12px;
        color: var(--warning);
        font-style: italic;
    }

    .btn-primary {
        padding: 8px 24px;
        font-size: 13px;
        font-weight: 600;
        background: var(--accent);
        color: var(--bg-base);
        border: none;
        border-radius: 4px;
        cursor: pointer;
        transition: opacity 0.15s;
    }

    .btn-primary:hover {
        opacity: 0.85;
    }

    /* ─── Scrollbar ─── */
    .flash-log-container::-webkit-scrollbar {
        width: 8px;
    }

    .flash-log-container::-webkit-scrollbar-track {
        background: transparent;
    }

    .flash-log-container::-webkit-scrollbar-thumb {
        background: var(--scrollbar-thumb);
        border-radius: 4px;
    }

    .flash-log-container::-webkit-scrollbar-thumb:hover {
        background: var(--scrollbar-thumb-hover);
    }
</style>
