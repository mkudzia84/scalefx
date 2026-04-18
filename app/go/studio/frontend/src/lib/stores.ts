// ScaleFX Studio — Svelte Stores
// Reactive state shared across components.

import { writable } from 'svelte/store'

export interface ConnectionInfo {
    connected: boolean
    initialized: boolean
    port: string
    controllerType: string
    controllerName: string
    firmwareVersion: string
    build: number
    platform: string
    cpuMHz: number
    freeRAM: number
}

export interface ConsoleMessage {
    type: 'ok' | 'error' | 'info' | 'warning' | 'output' | 'command' | 'debug'
    content: string
    timestamp: number
}

export interface PortInfo {
    name: string
    description: string
}

export interface SlaveInfo {
    type: string
    name: string
    connected: boolean
    ready: boolean
    enabled: boolean
}

// ─── Connection state ───

export const connectionInfo = writable<ConnectionInfo>({
    connected: false,
    initialized: false,
    port: '',
    controllerType: '',
    controllerName: '',
    firmwareVersion: '',
    build: 0,
    platform: '',
    cpuMHz: 0,
    freeRAM: 0,
})

// ─── Console messages ───

export const consoleMessages = writable<ConsoleMessage[]>([])

export function pushConsoleMessage(type: ConsoleMessage['type'], content: string) {
    consoleMessages.update(msgs => {
        const msg: ConsoleMessage = { type, content, timestamp: Date.now() }
        const next = [...msgs, msg]
        return next.length > 5000 ? next.slice(-4000) : next
    })
}

// ─── Board state machine ───

/** Board connection state: disconnected → connected → flashing → disconnected */
export type BoardState = 'disconnected' | 'connected' | 'flashing'
export const boardState = writable<BoardState>('disconnected')

// ─── UI state ───

/** Whether the connect popup overlay is visible */
export const connectPopupOpen = writable(true) // shown on startup
export const showAboutDialog = writable(false)
export const showViewSettings = writable(false)
export const showConsole = writable(false)
export const showFileManager = writable(false)

/** Currently active tab index */
export const activeTab = writable(0)

/** Available COM ports (updated by port watcher) */
export const availablePorts = writable<PortInfo[]>([])

/** Slave controller info (HubFX only) */
export const slaveInfo = writable<SlaveInfo[]>([])

// ─── Firmware flashing state ───

export interface FirmwareTarget {
    name: string
    platform: string
    subDir: string
}

export interface FirmwareProgress {
    step: number
    total: number
    message: string
    type: 'info' | 'ok' | 'warning' | 'error' | 'step'
    done: boolean
    error?: string
    reconnecting?: boolean
}

export interface ReleaseInfo {
    controller: string
    version: string
    tag: string
    name: string
    body: string          // release notes (markdown)
    prerelease: boolean
    published: string
    assetName: string
    assetSize: number
}

export const firmwareTargets = writable<FirmwareTarget[]>([])
export const firmwareRunning = writable(false)
export const firmwareLogs = writable<FirmwareProgress[]>([])
export const availableReleases = writable<ReleaseInfo[]>([])

/** Whether the flash progress dialog is visible */
export const showFlashProgress = writable(false)

/** Summary shown after flash completes */
export const flashResult = writable<{ success: boolean; message: string } | null>(null)

/** Whether the save config dialog is visible */
export const showSaveConfig = writable(false)
