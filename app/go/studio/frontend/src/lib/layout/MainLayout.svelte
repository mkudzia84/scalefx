<!-- ScaleFX Studio — Main Layout -->
<!-- The main application view shown after connecting. Tab bar + content area + console panel + status bar. -->
<script lang="ts">
    import TabBar from './TabBar.svelte'
    import StatusBar from './StatusBar.svelte'
    import ConfigToolbar from './ConfigToolbar.svelte'
    import ConsolePanel from '../dialogs/ConsoleDialog.svelte'
    import FirmwareTab from '../tabs/FirmwareTab.svelte'
    import IoTab from '../tabs/IoTab.svelte'
    import EnginePanel from '../tabs/EnginePanel.svelte'
    import GunFxPanel from '../tabs/GunFxPanel.svelte'
    import LightingTab from '../tabs/LightingTab.svelte'
    import GearLandingTab from '../tabs/GearLandingTab.svelte'
    import GearDiagnosticsTab from '../tabs/GearDiagnosticsTab.svelte'
    import DomainTab from '../tabs/DomainTab.svelte'
    import { showConsole, activeTab, connectionInfo } from '../stores'
    import { studioTabs } from '../devicemodel'
    import { SetInputLiveView } from '../../../wailsjs/go/main/App'
    import { diag } from '../diag'

    // The tab strip is derived from the device model (see devicemodel.ts):
    // two fixed setup tabs (Ports & Roles, Inputs), one tab per
    // capability-available functional domain, then Firmware.  This pane
    // renders whichever tab `activeTab` points at.
    $: tabs = $studioTabs
    $: current = tabs[Math.min($activeTab, tabs.length - 1)] ?? tabs[0]

    // Subscribe-on-view: the hub's live-channel wire broadcast is OFF unless a
    // tab that actually renders the colourful channel bars is on screen.  The
    // firmware feeds its effect dispatcher locally regardless, so leaving the
    // wire stream off when we're not viewing it costs nothing and stops a
    // continuous 50 Hz flood on every host RPC.  Tabs that consume the stream:
    // Inputs (io), Engine, Gun, Lighting — see app_input.go SetInputLiveView.
    const LIVE_TAB_KINDS = new Set(['io', 'engine', 'gun', 'lighting'])
    let liveApplied: boolean | null = null
    $: wantLive = !!$connectionInfo.connected && LIVE_TAB_KINDS.has(current?.kind)
    $: if (wantLive !== liveApplied) {
        liveApplied = wantLive
        SetInputLiveView(wantLive).catch((e) =>
            diag.warn('FE.INPUT', 'SetInputLiveView failed', { want: wantLive, err: String(e) }))
    }

    // --- Resizable console pane ---
    let consoleWidthPct = 40          // percentage of main-body width
    const MIN_PCT = 15
    const MAX_PCT = 70
    let dragging = false
    let mainBodyEl: HTMLDivElement

    function onGripDown(e: MouseEvent) {
        e.preventDefault()
        dragging = true
        window.addEventListener('mousemove', onGripMove)
        window.addEventListener('mouseup', onGripUp)
    }

    function onGripMove(e: MouseEvent) {
        if (!dragging || !mainBodyEl) return
        const rect = mainBodyEl.getBoundingClientRect()
        const xFromRight = rect.right - e.clientX
        let pct = (xFromRight / rect.width) * 100
        pct = Math.max(MIN_PCT, Math.min(MAX_PCT, pct))
        consoleWidthPct = pct
    }

    function onGripUp() {
        dragging = false
        window.removeEventListener('mousemove', onGripMove)
        window.removeEventListener('mouseup', onGripUp)
    }
</script>

<div class="main-layout">
    <TabBar />
    <!-- Config Apply/dirty/validation is HubFX-only: every DirtySource is a
         /hubfx.yaml or effect config the master owns. An expander has no config,
         so hide the toolbar (otherwise an unloaded engine/gun source validates
         empty state as an error — "resolve errors: enginefx"). -->
    {#if $connectionInfo.controllerType === 'hubfx'}
        <ConfigToolbar />
    {/if}

    <div class="main-body" bind:this={mainBodyEl} class:resizing={dragging}>
        <div class="main-content">
            <div class="tab-pane">
                {#if current?.kind === 'io'}
                    <IoTab />
                {:else if current?.kind === 'engine'}
                    <div class="tab-content"><EnginePanel /></div>
                {:else if current?.kind === 'gun'}
                    <div class="tab-content"><GunFxPanel /></div>
                {:else if current?.kind === 'lighting'}
                    <LightingTab />
                {:else if current?.kind === 'gear'}
                    {#key current.key}
                        <GearLandingTab domain={current.domain} />
                    {/key}
                {:else if current?.kind === 'domain' && current.domain}
                    {#key current.key}
                        <DomainTab domain={current.domain} />
                    {/key}
                {:else if current?.kind === 'gear-diagnostics'}
                    <div class="tab-content"><GearDiagnosticsTab /></div>
                {:else}
                    <FirmwareTab />
                {/if}
            </div>
        </div>

        {#if $showConsole}
            <!-- svelte-ignore a11y-no-static-element-interactions -->
            <div class="resize-grip" on:mousedown={onGripDown}></div>
            <div class="console-pane" style="width: {consoleWidthPct}%">
                <ConsolePanel />
            </div>
        {/if}

        <!-- Right-edge ticker tab to toggle console -->
        <button
            class="console-ticker"
            class:open={$showConsole}
            style={$showConsole ? `right: ${consoleWidthPct}%` : ''}
            on:click={() => $showConsole = !$showConsole}
            title={$showConsole ? 'Hide Console (Ctrl+`)' : 'Show Console (Ctrl+`)'}
        >
            <span class="ticker-label">Console</span>
        </button>
    </div>

    <StatusBar />
</div>

<style>
    .main-layout {
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }

    .main-body {
        flex: 1;
        display: flex;
        flex-direction: row;
        min-height: 0;
        position: relative;
    }

    /* Prevent text selection while dragging the resize grip */
    .main-body.resizing {
        user-select: none;
        cursor: col-resize;
    }

    .main-content {
        flex: 1;
        display: flex;
        flex-direction: column;
        min-height: 0;
        min-width: 0;
        overflow: auto;
    }

    .tab-pane {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
    }

    .console-pane {
        min-width: 200px;
        flex-shrink: 0;
        display: flex;
        flex-direction: column;
    }

    .resize-grip {
        width: 5px;
        cursor: col-resize;
        background: transparent;
        flex-shrink: 0;
        position: relative;
        z-index: 5;
    }

    .resize-grip::after {
        content: '';
        position: absolute;
        top: 0;
        bottom: 0;
        left: 2px;
        width: 1px;
        background: var(--border);
    }

    .resize-grip:hover::after,
    .main-body.resizing .resize-grip::after {
        width: 3px;
        left: 1px;
        background: var(--accent);
    }

    .console-ticker {
        position: absolute;
        right: 0;
        top: 50%;
        transform: translateY(-50%);
        writing-mode: vertical-rl;
        background: var(--bg-raised);
        border: 1px solid var(--border);
        border-right: none;
        border-radius: 6px 0 0 6px;
        padding: 12px 5px;
        cursor: pointer;
        color: var(--text-dim);
        font-size: 11px;
        font-weight: 600;
        letter-spacing: 1px;
        text-transform: uppercase;
        z-index: 10;
        transition: background 0.15s, color 0.15s, right 0.15s;
    }

    .console-ticker:hover {
        background: var(--bg-input);
        color: var(--text-bright);
    }

    .ticker-label {
        pointer-events: none;
    }
</style>
