<!-- ServoIoWidget — reusable per-servo I/O status (Rule 34 / 42).
     Pairs the live SIGNAL INPUT (RC channel value, 1000–2000 µs) with the live
     SERVO OUTPUT (two lines over the [min,max] travel band, like the
     calibration view): a solid RED line = the ACTUAL position (incl. recoil
     kicks) and a dashed YELLOW line = the commanded TARGET the profile is
     slewing to.  Generic + read-only: any panel that drives a servo from a
     channel reuses this; config lives in the owning panel +
     ServoCalibrationDialog.  Live servo position comes from the generic
     servo_status stream (the caller looks it up by PortRef and passes it in). -->
<script lang="ts">
    import { usToPct } from '../devicemodel'

    export let signalLabel = 'Signal input'
    export let outputLabel = 'Servo output'

    // Signal input (RC channel).  hasInput=false ⇒ no channel bound (input row
    // hidden).  inputUs/inputValid are the live decoded value.
    export let hasInput = false
    export let inputUs: number | null = null
    export let inputValid = false
    export let neutralUs = 1500

    // Servo presence + profile (travel + centre + REV).
    export let hasServo = true
    export let minUs = 1000
    export let maxUs = 2000
    export let centerUs = 1500
    export let reversed = false

    // Live servo telemetry ({posUs,...}) or null when not streaming yet.
    export let servo: { posUs: number; targetUs: number; velUsPerS: number } | null = null

    // Fixed physical-servo envelope so the configured [min,max] reads as a band
    // inside a wider track (mirrors the calibration slider-range).
    const SRV_ENV_LO = 500, SRV_ENV_HI = 2500
    const srvPct = (us: number) =>
        Math.max(0, Math.min(100, ((us - SRV_ENV_LO) / (SRV_ENV_HI - SRV_ENV_LO)) * 100))

    $: live  = !!servo && servo.posUs > 0
    $: inUs  = (hasInput && inputValid && inputUs != null) ? inputUs : neutralUs
    $: cmdUs = Math.min(maxUs, Math.max(minUs, inUs))
    $: posUs = live ? servo!.posUs : cmdUs       // actual position (solid line)
    $: tgtUs = live ? servo!.targetUs : 0        // commanded target (dashed line)
    $: showTgt = live && tgtUs > 0               // hide until telemetry streams
</script>

<div class="srv-io">
    {#if hasInput}
        <div class="io-label">{signalLabel}</div>
        <div class="io-bar" class:nosignal={!inputValid}>
            <div class="io-neutral" style="left:{usToPct(neutralUs)}%" title="neutral {neutralUs} µs"></div>
            {#if inputValid && inputUs != null}
                <div class="io-fill" style="width:{usToPct(inputUs)}%"></div>
                <span class="io-readout">{inputUs} µs</span>
            {:else}
                <span class="io-nosignal">NO SIGNAL</span>
            {/if}
        </div>
    {/if}

    {#if hasServo}
        <div class="io-label">{outputLabel}
            <span class="io-sub">{posUs} µs{showTgt && tgtUs !== posUs ? ` → ${tgtUs}` : ''}{live ? '' : ' · cmd'}{reversed ? ' · ↔ rev' : ''}</span>
        </div>
        <div class="servo-track" class:cmd-only={!live}
             title="Servo at {posUs} µs{showTgt ? ` → target ${tgtUs} µs` : ''} · travel {minUs}–{maxUs} µs · centre {centerUs} µs · open/deploy = {reversed ? 'MIN' : 'MAX'} end{reversed ? ' (reversed)' : ''}">
            <div class="servo-track-range"
                 style="left:{srvPct(minUs)}%; width:{Math.max(0.5, srvPct(maxUs) - srvPct(minUs))}%"></div>
            <div class="servo-track-center" style="left:{srvPct(centerUs)}%"></div>
            <!-- OPEN-end marker: the µs end that "open/deploy" intent drives to.
                 With ↔ Reversed this is the MIN end — the raw-µs bar is NOT
                 mirrored, so without the marker an open door reading near min
                 looks wrong even though it is right (bench confusion,
                 2026-08-08: "the widget mirrors it"). -->
            <div class="servo-track-open" style="left:{srvPct(reversed ? minUs : maxUs)}%"
                 title="OPEN / DEPLOY end ({reversed ? minUs : maxUs} µs{reversed ? ' — reversed' : ''})">▾</div>
            {#if showTgt}
                <div class="servo-track-target" style="left:{srvPct(tgtUs)}%" title="target {tgtUs} µs"></div>
            {/if}
            <div class="servo-track-pos" style="left:{srvPct(posUs)}%" title="actual {posUs} µs"></div>
        </div>
        {#if live}
            <div class="srv-legend">
                <span class="srv-legend-item"><span class="swatch pos"></span>actual</span>
                <span class="srv-legend-item"><span class="swatch tgt"></span>target</span>
            </div>
        {/if}
    {/if}
</div>

<style>
    .srv-io { display: flex; flex-direction: column; }
    .io-label { font-size: 9px; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-top: 4px; }
    .io-label .io-sub { text-transform: none; letter-spacing: 0; font-family: var(--font-mono); opacity: 0.85; margin-left: 4px; }

    /* Signal-input value bar (green fill over the 1000–2000 RC range). */
    .io-bar { position: relative; height: 12px; margin: 3px 0 4px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; }
    .io-bar.nosignal { background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 6px, transparent 6px, transparent 12px); }
    .io-fill { height: 100%; background: linear-gradient(90deg, var(--accent), var(--success)); transition: width 0.08s linear; }
    .io-neutral { position: absolute; top: -2px; bottom: -2px; width: 1px; background: var(--text-dim); pointer-events: none; }
    .io-readout { position: absolute; right: 6px; top: 0; line-height: 12px; font-family: var(--font-mono); font-size: 9px; color: var(--text-bright); text-shadow: 0 0 3px rgba(0,0,0,0.7); }
    .io-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-family: var(--font-mono); font-size: 9px; letter-spacing: 0.5px; color: var(--text-dim); }

    /* Servo-output track — thin line at the live position + [min,max] band. */
    .servo-track { position: relative; height: 12px; margin: 3px 0 4px; }
    .servo-track::before { content: ''; position: absolute; left: 0; right: 0; top: 5px; height: 2px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 1px; }
    .servo-track-range { position: absolute; top: 3px; height: 6px; background: color-mix(in srgb, var(--accent) 18%, transparent); border: 1px solid color-mix(in srgb, var(--accent) 45%, transparent); border-radius: 2px; pointer-events: none; }
    .servo-track-center { position: absolute; top: 0; bottom: 0; width: 0; border-left: 1px dashed color-mix(in srgb, var(--text-dim) 60%, transparent); pointer-events: none; }
    /* OPEN/DEPLOY-end marker (▾ above the band) — intent anchor so a
       reversed servo sitting at MIN reads as "open" at a glance. */
    .servo-track-open { position: absolute; top: -7px; margin-left: -4px; font-size: 8px; line-height: 8px; color: var(--success); cursor: help; }
    /* TARGET — dashed YELLOW line (where the servo is slewing to). */
    .servo-track-target { position: absolute; top: 0; bottom: 0; width: 0; border-left: 2px dashed var(--warning); opacity: 0.95; transition: left 0.06s linear; pointer-events: none; }
    /* ACTUAL — bright solid RED line (the live position, incl. recoil kick). */
    .servo-track-pos { position: absolute; top: -1px; bottom: -1px; width: 2px; margin-left: -1px; background: var(--error); box-shadow: 0 0 5px var(--error); border-radius: 1px; transition: left 0.06s linear; pointer-events: none; }
    .servo-track.cmd-only .servo-track-pos { background: var(--text-dim); box-shadow: none; opacity: 0.6; }

    /* Colour-key so red=actual / yellow=target is unambiguous even when the
       two lines overlap at rest. */
    .srv-legend { display: flex; gap: 10px; margin: 1px 0 4px; }
    .srv-legend-item { display: inline-flex; align-items: center; gap: 4px; font-size: 9px; color: var(--text-dim); }
    .srv-legend .swatch { width: 10px; height: 0; border-top-width: 2px; border-top-style: solid; }
    .srv-legend .swatch.pos { border-top-color: var(--error); }
    .srv-legend .swatch.tgt { border-top-style: dashed; border-top-color: var(--warning); }
</style>
