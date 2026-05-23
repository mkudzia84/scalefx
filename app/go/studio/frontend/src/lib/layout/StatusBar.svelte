<!-- ScaleFX Studio — Status Bar -->
<!-- Bottom bar showing connection state, controller info, port,
     AND the global config dirty / validation summary (Rule 46) — a
     second always-visible reminder mirroring the top ConfigToolbar
     pill, so the operator catches "unapplied changes" even when
     they're focused at the bottom of a long form. -->
<script lang="ts">
    import { connectionInfo } from '../stores'
    import { anyDirty, anyErrors, dirtyLabels, errorLabels } from '../dirty-registry'

    $: ci = $connectionInfo

    $: statusText = ci.connected
        ? ci.initialized
            ? `${ci.controllerName} v${ci.firmwareVersion} (build ${ci.build})`
            : `Connected — ${ci.port}`
        : 'Disconnected'

    $: statusColor = ci.initialized ? 'var(--success)' : ci.connected ? 'var(--warning)' : 'var(--text-dim)'
</script>

<div class="status-bar">
    <div class="status-left">
        <span class="status-dot" style="background: {statusColor}"></span>
        <span class="status-text">{statusText}</span>
    </div>
    <div class="status-right">
        {#if ci.connected}
            {#if $errorLabels.length > 0}
                <span class="cfg-flag err"
                      title="Resolve validation errors: {$errorLabels.join(', ')}">
                    ⚠ errors: {$errorLabels.join(', ')}
                </span>
            {:else if $dirtyLabels.length > 0}
                <span class="cfg-flag dirty"
                      title="Unapplied changes — press Apply (top bar): {$dirtyLabels.join(', ')}">
                    ● unapplied: {$dirtyLabels.join(', ')}
                </span>
            {:else}
                <span class="cfg-flag in-sync" title="Every config matches what's on the device.">
                    ✓ in sync
                </span>
            {/if}
        {/if}
        {#if ci.port && ci.connected}
            <span class="status-port">{ci.port}</span>
        {/if}
    </div>
</div>

<style>
    .status-bar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        height: 24px;
        padding: 0 12px;
        background: var(--bg-surface);
        border-top: 1px solid var(--border);
        font-size: 12px;
        color: var(--text-dim);
        flex-shrink: 0;
    }

    .status-left {
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .status-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        flex-shrink: 0;
    }

    .status-text {
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }

    .status-right {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .status-port {
        font-family: var(--font-mono);
        font-size: 11px;
    }

    .cfg-flag {
        font-family: var(--font-mono);
        font-size: 11px;
        padding: 1px 8px;
        border-radius: 3px;
        border: 1px solid var(--border);
        white-space: nowrap;
        max-width: 360px;
        overflow: hidden;
        text-overflow: ellipsis;
    }
    .cfg-flag.in-sync { color: var(--text-dim); }
    .cfg-flag.dirty {
        color: var(--accent);
        background: color-mix(in srgb, var(--accent) 12%, transparent);
        border-color: color-mix(in srgb, var(--accent) 50%, var(--border));
    }
    .cfg-flag.err {
        color: var(--error);
        background: color-mix(in srgb, var(--error) 12%, transparent);
        border-color: color-mix(in srgb, var(--error) 50%, var(--border));
        font-weight: 600;
    }
</style>
