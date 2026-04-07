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
    type: 'ok' | 'error' | 'info' | 'warning' | 'output' | 'command'
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

// ─── UI state ───

/** App phase: 'startup' shows connect dialog, 'main' shows the main layout */
export const appPhase = writable<'startup' | 'main'>('startup')

export const showConnectDialog = writable(true) // shown on startup
export const showAboutDialog = writable(false)
export const showConsole = writable(false)

/** Currently active tab index */
export const activeTab = writable(0)

/** Available COM ports (updated by port watcher) */
export const availablePorts = writable<PortInfo[]>([])

/** Slave controller info (HubFX only) */
export const slaveInfo = writable<SlaveInfo[]>([])

