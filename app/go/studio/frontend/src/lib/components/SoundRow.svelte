<!-- SoundRow.svelte — canonical "sound file path + routing + browse +
     clear" row reused by EnginePanel + GunFxPanel + future LightFx
     sound triggers.

     One row owns:
       - a 72 px field label
       - a wide text input that the operator can hand-type into
       - the shared speaker-routing button (Stereo / Left / Right)
       - browse (…) on the left, Clear on the right (Rule 34 button
         cluster order)

     The speaker-routing button stays ENABLED even when the sound path
     is empty so the operator can pre-select where the next browsed
     file will land — routing is a property of the slot, not the file.
     The Clear button is the only one that hides itself when there's
     nothing to clear (and even then we reserve the slot with a
     .btn-spacer so adjacent rows column-align).

     Required-vs-optional is purely a visual + validation contract:
     `required=true` adds a `*` suffix to the label, swaps the
     placeholder copy, hides Clear (slot reserved with spacer), and
     marks `.invalid` when `error` is non-empty.  The parent panel
     owns validation; this component just renders the verdict. -->
<script lang="ts">
    import {
        cycleOutputMask, speakerLabel, routeShortLabel, speakerIcon,
        speakerStateClass, MASK_STEREO,
    } from './speaker_routing'

    export let label:        string                       // displayed at row start
    export let placeholder:  string         = ''          // shown when value is empty
    export let value:        string                       // current sound-path text
    export let outputMask:   number         = MASK_STEREO // CH1=0x01, CH2=0x02
    export let busy:         boolean        = false       // disables every control
    export let required:     boolean        = false       // hides Clear, '*' on label
    export let error:        string         = ''          // empty = valid; sets .invalid

    /** Fired on every keystroke + after a browse — caller pushes the
     *  new sound path into its config draft. */
    export let onPathChange: (next: string) => void
    /** Fired when the operator clicks the speaker icon — caller
     *  persists the new routing mask (0x01 / 0x02 / 0x03). */
    export let onMaskChange: (next: number) => void
    /** Fired when the operator clicks the … button.  The parent owns
     *  the file picker (different backends per panel — sounds always
     *  live on SD but the picker call is up to the host). */
    export let onBrowse:     () => void
    /** Fired when the operator clicks Clear — caller resets the path
     *  to '' AND re-runs its validator. */
    export let onClear:      () => void

    function handleSpeaker() { onMaskChange(cycleOutputMask(outputMask)) }
    // Svelte attribute expressions don't accept TS `as` casts, so the
    // input-event helper lives in the script block.
    function handleInput(e: Event) {
        onPathChange((e.target as HTMLInputElement).value)
    }
</script>

<div class="sound-row" class:invalid={!!error}>
    <div class="form-row">
        <!-- Optional leading slot — GunFxPanel injects its rof-idx-pill
             placeholder here so this row column-aligns with the row 1
             #N badge above it.  EnginePanel leaves it empty. -->
        <slot name="lead"></slot>
        <span class="field-label" style="width: 72px">{label}{required ? ' *' : ''}</span>
        <input class="field-input wide" type="text"
               {placeholder}
               value={value}
               on:input={handleInput}
               disabled={busy} />
        <!-- Button cluster, Rule 34 order:
                browse (…) → Clear → speaker
             Speaker is RIGHTMOST so it stays visible regardless of
             whether the row is required (Clear slot becomes a hidden
             spacer) or optional (Clear is a real button).  It also
             stays interactive when the path is empty — routing is a
             property of the slot, not the file — so the operator can
             pre-select where the next browsed file will play.  Size
             matches `.btn-slot` so the three buttons column-align
             across stacked rows (required vs optional rows align
             because the speaker takes a fixed slot at the end). -->
        <button class="small btn-slot" on:click={onBrowse} disabled={busy}
                title="Browse SD card">…</button>
        {#if !required}
            <button class="small btn-slot" on:click={onClear}
                    disabled={busy || !value}
                    title="Clear — this sound is optional">Clear</button>
        {:else}
            <span class="btn-slot btn-spacer" aria-hidden="true"></span>
        {/if}
        <button class="small btn-slot route-btn {speakerStateClass(outputMask)}"
                on:click={handleSpeaker}
                disabled={busy}
                title="Speaker routing — click to cycle Stereo → Left → Right.&#10;Current: {speakerLabel(outputMask)}.&#10;Stays available even with no sound picked yet."
                aria-label="Speaker routing: {speakerLabel(outputMask)}">
            <span class="route-icon">{@html speakerIcon(outputMask)}</span>
            <span class="route-text">{routeShortLabel(outputMask)}</span>
        </button>
    </div>
    {#if error}<div class="row-err">⚠ {error}</div>{/if}
</div>

<style>
    /* Inherits .sound-row / .form-row / .field-label / .field-input /
       .btn-slot / .btn-spacer / .row-err from style.css (Rule 34).
       Only the route-* classes are local — those are widget-specific. */

    /* Speaker button — flex row inside the standard .btn-slot footprint.
       Layout: [10px speaker glyph] [4px gap] [short label "L"/"R"/"L+R"].
       The slot is 64 px wide; padding-adjusted inner area ≈ 44 px which
       fits 10 + 4 + ~16 (for "L+R") comfortably.  Glyph stays even on
       L/R mono states (its mute-wave variant signals which side is
       muted) so the operator always sees a speaker shape — same widget
       behaviour whether the path is empty or set. */
    .route-btn {
        display: inline-flex; align-items: center; justify-content: center;
        gap: 3px; padding: 0 4px;
        font-weight: 700; font-family: var(--font-mono); letter-spacing: 0.3px;
        line-height: 1;
    }
    .route-icon { display: inline-flex; align-items: center; }
    .route-icon :global(svg) { display: block; }
    .route-text { font-size: 10px; }
    .route-btn.route-stereo { color: var(--accent); }
    .route-btn.route-left   { color: var(--warning); }
    .route-btn.route-right  { color: var(--success); }
</style>
