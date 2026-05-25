<!-- ServoWidget — compact servo-port summary for feature panels
     (LandingPanel servo rows, GunFxPanel yaw/pitch axes, future
     gear-control servos).  Replaces the inline ServoProfileEditor
     (Rule 44): the feature row stays small; the deep edit surface
     lives in the popup ServoCalibrationDialog.

     Layout:
       [ <profile summary text> ]  [ ↔ Reversed ]  [ ⚙ Calibrate… ]

     The Reversed toggle is a one-click writer — it flips
     `profile.reversed`, pushes via SetPortProfile (live to role +
     /hubfx.yaml dirty), and refreshes the summary.  No dialog
     needed; the operator gets immediate feedback.  The Calibrate
     button opens the full popup for everything else (limits, speed,
     accel, jerk, live jog).

     Cross-board servos: today the dialog only jogs hub-local ports
     (the Wails ServoSetTarget NACKs guid != '').  The widget still
     opens the dialog for read-only inspection — jog buttons disable
     themselves there.  Live-tune across the bus is a follow-up.
-->
<script lang="ts">
    import type { PortRefT } from '../landing'
    import {
        openServoCalibrationFor, toggleReversed, summariseProfile,
        defaultServoProfile, type ServoProfileT,
    } from '../servo_calibration'

    /** The port this widget represents.  Empty / unset → widget
     *  shows a placeholder ("pick a port first") and disables the
     *  Calibrate + Reversed buttons. */
    export let port: PortRefT | null = null
    /** The current profile from the device model (typically read
     *  from `$deviceModel.ports[i].profile`).  When null, the widget
     *  shows "no profile (role defaults)" and the Calibrate dialog
     *  opens with `defaultServoProfile()`. */
    export let profile: ServoProfileT | null = null
    /** Optional human label for the popup header — usually
     *  "Hub · IN_3 (5 V)" computed via the shared port-label helper. */
    export let portLabel: string = ''
    /** Disable both buttons (mirrors the parent's `busy` flag). */
    export let busy: boolean = false

    /** Parent callback fired after a successful reversed toggle so
     *  the panel can refresh its draft / device-model view.  Optional
     *  — when omitted, the widget assumes the parent already subscribes
     *  to `$deviceModel.ports` for live updates. */
    export let onProfileChanged: ((next: ServoProfileT) => void) | null = null

    let localBusy = false
    let error = ''
    $: portRef = port
    $: prof    = profile ?? defaultServoProfile()
    $: hasPort = !!(portRef && portRef.kind && portRef.idx !== undefined && portRef.idx >= 0
                    && portRef.kind === 'servo')
    $: summary = profile ? summariseProfile(profile) : 'no profile (role defaults)'

    async function onCalibrate() {
        if (!hasPort || !portRef) return
        await openServoCalibrationFor(
            portRef.guid ?? '', portRef.idx, portLabel || `Servo idx ${portRef.idx}`,
            prof,
            /*startingTargetUs=*/ prof.centerUs,
        )
    }
    async function onToggleReversed() {
        if (!hasPort || !portRef) return
        localBusy = true; error = ''
        try {
            const next = await toggleReversed(portRef.guid ?? '', portRef.idx, prof)
            if (onProfileChanged) onProfileChanged(next)
        } catch (e) {
            error = String(e)
        } finally {
            localBusy = false
        }
    }
</script>

<div class="servo-widget">
    {#if !hasPort}
        <span class="placeholder">pick a servo port first</span>
    {:else}
        <span class="summary" title={summary}>{summary}</span>
    {/if}
    <button class="small rev-btn" class:on={prof.reversed}
            on:click={onToggleReversed}
            disabled={busy || localBusy || !hasPort}
            title={prof.reversed
                ? 'Direction is REVERSED — "open" maps to min µs.  Click to un-reverse.'
                : 'Direction is NORMAL — "open" maps to max µs.  Click to reverse so open maps to min instead.'}>
        ↔ {prof.reversed ? 'Reversed' : 'Normal'}
    </button>
    <button class="small" on:click={onCalibrate}
            disabled={busy || localBusy || !hasPort}
            title="Open the calibration popup — live jog, set limits, edit speed / accel / jerk.">
        ⚙ Calibrate…
    </button>
    {#if error}<span class="err" title={error}>⚠</span>{/if}
</div>

<style>
    .servo-widget {
        display: inline-flex; align-items: center; gap: 8px;
        font-size: 11px;
    }
    .summary {
        font-family: var(--font-mono); color: var(--text-dim);
        max-width: 280px;
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
    }
    .placeholder { color: var(--text-dim); font-style: italic; }

    /* The reversed-toggle reads as a STATEFUL chip — text changes,
       colour cue when on.  Mirrors the speaker-routing button pattern. */
    .rev-btn { min-width: 80px; }
    .rev-btn.on { color: var(--warning); font-weight: 600; }

    .err { color: var(--error); font-weight: 700; cursor: help; }
</style>
