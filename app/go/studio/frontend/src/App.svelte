<!-- ScaleFX Studio — Root Layout -->
<!-- Main layout always visible. Connect dialog is a popup overlay. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import ConnectDialog from './lib/dialogs/ConnectDialog.svelte'
    import AboutDialog from './lib/dialogs/AboutDialog.svelte'
    import ViewSettingsDialog from './lib/dialogs/ViewSettingsDialog.svelte'
    import FlashProgressDialog from './lib/dialogs/FlashProgressDialog.svelte'
    import FileManagerDialog from './lib/dialogs/FileManagerDialog.svelte'
    import PcbOverlayDialog from './lib/dialogs/PcbOverlayDialog.svelte'
    import MainLayout from './lib/layout/MainLayout.svelte'
    import {
        boardState, connectPopupOpen, showAboutDialog, showConsole,
        showViewSettings, showFileManager, showPcbOverlay,
        connectionInfo, activeTab, showFlashProgress,
        pushConsoleMessage,
    } from './lib/stores'
    import type { ConsoleMessage } from './lib/stores'
    import { theme, fontSize } from './lib/theme'
    import { EventsOn } from '../wailsjs/runtime/runtime'
    import { GetConnectionInfo } from '../wailsjs/go/main/App'
    import { installDiagBridge, diag } from './lib/diag'
    import {
        installDeviceModelBridge, installInputValuesBridge, loadCatalogs,
        refresh as refreshDeviceModel, reset as resetDeviceModel,
    } from './lib/devicemodel'

    onMount(async () => {
        // Hook window.onerror / unhandledrejection / console.error so JS
        // exceptions show up in the same diagnostic stream as Go events.
        installDiagBridge()
        diag.info('FE.APP', 'App.svelte mounted')

        // Device-model events + catalogs (domains/roles/presets are
        // available pre-connect so the tab strip renders immediately).
        installDeviceModelBridge()
        installInputValuesBridge()
        try { await loadCatalogs() } catch { /* app still starting */ }

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
        EventsOn('menu:filemanager', () => {
            $showFileManager = true
        })

        // Connection state changes from backend
        EventsOn('connection:changed', async (info: any) => {
            const wasConnected = $connectionInfo.connected
            $connectionInfo = info
            diag.info('FE.CONN', 'connection:changed', {
                connected: info.connected,
                controller: info.controllerType,
                port: info.port,
            })

            if (info.connected) {
                $boardState = 'connected'
                $connectPopupOpen = false
                // Pull the topology (ports + roles) so the setup +
                // functional tabs populate.  Capability-gated domain tabs
                // appear once the model reports the hub's capabilities.
                try { await refreshDeviceModel() } catch (e) {
                    diag.warn('FE.DM', 'device-model refresh failed', { err: String(e) })
                }
            } else if (wasConnected && $boardState !== 'flashing') {
                // Unexpected disconnect (not flashing) — show connect popup
                diag.warn('FE.CONN', 'unexpected disconnect — showing reconnect dialog')
                $boardState = 'disconnected'
                $connectPopupOpen = true
                $activeTab = 0
                resetDeviceModel()
            }
        })

        // Load initial connection state
        try {
            const info = await GetConnectionInfo()
            $connectionInfo = info
            if (info.connected) {
                $boardState = 'connected'
                $connectPopupOpen = false
                try { await refreshDeviceModel() } catch { /* ignore */ }
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

{#if $showFileManager}
    <FileManagerDialog />
{/if}

{#if $showPcbOverlay}
    <PcbOverlayDialog />
{/if}

<style>
    .app-layout {
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }
</style>
