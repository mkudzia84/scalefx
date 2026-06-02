<!-- ChannelBandCluster.svelte — canonical "RC channel → multi-band
     selector" widget (Rule 36 + Rule 38).  Reused by every effect that
     picks ONE-OF-N items by driving a selector channel into per-item
     [loUs, hiUs] zones:
       - GunFx rate-of-fire (this widget's first home)
       - LightFx program selector (future — same shape, different items)
       - Alert sound bank picker (future)

     One cluster owns the rows:
       (1) Named-channel selector dropdown (caller picks)
       (2) Live bar with:
              - per-item coloured zone at [usToPct(lo), usToPct(hi)]
              - live µs marker on top (success-green)
              - armed item glow (accent-coloured inner outline)
              - explicit unbounded-band cue (diagonal-stripe overlay) so
                the operator can see a "matches everything" item even
                when a sibling band covers part of the bar
              - NO-SIGNAL striped state when input is dead / unbound
              - overlap-error diagonal-red overlay when any two items
                overlap (Rule 35 → caller gates Apply on hasOverlaps)
       (3) One-line legend with the live µs read, range hint, and
           overlap count (if any).

     Render order — IMPORTANT (fixes the "first item is hidden" bug):
     items render LAST → FIRST so the FIRST item paints ON TOP of
     later siblings.  Combined with the unbounded-band cue, this means
     a [0,0] catch-all item is always visible at the bar's leading
     edge regardless of what later items occupy. -->
<script lang="ts">
    import { usToPct } from '../devicemodel'

    export interface BandItem {
        loUs:  number       // 0 = unbounded low
        hiUs:  number       // 0 = unbounded high
        name:  string       // shown inside the band
        meta?: string       // shown in the tooltip ("550 rpm", "program 3", …)
        color: string       // CSS colour for the zone fill + border
        armed: boolean      // live µs is INSIDE this band right now
    }

    export let title:        string             = 'Selector'
    export let channelLabel: string             = 'Selector channel'
    export let emptyOption:  string             = '— none (use item #1 always) —'
    export let options:      Array<{ id: string; label: string }>
    export let inputId:      string                              // selected fnId
    export let bands:        BandItem[]                          // caller-prepared
    export let overlapIndices: number[]        = []              // 0-based item idx
    export let liveUs:       number | null     = null
    export let liveValid:    boolean           = false
    export let busy:         boolean           = false

    export let onInputChange: (next: string) => void

    /** Is THIS band item rendered as "unbounded" (i.e. caller used 0
     *  for lo OR hi)?  Rendered as a diagonal-stripe overlay so the
     *  operator can spot a catch-all even when a sibling covers it. */
    function isUnbounded(b: BandItem): boolean {
        return b.loUs === 0 || b.hiUs === 0
    }
    function bandLo(b: BandItem): number { return b.loUs === 0 ? 1000 : b.loUs }
    function bandHi(b: BandItem): number { return b.hiUs === 0 ? 2000 : b.hiUs }
    function bandLeftPct(b: BandItem):  number { return usToPct(bandLo(b)) }
    function bandWidthPct(b: BandItem): number {
        return Math.max(0.4, usToPct(bandHi(b)) - usToPct(bandLo(b)))
    }

    // Svelte attribute expressions don't accept TS `as` casts — extract.
    function onSelectChange(e: Event) {
        onInputChange((e.target as HTMLSelectElement).value)
    }

    // Stack item #1 on top of items #2..N via inline z-index instead of
    // reversing the array.  Reverse-paint with a keyed each (`(idx)`)
    // produced stale styles on the first item under live edit on
    // 2026-05-24 — Svelte 4 keeps the DOM node but the reactive style
    // string for the topmost (= first source item) sometimes didn't
    // recompute.  Source-order render + z-index sidesteps the bug:
    // every band is in its natural index position, no array spread or
    // reverse, no key, so Svelte refreshes every node on every prop
    // change without keyed-block reconciliation getting in the way. */
    function zIndexFor(idx: number, total: number): number {
        return total - idx;        // item 0 → highest, item N-1 → lowest
    }
</script>

<div class="band-cluster">
    <div class="form-row">
        <span class="field-label">{channelLabel}</span>
        <select class="field-input wide" value={inputId}
                on:change={onSelectChange}
                disabled={busy}
                title="Pick the named channel (defined on the IO tab's inputs[] block).&#10;Each item below arms when the channel value falls within its band.">
            <option value="">{emptyOption}</option>
            {#each options as o}
                <option value={o.id}>{o.label}</option>
            {/each}
        </select>
    </div>

    <!-- Bar shows whenever there are bands — even before a channel is bound
         (shows "no channel bound"), so the band layout is always visible. -->
    {#if bands.length > 0}
        <div class="band-bar"
             class:nosignal={!liveValid}
             class:overlap-error={overlapIndices.length > 0}>
            {#each bands as b, idx}
                <div class="band-zone"
                     class:armed={b.armed}
                     class:unbounded={isUnbounded(b)}
                     style="left:{bandLeftPct(b)}%;
                            width:{bandWidthPct(b)}%;
                            z-index:{zIndexFor(idx, bands.length)};
                            background:color-mix(in srgb, {b.color} 30%, transparent);
                            border-color:color-mix(in srgb, {b.color} 80%, transparent)"
                     title="#{idx + 1} {b.name}{b.meta ? ' · ' + b.meta : ''} · {b.loUs || '∞'}–{b.hiUs || '∞'} µs">
                    {b.name || `#${idx + 1}`}{#if isUnbounded(b)} <span class="band-unbound-tag" title="Unbounded — bandLoUs=0 or bandHiUs=0; catches any pulse outside other bands">∞</span>{/if}
                </div>
            {/each}
            {#if liveValid && liveUs != null}
                <div class="band-mark" style="left:{usToPct(liveUs)}%"
                     title="live: {liveUs} µs"></div>
            {/if}
            {#if !liveValid}
                <span class="band-nosignal">{inputId ? 'NO SIGNAL' : 'no channel bound'}</span>
            {/if}
        </div>
        <div class="band-legend">
            <span class="leg-live">
                {#if liveValid && liveUs != null}{liveUs} µs{:else}—{/if}
                <span class="leg-label">live</span>
            </span>
            <span class="leg-sep">·</span>
            <span class="leg-count">{bands.length} items</span>
            {#if overlapIndices.length > 0}
                <span class="leg-sep">·</span>
                <span class="leg-overlap">⚠ {overlapIndices.length} overlap</span>
            {/if}
            <span class="leg-range" title="RC PPM/SBUS range — 1000µs is min stick, 2000µs is max stick">1000–2000 µs</span>
        </div>
    {/if}
</div>

<style>
    /* Card chrome — matches ChannelToggleCluster so the two widgets
       sit side-by-side without visual mismatch. */
    .band-cluster { background: var(--bg-raised); border: 1px solid var(--border); border-radius: 6px; padding: 8px 10px 6px; margin: 6px 0 14px; }
    .band-cluster :global(.form-row) { margin-bottom: 6px; }

    .band-bar { position: relative; height: 22px; margin: 4px 0 2px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; }
    .band-bar.nosignal { background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 6px, transparent 6px, transparent 12px); }
    .band-bar.overlap-error { box-shadow: inset 0 0 0 2px var(--error); }
    .band-bar.overlap-error::after { content: ''; position: absolute; inset: 0; background: repeating-linear-gradient(45deg, transparent, transparent 6px, rgba(255,80,80,0.18) 6px, rgba(255,80,80,0.18) 12px); pointer-events: none; z-index: 4; }

    .band-zone { position: absolute; top: 1px; bottom: 1px; border-left: 1px solid; border-right: 1px solid; display: flex; align-items: center; justify-content: center; gap: 3px; font-size: 9px; font-family: var(--font-mono); color: var(--text-bright); text-shadow: 0 0 3px rgba(0,0,0,0.7); white-space: nowrap; overflow: hidden; cursor: help; }
    .band-zone.armed { box-shadow: inset 0 0 0 2px var(--accent); z-index: 3; }
    /* Unbounded bands get a diagonal-stripe overlay so the operator
       can tell a catch-all from a regular band even when colour mixing
       puts them close to each other.  The pseudo-element overlays
       inside the zone (matched border-radius via inherit). */
    .band-zone.unbounded::before {
        content: ''; position: absolute; inset: 0; pointer-events: none;
        background: repeating-linear-gradient(45deg, transparent 0 4px, rgba(255,255,255,0.12) 4px 8px);
    }
    .band-unbound-tag { font-weight: 700; font-size: 11px; opacity: 0.9; }

    .band-mark { position: absolute; top: -3px; bottom: -3px; width: 2px; background: var(--success); box-shadow: 0 0 4px rgba(100,255,150,0.7); transform: translateX(-1px); pointer-events: none; z-index: 5; }
    .band-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-family: var(--font-mono); font-size: 9px; letter-spacing: 0.5px; color: var(--text-dim); }

    .band-legend { display: flex; align-items: center; gap: 4px; font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); margin-top: 2px; }
    .leg-live    { color: var(--success); font-weight: 600; }
    .leg-count   { color: var(--text-dim); }
    .leg-overlap { color: var(--error); font-weight: 600; }
    .leg-range   { margin-left: auto; opacity: 0.6; }
    .leg-sep     { opacity: 0.4; }
    .leg-label   { color: var(--text-dim); font-weight: 400; text-transform: uppercase; letter-spacing: 0.3px; font-size: 9px; margin-left: 2px; }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
</style>
