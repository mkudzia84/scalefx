<!-- ServoWidget — compact servo-port summary for feature panels
     (LandingPanel servo rows, GunFxPanel yaw/pitch axes, future
     gear-control servos).  Replaces the inline ServoProfileEditor
     (Rule 44): the feature row stays small; the deep edit surface
     lives in the popup ServoCalibrationDialog.

     Layout:
       [ <profile summary text> ]  [ ⚙ Calibrate… ]

     The Calibrate button opens the popup (limits, speed, accel,
     jerk, live jog).  Direction is NOT here (2.46.0): open/close
     positions are absolute µs set per effect with the endpoints
     widget.

     Cross-board servos: today the dialog only jogs hub-local ports
     (the Wails ServoSetTarget NACKs guid != '').  The widget still
     opens the dialog for read-only inspection — jog buttons disable
     themselves there.  Live-tune across the bus is a follow-up.
-->
<script lang="ts">
    import type { PortRefT } from '../landing'
    import {
        openServoCalibrationFor, summariseProfile,
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
</script>

<div class="servo-widget">
    {#if !hasPort}
        <span class="placeholder">pick a servo port first</span>
    {:else}
        <span class="summary" title={summary}>{summary}</span>
    {/if}
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


    .err { color: var(--error); font-weight: 700; cursor: help; }
</style>
