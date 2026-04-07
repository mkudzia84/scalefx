<!-- ScaleFX Studio — Root Layout -->
<!-- Routes between startup (connect dialog) and main (tabbed layout). -->
<script lang="ts">
    import { onMount } from 'svelte'
    import ConnectDialog from './lib/dialogs/ConnectDialog.svelte'
    import AboutDialog from './lib/dialogs/AboutDialog.svelte'
    import MainLayout from './lib/layout/MainLayout.svelte'
    import {
        appPhase, showConnectDialog, showAboutDialog, showConsole,
        connectionInfo, activeTab
    } from './lib/stores'
    import { theme } from './lib/theme'
    import { EventsOn } from '../wailsjs/runtime/runtime'
    import { GetConnectionInfo } from '../wailsjs/go/main/App'

    onMount(async () => {
        // Menu events from native Wails menus
        EventsOn('menu:connect', () => {
            $showConnectDialog = true
        })
        EventsOn('menu:about', () => {
            $showAboutDialog = true
        })
        EventsOn('menu:console', () => {
            $showConsole = !$showConsole
        })

        // Connection state changes from backend
        EventsOn('connection:changed', (info: any) => {
            const wasConnected = $connectionInfo.connected
            $connectionInfo = info

            if (!info.connected && wasConnected) {
                // Board disconnected — return to startup
                $appPhase = 'startup'
                $showConnectDialog = true
                $activeTab = 0
            }
        })

        // Load initial connection state
        try {
            const info = await GetConnectionInfo()
            $connectionInfo = info
            if (info.connected) {
                $appPhase = 'main'
                $showConnectDialog = false
            }
        } catch (_) {
            // app still starting
        }
    })

    // Apply theme class to document root
    $: {
        document.documentElement.setAttribute('data-theme', $theme)
    }
</script>

{#if $appPhase === 'startup'}
    <ConnectDialog />
{:else}
    <div class="app-layout">
        <MainLayout />
    </div>
{/if}

<!-- Overlays (render above everything) -->
{#if $showAboutDialog}
    <AboutDialog />
{/if}

{#if $showConnectDialog && $appPhase === 'main'}
    <!-- Reconnect dialog over the main layout -->
    <ConnectDialog />
{/if}

<style>
    .app-layout {
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }
</style>
