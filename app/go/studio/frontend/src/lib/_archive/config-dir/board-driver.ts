// ScaleFX Studio — Board Configuration Driver
//
// A BoardConfigDriver bridges a tab's reactive state to the SaveConfigDialog.
// Each board tab (LightFX / GearControl / HubFX) implements one and hands it
// to the dialog as a single prop. The dialog uses it to:
//
//   • buildState()       — snapshot the tab's live state
//   • generateYaml(s)    — render snapshot → YAML text (for the YAML tab)
//   • parseYaml(text)    — YAML → snapshot + validation errors
//   • applyState(s)      — push snapshot back into the tab's reactive state
//                          (wires the Import flow — dialog asks the driver to
//                          update the tab, which re-fires the verifier)
//   • verify(s)          — run the board's ConfigVerifier on a snapshot
//
// The driver keeps the dialog board-agnostic: it never knows about LightFX
// programs or GearControl pins — only about the interface.

import type { VerifyResult } from './config-verifier'

export interface ParseResult<T> {
    /** Parsed state — undefined if parse failed and no recovery was possible. */
    state?: T
    /** Hard errors — show in red; import button disabled if non-empty. */
    errors: string[]
    /** Soft issues — shown but don't block import. */
    warnings: string[]
}

export interface BoardConfigDriver<TState> {
    /** Machine key for Wails/engine (e.g. 'lightfx', 'gearcontrol', 'hubfx'). */
    readonly boardType: string
    /** Display label shown in the dialog header. */
    readonly boardLabel: string
    /** Snapshot current tab state into a plain object. */
    buildState(): TState
    /** Render a state snapshot to YAML text. */
    generateYaml(state: TState): string
    /** Parse YAML text into a state snapshot + diagnostics. */
    parseYaml(text: string): ParseResult<TState>
    /** Push a state snapshot back into the tab's reactive variables. */
    applyState(state: TState): void
    /** Run the board-specific verifier on a snapshot. */
    verify(state: TState): VerifyResult
}
