<!-- ScaleFX Studio — Status Bar -->
<!-- Bottom bar showing connection state, controller info, port. -->
<script lang="ts">
    import { connectionInfo } from '../stores'

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
</style>
