<!-- MotorWidget — compact gear-motor (BiDcMotor / H-bridge) summary + the
     "⚙ Calibrate motor…" button + a LIVE state readout, for the GearPanel
     strut motor card.  The H-bridge analogue of ServoWidget.

     Layout:
       [ drive V · direction · timeout summary ]      [ ⚙ Calibrate motor… ]
       [ live:  <I> mA · duty <d> · <pos> · <stall> ]   (when a live sample exists)

     The mA is the headline — it's the operator's stall-current readout
     "hooked into status" (Rule 60.2: a per-element live strip stays with its
     element).  The Calibrate button opens MotorCalibrationDialog (live drive,
     sweep, stall-guard tuning); Save there writes duty + travel timeout back. -->
<script lang="ts">
    import type { MotorLive } from '../motor_status'

    /** Whether the strut has a resolved BiDcMotor port (else the buttons
     *  disable + the widget shows a placeholder). */
    export let hasMotor: boolean = false
    export let motorVoltageMv: number = 6000
    export let reverse: boolean = false
    export let timeoutMs: number = 0
    /** Latest live sample for this motor (from the motorStatus store) or null. */
    export let live: MotorLive | null = null
    export let busy: boolean = false
    /** Open the calibration dialog — the parent wires this to
     *  openMotorCalibrationFor(...) with the strut's onCommit. */
    export let onCalibrate: () => void = () => {}

    $: summary = `drive ${(motorVoltageMv / 1000).toFixed(1)} V · ${reverse ? 'reversed' : 'forward'} · timeout ${timeoutMs} ms`
    // Stale after ~2 s with no fresh sample (poll cadence is ~0.7–2 Hz).
    $: stale = !live || (Date.now() - live.ts) > 2500
</script>

<div class="motor-widget">
    <div class="row">
        {#if !hasMotor}
            <span class="placeholder">pick a gear motor port first</span>
        {:else}
            <span class="summary" title={summary}>{summary}</span>
        {/if}
        <button class="small" on:click={onCalibrate} disabled={busy || !hasMotor}
                title="Open the motor calibration popup — live drive, A→B sweep (measures travel + stall current), stall-guard tuning.">
            ⚙ Calibrate motor…
        </button>
    </div>

    {#if hasMotor}
        <div class="live" class:stale>
            <span class="lk">live</span>
            {#if live}
                <span class="lv current" class:warn={live.stalled}>{live.currentMa} mA</span>
                <span class="sep">·</span>
                <span class="lv">duty {live.signedDuty}</span>
                <span class="sep">·</span>
                <span class="lv">{live.positionName}</span>
                <span class="sep">·</span>
                {#if live.stalled}<span class="lv warn">STALL</span>{:else}<span class="lv dim">clear</span>{/if}
                {#if stale}<span class="lv dim">(stale)</span>{/if}
            {:else}
                <span class="lv dim">— waiting for status —</span>
            {/if}
        </div>
    {/if}
</div>

<style>
    .motor-widget { display: flex; flex-direction: column; gap: 4px; font-size: 11px; }
    .row { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
    .summary { font-family: var(--font-mono); color: var(--text-dim); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .placeholder { color: var(--text-dim); font-style: italic; }

    .live { display: flex; align-items: baseline; gap: 6px; font-family: var(--font-mono); padding: 4px 8px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px; }
    .live.stale { opacity: 0.6; }
    .lk { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); }
    .lv { color: var(--text-bright); }
    .lv.current { font-weight: 700; }
    .lv.dim { color: var(--text-dim); }
    .lv.warn, .current.warn { color: var(--error); }
    .sep { color: var(--text-dim); }
</style>
