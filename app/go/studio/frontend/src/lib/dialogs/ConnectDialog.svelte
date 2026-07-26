<!-- ScaleFX Studio — Connect Dialog -->
<!-- Popup overlay for selecting a serial port. Shown on startup and on disconnect. -->
<!-- Auto-refreshes when COM ports change (via ports:changed event from Go backend). -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import {
        connectPopupOpen, connectionInfo, boardState,
        pushConsoleMessage, availablePorts
    } from '../stores'
    import type { PortInfo } from '../stores'
    import { ListPorts, Connect } from '../../../wailsjs/go/main/App'
    import { EventsOn, EventsOff } from '../../../wailsjs/runtime/runtime'
    import logoUrl from '../../assets/images/logo.jpg'

    let selectedPort = ''
    let loading = false
    let connecting = false
    let errorMsg = ''

    let ports: PortInfo[] = []
    $: ports = $availablePorts

    onMount(() => {
        refresh()
        // Listen for port changes from Go watcher
        EventsOn('ports:changed', (newPorts: PortInfo[]) => {
            $availablePorts = newPorts
            // Auto-select if only one
            if (newPorts.length === 1 && !connecting) {
                selectedPort = newPorts[0].name
            }
        })
    })

    onDestroy(() => {
        EventsOff('ports:changed')
    })

    async function refresh() {
        loading = true
        errorMsg = ''
        try {
            const result = await ListPorts()
            $availablePorts = result
            if (result.length === 1) {
                selectedPort = result[0].name
            }
        } catch (e) {
            $availablePorts = []
        }
        loading = false
    }

    async function doConnect(portOverride?: string) {
        const port = portOverride ?? selectedPort
        if (!port) return
        connecting = true
        errorMsg = ''
        try {
            const info = await Connect(port)
            $connectionInfo = info
            if (info.connected) {
                $connectPopupOpen = false
                $boardState = 'connected'
            } else {
                errorMsg = 'Connection failed — device did not respond'
            }
        } catch (e) {
            errorMsg = `Connect failed: ${e}`
            pushConsoleMessage('error', `Connect failed: ${e}`)
        }
        connecting = false
    }

    // The dialog can only be dismissed once a board is CONNECTED — there is
    // nothing usable behind it without a connection (an empty device model
    // renders as a blank "stuck" UI with no ports).  Not-connected ⇒ Escape /
    // backdrop-click / ✕ do nothing; the only way forward is to Connect.  When
    // already connected (re-opened to switch ports) closing works normally.
    $: canDismiss = !!$connectionInfo.connected

    function handleKeydown(e: KeyboardEvent) {
        if (e.key === 'Enter' && selectedPort && !connecting) doConnect()
        if (e.key === 'Escape') dismiss()
    }

    // Wrap doConnect for inline button handlers — the click handler signature
    // is `MouseEventHandler`, not `(portOverride?: string)`.
    const onClickConnect = () => { void doConnect() }

    function dismiss() {
        if (!canDismiss) return
        $connectPopupOpen = false
    }
</script>

<svelte:window on:keydown={handleKeydown} />

<div class="connect-overlay" on:click|self={dismiss}>
    <div class="connect-card">
        {#if canDismiss}
            <button class="close-btn" on:click={dismiss} title="Close">✕</button>
        {/if}
        <div class="connect-header">
            <img class="connect-logo" src={logoUrl} alt="ScaleFX" />
            <p class="connect-subtitle">Select a board to connect</p>
        </div>

        <div class="port-list">
            {#if loading}
                <div class="port-empty">
                    <span class="spinner"></span>
                    Scanning ports...
                </div>
            {:else if ports.length === 0}
                <div class="port-empty">
                    <span class="port-empty-icon">⊘</span>
                    No serial ports detected
                    <span class="port-hint">Connect a ScaleFX board via USB</span>
                </div>
            {:else}
                {#each ports as port}
                    <button
                        class="port-item"
                        class:selected={selectedPort === port.name}
                        on:click={() => { selectedPort = port.name; errorMsg = '' }}
                        on:dblclick={onClickConnect}
                    >
                        <span class="port-icon">⎔</span>
                        <span class="port-name">{port.name}</span>
                        {#if port.description}
                            <span class="port-desc">{port.description}</span>
                        {/if}
                    </button>
                {/each}
            {/if}
        </div>

        {#if errorMsg}
            <div class="connect-error">{errorMsg}</div>
        {/if}

        <div class="connect-actions">
            <button class="btn-link" on:click={refresh} disabled={loading}>
                Refresh
            </button>
            <button class="primary" on:click={onClickConnect} disabled={!selectedPort || connecting}>
                {connecting ? 'Connecting...' : 'Connect'}
            </button>
        </div>
    </div>
</div>

<style>
    .connect-overlay {
        position: fixed;
        inset: 0;
        display: flex;
        align-items: center;
        justify-content: center;
        background: rgba(0, 0, 0, 0.6);
        backdrop-filter: blur(4px);
        z-index: 200;
    }

    .connect-card {
        position: relative;
        width: 400px;
        max-width: 90vw;
        background: var(--bg-base);
        border-radius: 10px;
        padding: 28px;
        box-shadow: 0 8px 32px rgba(0, 0, 0, 0.5);
    }

    .close-btn {
        position: absolute;
        top: 10px;
        right: 12px;
        background: none;
        border: none;
        color: var(--text-dim);
        font-size: 16px;
        cursor: pointer;
        padding: 4px 6px;
        border-radius: 4px;
        line-height: 1;
    }

    .close-btn:hover {
        color: var(--text-bright);
        background: var(--bg-raised);
    }

    .connect-header {
        text-align: center;
        margin-bottom: 24px;
    }

    .connect-logo {
        width: 100%;
        max-height: 160px;
        object-fit: cover;
        object-position: center;
        margin: 0 auto 16px;
        display: block;
        border-radius: 6px;
        box-shadow: var(--logo-shadow);
    }

    .connect-header h1 {
        font-size: 20px;
        font-weight: 600;
        color: var(--text-bright);
        margin-bottom: 4px;
    }

    .connect-subtitle {
        font-size: 13px;
        color: var(--text-dim);
    }

    .port-list {
        max-height: 240px;
        overflow-y: auto;
        border: 1px solid var(--border);
        border-radius: 6px;
        background: var(--bg-surface);
        margin-bottom: 12px;
    }

    .port-empty {
        padding: 32px;
        text-align: center;
        color: var(--text-dim);
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 8px;
    }

    .port-empty-icon {
        font-size: 24px;
        opacity: 0.4;
    }

    .port-hint {
        font-size: 11px;
        opacity: 0.6;
    }

    .spinner {
        display: inline-block;
        width: 16px;
        height: 16px;
        border: 2px solid var(--border);
        border-top-color: var(--accent);
        border-radius: 50%;
        animation: spin 0.8s linear infinite;
    }

    @keyframes spin {
        to { transform: rotate(360deg); }
    }

    .port-item {
        display: flex;
        align-items: center;
        gap: 10px;
        width: 100%;
        padding: 10px 14px;
        border: none;
        border-radius: 0;
        background: transparent;
        color: var(--text);
        cursor: pointer;
        text-align: left;
        font-family: var(--font-mono);
        font-size: 13px;
        transition: background 0.1s;
    }

    .port-item:hover {
        background: var(--bg-raised);
    }

    .port-item.selected {
        background: var(--accent);
        color: #fff;
    }

    .port-icon {
        font-size: 16px;
        opacity: 0.6;
    }

    .port-name {
        flex-shrink: 0;
    }

    .port-desc {
        font-family: var(--font-sans, sans-serif);
        font-size: 11px;
        opacity: 0.55;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .connect-error {
        padding: 8px 12px;
        margin-bottom: 12px;
        background: rgba(244, 71, 71, 0.1);
        border: 1px solid var(--error);
        border-radius: 4px;
        color: var(--error);
        font-size: 12px;
    }

    .connect-actions {
        display: flex;
        justify-content: space-between;
        align-items: center;
    }

    .btn-link {
        background: none;
        border: none;
        color: var(--accent);
        cursor: pointer;
        font-size: 13px;
        padding: 4px 8px;
    }

    .btn-link:hover {
        text-decoration: underline;
    }

    .btn-link:disabled {
        opacity: 0.4;
        cursor: default;
        text-decoration: none;
    }
</style>
