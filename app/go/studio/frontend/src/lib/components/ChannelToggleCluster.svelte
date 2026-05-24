<!-- ChannelToggleCluster.svelte — canonical "RC channel → boolean
     dispatch" widget (Rule 36).  Reused by every effect that needs to
     gate an action on a µs threshold:
       - EngineFx engine on/off (running ↔ stopped)
       - GunFx per-gun trigger (fire ↔ idle)
       - Future LightFx mode-switch (program A ↔ B), Alert mute, …

     One cluster owns four rows in a single .chan-cluster card:
       (1) Named-channel selector dropdown
       (2) Inline "Fires when channel ≥ N µs ± H µs" trigger settings
       (3) Live channel bar (.bar.tall) with:
              - bar-fill at the live µs (success-green)
              - solid threshold-mark at thresholdUs (error-red)
              - translucent hyst-band around it (warning-amber)
              - NO-SIGNAL striped state when the input is dead
       (4) One-line legend color-coded to the markers

     Every input pushes through `onChange()` so the marker + band move
     LIVE as the operator dials thresholds — that's the whole point of
     putting the settings ABOVE the bar.  The cluster is presentational
     only; the caller owns the named-channel options list AND the live
     channel lookup (because both depend on the panel's data sources). -->
<script lang="ts">
    import { usToPct } from '../devicemodel'

    export let title:          string           = 'Channel'   // section label
    export let channelLabel:   string           = 'Channel'   // dropdown label
    export let emptyOption:    string           = '— none —'  // "no channel bound" pick
    export let options:        Array<{ id: string; label: string }>
    export let inputId:        string                          // selected fnId (empty = unbound)
    export let thresholdUs:    number                          // µs the action fires AT
    export let hysteresisUs:   number                          // ±deadband around threshold
    export let liveUs:         number | null    = null         // current channel value
    export let liveValid:      boolean          = false        // false → NO SIGNAL
    export let busy:           boolean          = false
    export let actionVerb:     string           = 'Fires'      // "Fires" / "Triggers" / "Switches"
    export let thresholdMin:   number           = 800
    export let thresholdMax:   number           = 2200
    export let hysteresisMax:  number           = 500

    /** Fired on every change.  Caller mutates its own draft + marks
     *  dirty + persists.  Pure presentational widget — no internal
     *  state. */
    export let onChange: (next: { inputId: string; thresholdUs: number; hysteresisUs: number }) => void

    function setInput(v: string)        { onChange({ inputId: v,        thresholdUs, hysteresisUs }) }
    function setThreshold(v: number)    { onChange({ inputId,           thresholdUs: v, hysteresisUs }) }
    function setHysteresis(v: number)   { onChange({ inputId,           thresholdUs, hysteresisUs: v }) }

    // Svelte attribute expressions don't accept TS `as` casts — every
    // DOM-event handler lives in the script block.
    function onSelectChange(e: Event) { setInput((e.target as HTMLSelectElement).value) }
    function onThresholdChange(e: Event) { setThreshold(Number((e.target as HTMLInputElement).value)) }
    function onHysteresisChange(e: Event) { setHysteresis(Number((e.target as HTMLInputElement).value)) }
</script>

<!-- Card-style cluster so the four rows visually group as ONE widget. -->
<div class="chan-cluster">
    <div class="form-row">
        <span class="field-label">{channelLabel}</span>
        <select class="field-input wide" value={inputId}
                on:change={onSelectChange}
                disabled={busy}
                title="Pick the named channel (defined on the IO tab's inputs[] block).&#10;Leave empty for manual control only.">
            <option value="">{emptyOption}</option>
            {#each options as o}
                <option value={o.id}>{o.label}</option>
            {/each}
        </select>
    </div>

    <div class="form-row trigger-row"
         title="{actionVerb} when the channel value rises past threshold; hysteresis is the deadband that prevents stick jitter from re-triggering.">
        <span class="field-label">{actionVerb} when channel ≥</span>
        <input class="field-input narrow" type="number" min={thresholdMin} max={thresholdMax} step="10"
               value={thresholdUs}
               on:change={onThresholdChange}
               disabled={busy} />
        <span class="unit">µs</span>
        <span class="trigger-pm">±</span>
        <input class="field-input narrow" type="number" min="0" max={hysteresisMax} step="5"
               value={hysteresisUs}
               on:change={onHysteresisChange}
               disabled={busy} />
        <span class="unit">µs hysteresis</span>
    </div>

    <div class="bar tall" class:nosignal={!liveValid}>
        {#if liveValid && liveUs != null}
            <div class="bar-fill" style="width: {usToPct(liveUs)}%"></div>
        {/if}
        <div class="hyst-band"
             style="left: {usToPct(thresholdUs - hysteresisUs)}%;
                    width: {Math.max(0.4, usToPct(thresholdUs + hysteresisUs) - usToPct(thresholdUs - hysteresisUs))}%"
             title="hysteresis band — ±{hysteresisUs} µs around threshold"></div>
        <div class="threshold-mark" style="left: {usToPct(thresholdUs)}%"
             title="threshold {thresholdUs} µs"></div>
        {#if !liveValid}
            <span class="bar-nosignal">{inputId ? 'NO SIGNAL' : 'no channel bound — manual only'}</span>
        {/if}
    </div>

    <div class="bar-legend">
        <span class="leg-live">
            {#if liveValid && liveUs != null}{liveUs} µs{:else}—{/if}
            <span class="leg-label">live</span>
        </span>
        <span class="leg-sep">·</span>
        <span class="leg-mark">{thresholdUs} µs <span class="leg-label">threshold</span></span>
        <span class="leg-sep">·</span>
        <span class="leg-band">±{hysteresisUs} µs <span class="leg-label">hysteresis</span></span>
        <span class="leg-sep">·</span>
        <span class="leg-range" title="RC PPM/SBUS range — 1000µs is min stick, 2000µs is max stick">1000–2000 µs</span>
    </div>
</div>

<style>
    /* All styling consumes the design-system tokens (Rule 34).  The
       widget is layout-only — colours come from CSS vars; the .bar
       chrome + .field-input + .form-row classes are global. */
    .chan-cluster { background: var(--bg-raised); border: 1px solid var(--border); border-radius: 6px; padding: 8px 10px 6px; margin: 6px 0 14px; }
    .chan-cluster :global(.form-row) { margin-bottom: 6px; }
    .chan-cluster :global(.form-row:last-of-type) { margin-bottom: 0; }
    .trigger-row { font-size: 11px; color: var(--text-dim); }

    /* Bar — height 18 so the threshold-mark + hyst-band are clearly
       readable.  Positioning is relative so children layer correctly. */
    .bar { position: relative; height: 12px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; }
    .bar.tall { height: 18px; margin-bottom: 4px; }
    .bar-fill { position: absolute; top: 0; bottom: 0; left: 0; background: var(--success); opacity: 0.7; }
    .bar.nosignal {
        background: repeating-linear-gradient(135deg, var(--bg-input) 0 8px, color-mix(in srgb, var(--text-dim) 8%, var(--bg-input)) 8px 16px);
    }
    .bar-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-size: 9px; font-weight: 700; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.5px; z-index: 3; }
    .hyst-band { position: absolute; top: 0; bottom: 0; background: color-mix(in srgb, var(--warning) 28%, transparent); border-left: 1px dashed color-mix(in srgb, var(--warning) 70%, transparent); border-right: 1px dashed color-mix(in srgb, var(--warning) 70%, transparent); pointer-events: none; z-index: 1; }
    .threshold-mark { position: absolute; top: -2px; bottom: -2px; width: 2px; background: var(--error); box-shadow: 0 0 4px rgba(255,80,80,0.6); pointer-events: none; z-index: 2; transform: translateX(-1px); }

    /* Legend — one-liner under the bar.  Colours mirror the markers so
       the operator pairs "live=green" / "threshold=red" / "hyst=amber"
       without re-reading. */
    .bar-legend { display: flex; align-items: center; gap: 4px; font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); margin-top: 2px; }
    .leg-live  { color: var(--success); font-weight: 600; }
    .leg-mark  { color: var(--error);   font-weight: 600; }
    .leg-band  { color: var(--warning); font-weight: 600; }
    .leg-range { margin-left: auto; opacity: 0.6; }
    .leg-sep   { opacity: 0.4; }
    .leg-label { color: var(--text-dim); font-weight: 400; text-transform: uppercase; letter-spacing: 0.3px; font-size: 9px; margin-left: 2px; }

    .unit       { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
    .trigger-pm { margin: 0 4px; color: var(--text-dim); font-family: var(--font-mono); font-weight: 700; }
    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
</style>
