<!-- ScaleFX Studio — Root Layout -->
<!-- Main layout always visible. Connect dialog is a popup overlay. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import ConnectDialog from './lib/dialogs/ConnectDialog.svelte'
    import AboutDialog from './lib/dialogs/AboutDialog.svelte'
    import ViewSettingsDialog from './lib/dialogs/ViewSettingsDialog.svelte'
    import FlashProgressDialog from './lib/dialogs/FlashProgressDialog.svelte'
    import MainLayout from './lib/layout/MainLayout.svelte'
    import {
        boardState, connectPopupOpen, showAboutDialog, showConsole,
        showViewSettings,
        connectionInfo, activeTab, showFlashProgress,
        pushConsoleMessage
    } from './lib/stores'
    import type { ConsoleMessage } from './lib/stores'
    import { theme, fontSize } from './lib/theme'
    import { EventsOn } from '../wailsjs/runtime/runtime'
    import { GetConnectionInfo } from '../wailsjs/go/main/App'

    onMount(async () => {
        // Console output events from backend (always active, even when panel hidden)
        EventsOn('console:output', (msg: { type: string; content: string }) => {
            pushConsoleMessage(msg.type as ConsoleMessage['type'], msg.content)
        })

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
        EventsOn('menu:viewsettings', () => {
            $showViewSettings = true
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
    $: document.documentElement.setAttribute('data-theme', $theme)

    // Apply font size to document root
    $: document.documentElement.style.fontSize = `${$fontSize}px`
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

{#if $showViewSettings}
    <ViewSettingsDialog />
{/if}

<style>
    .app-layout {
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }
</style>
