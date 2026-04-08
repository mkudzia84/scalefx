<!-- ScaleFX Studio — Root Layout -->
<!-- Main layout always visible. Connect dialog is a popup overlay. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import ConnectDialog from './lib/dialogs/ConnectDialog.svelte'
    import AboutDialog from './lib/dialogs/AboutDialog.svelte'
    import FlashProgressDialog from './lib/dialogs/FlashProgressDialog.svelte'
    import MainLayout from './lib/layout/MainLayout.svelte'
    import {
        boardState, connectPopupOpen, showAboutDialog, showConsole,
        connectionInfo, activeTab, showFlashProgress
    } from './lib/stores'
    import { theme } from './lib/theme'
    import { EventsOn } from '../wailsjs/runtime/runtime'
    import { GetConnectionInfo } from '../wailsjs/go/main/App'

    onMount(async () => {
        // Menu events from native Wails menus
        EventsOn('menu:connect', () => {
            $connectPopupOpen = true
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

            if (info.connected) {
                $boardState = 'connected'
                $connectPopupOpen = false
            } else if (wasConnected && $boardState !== 'flashing') {
                // Unexpected disconnect (not flashing) — show connect popup
                $boardState = 'disconnected'
                $connectPopupOpen = true
                $activeTab = 0
            }
        })

        // Load initial connection state
        try {
            const info = await GetConnectionInfo()
            $connectionInfo = info
            if (info.connected) {
                $boardState = 'connected'
                $connectPopupOpen = false
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

<div class="app-layout">
    <MainLayout />
</div>

<!-- Overlays (render above everything) -->
{#if $connectPopupOpen}
    <ConnectDialog />
{/if}

{#if $showAboutDialog}
    <AboutDialog />
{/if}

{#if $showFlashProgress}
    <FlashProgressDialog />
{/if}

<style>
    .app-layout {
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }
</style>
