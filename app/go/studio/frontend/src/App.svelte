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
    import ServoCalibrationDialog from './lib/dialogs/ServoCalibrationDialog.svelte'
    import { closeServoCalibrationSilent } from './lib/servo_calibration'
    import MotorCalibrationDialog from './lib/dialogs/MotorCalibrationDialog.svelte'
    import ConfigWizard from './lib/dialogs/ConfigWizard.svelte'
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
        hydrateFromHubYaml, hubConfigSource,
    } from './lib/devicemodel'
    import {
        installEngineStateBridge, loadEngineConfig, engineConfigSource,
    } from './lib/effects'
    import { gunfxConfigSource, loadGunFxConfig } from './lib/gunfx'
    import { lightfxConfigSource, loadLightFxConfig } from './lib/lightfx'
    import { landingConfigSource, loadLandingConfig } from './lib/landing'
    import { gearConfigSource, loadGearConfig } from './lib/gear'
    import { registerDirtySource } from './lib/dirty-registry'

    onMount(async () => {
        // Hook window.onerror / unhandledrejection / console.error so JS
        // exceptions show up in the same diagnostic stream as Go events.
        installDiagBridge()
        diag.info('FE.APP', 'App.svelte mounted')

        // Device-model events + catalogs (domains/roles/presets are
        // available pre-connect so the tab strip renders immediately).
        installDeviceModelBridge()
        installInputValuesBridge()
        installEngineStateBridge()
        // Setup Wizard is available on demand from the toolbar (the ✨ button).
        // It is NOT auto-offered on connect — a fresh board opens straight to
        // the normal tabs + ports; an uninvited full-screen modal read as a
        // freeze (covered the ports, blocked tab-switching) — 2026-07-25.
        try { await loadCatalogs() } catch { /* app still starting */ }

        // Rule 46 — register every config source at app startup so the
        // global Apply order is deterministic (hubconfig → enginefx →
        // gunfx → future LightFx / GearControl).  Order matters
        // because effect translators on the firmware resolve named
        // inputs against /hubfx.yaml — that file MUST commit first.
        // Adding a new effect = `registerDirtySource(newConfigSource)`
        // here, after its dependencies; the panel stays a pure view.
        registerDirtySource(hubConfigSource)
        registerDirtySource(engineConfigSource)
        registerDirtySource(gunfxConfigSource)
        registerDirtySource(lightfxConfigSource)
        registerDirtySource(landingConfigSource)
        registerDirtySource(gearConfigSource)

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
                // Topology + every on-device YAML are HubFX-ONLY: the master
                // owns the port/role model and holds all effect config (incl.
                // the gearcontrol expander's config in /hubfx.yaml's expanders
                // block).  An expander board carries no config of its own, so
                // probing it for /hubfx.yaml /enginefx.yaml /gunfx.yaml … only
                // produces a flurry of INVALID_COMMAND NACKs.  Skip them.
                if (info.controllerType === 'hubfx') {
                    try { await refreshDeviceModel() } catch (e) {
                        diag.warn('FE.DM', 'device-model refresh failed', { err: String(e) })
                    }
                    try { await hydrateFromHubYaml() } catch (e) {
                        diag.warn('FE.CFG', 'hub config load failed', { err: String(e) })
                    }
                    try { await loadEngineConfig() } catch (e) {
                        diag.warn('FE.CFG', 'engine config load failed', { err: String(e) })
                    }
                    try { await loadGunFxConfig() } catch (e) {
                        diag.warn('FE.CFG', 'gunfx config load failed', { err: String(e) })
                    }
                    try { await loadLightFxConfig() } catch (e) {
                        diag.warn('FE.CFG', 'lightfx config load failed', { err: String(e) })
                    }
                    try { await loadLandingConfig() } catch (e) {
                        diag.warn('FE.CFG', 'landing config load failed', { err: String(e) })
                    }
                    try { await loadGearConfig() } catch (e) {
                        diag.warn('FE.CFG', 'gear config load failed', { err: String(e) })
                    }
                }
            } else if (wasConnected && $boardState !== 'flashing') {
                // Unexpected disconnect (not flashing) — show connect popup
                diag.warn('FE.CONN', 'unexpected disconnect — showing reconnect dialog')
                $boardState = 'disconnected'
                $connectPopupOpen = true
                $activeTab = 0
                // A calibration session cannot survive the link: its cancel
                // path needs the wire (origin restore).  Close it silently —
                // the widened envelope was live-only, nothing persists.
                closeServoCalibrationSilent()
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
                // HubFX-only (see the connection:changed handler above).
                if (info.controllerType === 'hubfx') {
                    try { await refreshDeviceModel() } catch { /* ignore */ }
                    try { await hydrateFromHubYaml() } catch { /* ignore */ }
                    try { await loadEngineConfig() } catch { /* ignore */ }
                    try { await loadGunFxConfig() } catch { /* ignore */ }
                    try { await loadLightFxConfig() } catch { /* ignore */ }
                    try { await loadLandingConfig() } catch { /* ignore */ }
                    try { await loadGearConfig() } catch { /* ignore */ }
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

{#if $showPcbOverlay}
    <PcbOverlayDialog />
{/if}

<!-- (ProgramEditorDialog was removed 2026-05-24 — the LightFxPanel now
     inlines the channel/event editor per active program, see Rule
     46 refactor + the new preset-library workflow.) -->

<!-- Servo calibration popup (Rule 44 + new popup pattern).
     Visibility driven by the `openServoCalibration` store; any
     ServoWidget on the IO / Lighting / GunFx tabs pops it open via
     `openServoCalibrationFor(...)`.  Replaces the inline
     ServoProfileEditor so feature rows stay compact. -->
<ServoCalibrationDialog />

<!-- Gear-motor (H-bridge) calibration popup — opened from the GearPanel
     strut motor card via `openMotorCalibrationFor(...)`. -->
<MotorCalibrationDialog />

<!-- Setup Wizard — guided config (opened from the toolbar or auto-offered
     on connect to a fresh board).  Visibility via `showConfigWizard`. -->
<ConfigWizard />

<style>
    .app-layout {
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }

</style>
