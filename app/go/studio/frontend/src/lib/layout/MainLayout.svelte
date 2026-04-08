<!-- ScaleFX Studio — Main Layout -->
<!-- The main application view shown after connecting. Tab bar + content area + console panel + status bar. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import TabBar from './TabBar.svelte'
    import StatusBar from './StatusBar.svelte'
    import ConsolePanel from '../dialogs/ConsoleDialog.svelte'
    import BoardTab from '../tabs/BoardTab.svelte'
    import FirmwareTab from '../tabs/FirmwareTab.svelte'
    import HubFxTab from '../tabs/HubFxTab.svelte'
    import GunFxTab from '../tabs/GunFxTab.svelte'
    import GearControlTab from '../tabs/GearControlTab.svelte'
    import LightFxTab from '../tabs/LightFxTab.svelte'
    import { connectionInfo, activeTab, slaveInfo, showConsole } from '../stores'

    // Derive tab count to clamp activeTab
    $: isHubFX = $connectionInfo.controllerType === 'hubfx'
    $: slaveCount = isHubFX ? $slaveInfo.length : 0
    $: tabCount = (isHubFX ? 1 + slaveCount : 1) + 1 // +1 for firmware
    $: if ($activeTab >= tabCount) $activeTab = 0

    // Determine which tab type to show
    $: isFirmwareTab = $activeTab === tabCount - 1

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

    <div class="main-body" bind:this={mainBodyEl} class:resizing={dragging}>
        <div class="main-content">
            <!-- Keep-alive: all tabs always mounted, only active one visible -->
            <div class="tab-pane" class:hidden={!isFirmwareTab}>
                <FirmwareTab />
            </div>

            {#if isHubFX}
                <div class="tab-pane" class:hidden={isFirmwareTab || $activeTab !== 0}>
                    <HubFxTab boardLabel={$connectionInfo.controllerName || 'HubFX'} />
                </div>

                {#each $slaveInfo as slave, i (i)}
                    <div class="tab-pane" class:hidden={isFirmwareTab || $activeTab !== i + 1}>
                        {#if slave.type === 'gunfx'}
                            <GunFxTab boardLabel={slave.name} />
                        {:else if slave.type === 'gearcontrol'}
                            <GearControlTab boardLabel={slave.name} />
                        {:else if slave.type === 'lightfx'}
                            <LightFxTab boardLabel={slave.name} />
                        {:else}
                            <BoardTab boardType={slave.type} boardLabel={slave.name} />
                        {/if}
                    </div>
                {/each}
            {:else}
                <div class="tab-pane" class:hidden={isFirmwareTab}>
                    {#if $connectionInfo.controllerType === 'gunfx'}
                        <GunFxTab boardLabel={$connectionInfo.controllerName || 'GunFX'} />
                    {:else if $connectionInfo.controllerType === 'gearcontrol'}
                        <GearControlTab boardLabel={$connectionInfo.controllerName || 'GearControl'} />
                    {:else if $connectionInfo.controllerType === 'lightfx'}
                        <LightFxTab boardLabel={$connectionInfo.controllerName || 'LightFX'} />
                    {:else}
                        <BoardTab boardType={$connectionInfo.controllerType} boardLabel={$connectionInfo.controllerName || 'Board'} />
                    {/if}
                </div>
            {/if}
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

    .tab-pane.hidden {
        display: none;
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
