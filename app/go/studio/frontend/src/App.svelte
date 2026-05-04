<!-- ScaleFX Studio — Root Layout -->
<!-- Main layout always visible. Connect dialog is a popup overlay. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import ConnectDialog from './lib/dialogs/ConnectDialog.svelte'
    import AboutDialog from './lib/dialogs/AboutDialog.svelte'
    import ViewSettingsDialog from './lib/dialogs/ViewSettingsDialog.svelte'
    import FlashProgressDialog from './lib/dialogs/FlashProgressDialog.svelte'
    import FileManagerDialog from './lib/dialogs/FileManagerDialog.svelte'
    import MainLayout from './lib/layout/MainLayout.svelte'
    import {
        boardState, connectPopupOpen, showAboutDialog, showConsole,
        showViewSettings, showFileManager,
        connectionInfo, activeTab, showFlashProgress,
        pushConsoleMessage, slaveInfo,
    } from './lib/stores'
    import type { ConsoleMessage, SlaveInfo } from './lib/stores'
    import { theme, fontSize } from './lib/theme'
    import { EventsOn } from '../wailsjs/runtime/runtime'
    import { GetConnectionInfo, GetSlaveInfo } from '../wailsjs/go/main/App'
    import { installDiagBridge, diag } from './lib/diag'

    onMount(async () => {
        // Hook window.onerror / unhandledrejection / console.error so JS
        // exceptions show up in the same diagnostic stream as Go events.
        installDiagBridge()
        diag.info('FE.APP', 'App.svelte mounted')

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
                // Seed the slave list on connect so HubFX tabs appear
                // immediately (greyed out) before the first STATUS_BROADCAST.
                if (info.controllerType === 'hubfx') {
                    try {
                        const list = await GetSlaveInfo()
                        $slaveInfo = (list || []).map((s: any) => ({
                            ...s, enabled: s.enabled ?? true,
                        }))
                    } catch { /* ignore */ }
                } else {
                    $slaveInfo = []
                }
            } else if (wasConnected && $boardState !== 'flashing') {
                // Unexpected disconnect (not flashing) — show connect popup
                diag.warn('FE.CONN', 'unexpected disconnect — showing reconnect dialog')
                $boardState = 'disconnected'
                $connectPopupOpen = true
                $activeTab = 0
                $slaveInfo = []
            }
        })

        // Slave online/offline state — driven by HubFX STATUS_BROADCAST
        // (every ~1s from the hub). The tab bar reads $slaveInfo reactively,
        // so tabs fade/unfade as slaves connect or disconnect. The backend
        // doesn't track the per-slave `enabled` UI toggle, so merge it from
        // the previous store snapshot instead of resetting to true.
        EventsOn('slaves:changed', (list: SlaveInfo[]) => {
            slaveInfo.update(prev => {
                const enabledBy: Record<string, boolean> = {}
                for (const s of prev) enabledBy[s.type] = s.enabled ?? true
                return (list || []).map((s: any) => ({
                    ...s,
                    enabled: enabledBy[s.type] ?? s.enabled ?? true,
                }))
            })
        })

        // Load initial connection state
        try {
            const info = await GetConnectionInfo()
            $connectionInfo = info
            if (info.connected) {
                $boardState = 'connected'
                $connectPopupOpen = false
                if (info.controllerType === 'hubfx') {
                    try {
                        const list = await GetSlaveInfo()
                        $slaveInfo = (list || []).map((s: any) => ({
                            ...s, enabled: s.enabled ?? true,
                        }))
                    } catch { /* ignore */ }
                }
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

<style>
    .app-layout {
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }
</style>
