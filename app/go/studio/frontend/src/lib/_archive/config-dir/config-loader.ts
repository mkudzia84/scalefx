// ScaleFX Studio — Generic Device Config Loader (Rule 26)
//
// On connect, board tabs must hydrate their reactive state from /config.yaml
// on the device instead of staying on hardcoded defaults. This helper is the
// single path that downloads + parses + applies — every tab uses it through
// its existing BoardConfigDriver<T>, so the hook stays board-agnostic.
//
// Wire-up (per tab):
//
//     import { autoLoadOnConnect } from '../config/config-loader'
//     onMount(() => autoLoadOnConnect(driver, 'gearcontrol'))
//
// `autoLoadOnConnect` subscribes to connectionInfo and fires exactly once per
// connection session, when (connected && initialized && controllerType match).
// It is idempotent across reconnects — the subscription itself is persistent
// and guarded by `loadedForPort` so the same YAML is not re-applied twice.

import { get } from 'svelte/store'
import type { BoardConfigDriver } from './board-driver'
import { DownloadConfig } from '../../../wailsjs/go/main/App'
import { connectionInfo, pushConsoleMessage } from '../stores'

/** Download /config.yaml, parse via the driver, apply to tab state.
 *  Parse warnings/errors are echoed to the Studio console. Returns `true`
 *  on successful apply, `false` if the device had no config or parsing
 *  failed hard. Safe to call while disconnected — returns false silently. */
export async function loadConfigFromDevice<T>(
    driver: BoardConfigDriver<T>,
): Promise<boolean> {
    const info = get(connectionInfo)
    if (!info.connected || !info.initialized) return false

    let yaml: string
    try {
        yaml = await DownloadConfig()
    } catch (e: any) {
        pushConsoleMessage('error', `[${driver.boardType}] config load failed: ${e?.message || e}`)
        return false
    }
    if (!yaml) return false

    // Raw YAML head — the single most useful diagnostic when "applied but UI
    // unchanged": it reveals whether the device file has a different schema
    // (nested under `gear_control:`, old key names, etc) from what the parser
    // expects.
    const headKeys = extractTopLevelKeys(yaml)
    pushConsoleMessage('debug', `[${driver.boardType}] yaml top-level keys: ${headKeys.join(', ') || '(none)'}`)
    const preview = yaml.length > 240 ? yaml.slice(0, 240) + '…' : yaml
    pushConsoleMessage('debug', `[${driver.boardType}] yaml head: ${preview.replace(/\n/g, ' | ')}`)

    const result = driver.parseYaml(yaml)
    for (const w of result.warnings) {
        pushConsoleMessage('warning', `[${driver.boardType}] ${w}`)
    }
    for (const err of result.errors) {
        pushConsoleMessage('error', `[${driver.boardType}] ${err}`)
    }
    if (!result.state) {
        pushConsoleMessage('warning', `[${driver.boardType}] parser returned no state — keeping defaults`)
        return false
    }

    // Debug trace — state keys + element counts help diagnose "silent apply"
    // bugs where parseYaml succeeds but applyState has nothing to write.
    pushConsoleMessage('debug', `[${driver.boardType}] parsed state: ${summarizeState(result.state)}`)

    try {
        driver.applyState(result.state)
        pushConsoleMessage('ok', `[${driver.boardType}] applied config to UI`)
        return true
    } catch (e: any) {
        pushConsoleMessage('error', `[${driver.boardType}] apply failed: ${e?.message || e}`)
        return false
    }
}

/** Walks the YAML text and returns the names of all column-0 (unindented)
 *  mapping keys. Used purely for diagnostics so we can see what the device
 *  file actually contains. Skips comments and blank lines. */
function extractTopLevelKeys(yaml: string): string[] {
    const keys: string[] = []
    for (const line of yaml.split(/\r?\n/)) {
        if (!line || line.startsWith(' ') || line.startsWith('\t')) continue
        const stripped = line.replace(/#.*$/, '').trimEnd()
        if (!stripped) continue
        const colon = stripped.indexOf(':')
        if (colon <= 0) continue
        const key = stripped.slice(0, colon).trim()
        if (key && !keys.includes(key)) keys.push(key)
    }
    return keys
}

/** One-line state summary: scalar values printed inline, arrays shown with
 *  their length, objects recursed one level deep. Keeps diagnostic output
 *  short enough for the console without burying the user in a JSON dump. */
function summarizeState(state: unknown): string {
    if (state == null || typeof state !== 'object') return String(state)
    const parts: string[] = []
    for (const [k, v] of Object.entries(state as Record<string, unknown>)) {
        if (Array.isArray(v)) parts.push(`${k}[${v.length}]`)
        else if (typeof v === 'object' && v !== null) {
            const keys = Object.keys(v).length
            parts.push(`${k}{${keys}}`)
        } else parts.push(`${k}=${v}`)
    }
    return parts.join(', ')
}

/** Subscribe to connection state and auto-hydrate the tab on every fresh
 *  connection. Returns an unsubscribe function — caller wires it through
 *  onDestroy. Typically invoked from onMount():
 *
 *      onMount(() => autoLoadOnConnect(driver, 'gearcontrol'))
 *
 *  `matchControllerTypes` filters which controller types trigger the load
 *  (e.g. GearControlTab passes ['gearcontrol']; a HubFX-aware board tab may
 *  pass ['gearcontrol', 'hubfx']). Pass `'*'` to accept any type.
 *
 *  The loader fires once per (port × controllerType) tuple so a reconnect
 *  re-hydrates but a spurious connectionInfo update does not re-apply. */
export function autoLoadOnConnect<T>(
    driver: BoardConfigDriver<T>,
    matchControllerTypes: readonly string[] | '*',
): () => void {
    let lastKey = ''
    let pending = false

    const unsub = connectionInfo.subscribe(info => {
        if (!info.connected || !info.initialized) {
            lastKey = ''
            return
        }
        if (matchControllerTypes !== '*' &&
            !matchControllerTypes.includes(info.controllerType)) return

        const key = `${info.port}|${info.controllerType}`
        if (key === lastKey || pending) return
        lastKey = key
        pending = true
        loadConfigFromDevice(driver).finally(() => { pending = false })
    })
    return unsub
}
