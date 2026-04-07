<!-- ScaleFX Studio — Firmware Tab -->
<!-- Displays firmware and hardware info for the connected board. -->
<script lang="ts">
    import { connectionInfo, slaveInfo } from '../stores'
</script>

<div class="firmware-tab">
    <h2>Firmware Information</h2>

    <section class="info-section">
        <h3>Connected Board</h3>
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
    </section>

    {#if $connectionInfo.controllerType === 'hubfx' && $slaveInfo.length > 0}
        <section class="info-section">
            <h3>Slave Controllers</h3>
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
</div>

<script context="module" lang="ts">
    function formatBytes(bytes: number): string {
        if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(1)} MB`
        if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`
        return `${bytes} B`
    }
</script>

<style>
    .firmware-tab {
        padding: 24px;
        max-width: 640px;
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
</style>
