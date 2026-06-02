<!-- LightTimelineTrack — visual editor for one LED channel's event sequence.

     Replaces the dense 9-column events table (LightFxPanel) with a strip that
     DRAWS the animation: the filled area is brightness(t) sampled across the
     whole sequence (so you read shape + timing at a glance), and each event is
     a clickable band above it.  Clicking a band selects it and opens a compact
     inline editor showing ONLY the fields that kind uses (FIELD_MAP) — no more
     columns of dimmed inputs.

     Pure UI: reads `events` + `loop`, emits edits through callbacks.  The
     parent owns the draft store + mutators (addEvent / setEvent / removeEvent).
     The brightness sampling is a display approximation of the firmware
     LedAnimator semantics — good enough to author against; the on-device
     Preview (▶ on the channel header) is the source of truth.
-->
<script lang="ts">
    import type { ProgramEventT, LightEventKindT } from '../lightfx'

    export let events: ProgramEventT[] = []
    export let loop = false
    export let disabled = false
    /** Mutators owned by the parent (operate on the track's events list). */
    export let onSet: (ei: number, patch: Partial<ProgramEventT>) => void = () => {}
    export let onAdd: () => void = () => {}
    export let onRemove: (ei: number) => void = () => {}

    const KIND_OPTIONS: { id: LightEventKindT; label: string }[] = [
        { id: 'on',       label: 'On (steady)' },
        { id: 'off',      label: 'Off (gap)' },
        { id: 'flash',    label: 'Flash (square)' },
        { id: 'fade_in',  label: 'Fade in' },
        { id: 'fade_out', label: 'Fade out' },
        { id: 'fading',   label: 'Fading (sine)' },
        { id: 'beacon',   label: 'Beacon (peak+base)' },
    ]
    // Which numeric fields each kind actually uses (drives the inline editor).
    const FIELD_MAP: Record<LightEventKindT, Array<keyof ProgramEventT>> = {
        on:       ['brightnessPct', 'durationMs'],
        off:      ['durationMs'],
        flash:    ['cycleMs', 'brightnessPct', 'flashPct', 'durationMs'],
        fade_in:  ['durationMs', 'brightnessPct'],
        fade_out: ['durationMs', 'brightnessPct'],
        fading:   ['cycleMs', 'minPct', 'maxPct', 'durationMs'],
        beacon:   ['cycleMs', 'minPct', 'maxPct', 'flashPct', 'durationMs'],
    }
    const FIELD_LABEL: Partial<Record<keyof ProgramEventT, string>> = {
        durationMs: 'duration', cycleMs: 'cycle', brightnessPct: 'brightness',
        minPct: 'min', maxPct: 'max', flashPct: 'flash',
    }
    const FIELD_UNIT: Partial<Record<keyof ProgramEventT, string>> = {
        durationMs: 'ms', cycleMs: 'ms', brightnessPct: '%', minPct: '%', maxPct: '%', flashPct: '%',
    }
    const KIND_COLOR: Record<LightEventKindT, string> = {
        on: 'var(--success)', off: 'var(--text-dim)', flash: 'var(--accent)',
        fade_in: 'var(--accent)', fade_out: 'var(--accent)',
        fading: 'var(--warning)', beacon: 'var(--warning)',
    }
    const KIND_GLYPH: Record<LightEventKindT, string> = {
        on: '▀', off: '▁', flash: '▕▏', fade_in: '◢', fade_out: '◣', fading: '∿', beacon: '⏶',
    }

    let selected = -1

    // ── Layout: each event gets a horizontal band; width ∝ duration.  A
    //    0-duration (terminal / hold) event gets a fixed display width + an ∞
    //    marker — it runs until the next program switch. ──────────────────────
    const TERMINAL_MS = 600
    function dispMs(e: ProgramEventT): number { return e.durationMs > 0 ? e.durationMs : TERMINAL_MS }

    interface Seg { ev: ProgramEventT; i: number; x0: number; w: number; terminal: boolean }
    $: segs = (() => {
        const total = events.reduce((a, e) => a + dispMs(e), 0) || 1
        let acc = 0
        return events.map((ev, i) => {
            const w = dispMs(ev) / total
            const s: Seg = { ev, i, x0: acc, w, terminal: ev.durationMs === 0 }
            acc += w
            return s
        })
    })()
    $: periodMs = events.filter(e => e.durationMs > 0).reduce((a, e) => a + e.durationMs, 0)

    // ── Brightness sampling → SVG area polygon (viewBox 0..1000 × 0..100). ───
    function eventPoints(e: ProgramEventT): Array<[number, number]> {
        const b = clamp(e.brightnessPct), lo = clamp(e.minPct), hi = clamp(e.maxPct)
        switch (e.kind) {
            case 'on':       return [[0, b], [1, b]]
            case 'off':      return [[0, 0], [1, 0]]
            case 'fade_in':  return [[0, 0], [1, b]]
            case 'fade_out': return [[0, b], [1, 0]]
            case 'flash': {
                const cyc = Math.max(1, Math.round(dispMs(e) / Math.max(1, e.cycleMs)))
                const n = Math.min(cyc, 24), hiFrac = clamp(e.flashPct) / 100
                const pts: Array<[number, number]> = []
                for (let k = 0; k < n; k++) {
                    const a = k / n, m = (k + hiFrac) / n, z = (k + 1) / n
                    pts.push([a, 0], [a, b], [m, b], [m, 0], [z, 0])
                }
                return pts
            }
            case 'fading': {
                const cyc = Math.max(1, dispMs(e) / Math.max(1, e.cycleMs))
                const steps = Math.min(120, Math.max(12, Math.round(cyc * 12)))
                const pts: Array<[number, number]> = []
                for (let k = 0; k <= steps; k++) {
                    const t = k / steps
                    const ph = 0.5 - 0.5 * Math.cos(2 * Math.PI * cyc * t)
                    pts.push([t, lo + (hi - lo) * ph])
                }
                return pts
            }
            case 'beacon': {
                const cyc = Math.max(1, Math.round(dispMs(e) / Math.max(1, e.cycleMs)))
                const n = Math.min(cyc, 24), hiFrac = clamp(e.flashPct) / 100
                const pts: Array<[number, number]> = []
                for (let k = 0; k < n; k++) {
                    const a = k / n, m = (k + hiFrac) / n, z = (k + 1) / n
                    pts.push([a, lo], [a, hi], [m, hi], [m, lo], [z, lo])
                }
                return pts
            }
        }
        return [[0, 0], [1, 0]]
    }
    $: areaPoints = (() => {
        if (!segs.length) return ''
        const xy: string[] = ['0,100']
        for (const s of segs) {
            for (const [lf, pct] of eventPoints(s.ev)) {
                const x = (s.x0 + lf * s.w) * 1000
                xy.push(`${x.toFixed(1)},${(100 - clamp(pct)).toFixed(1)}`)
            }
        }
        xy.push('1000,100')
        return xy.join(' ')
    })()

    function clamp(v: number): number { return v < 0 ? 0 : v > 100 ? 100 : v }
    function num(e: Event): number {
        const v = Number((e.target as HTMLInputElement).value)
        return Number.isFinite(v) ? v : 0
    }
    function selectSeg(i: number) { selected = selected === i ? -1 : i }
    $: if (selected >= events.length) selected = -1
    $: sel = selected >= 0 ? events[selected] : null

    $: periodLabel = loop
        ? 'loop ' + (periodMs % 1000 ? (periodMs / 1000).toFixed(2) : (periodMs / 1000).toFixed(0)) + ' s'
        : 'one-shot'
    function setField(f: keyof ProgramEventT, e: Event) {
        const patch = { [f]: num(e) } as Partial<ProgramEventT>
        onSet(selected, patch)
    }
    function maxFor(f: keyof ProgramEventT): number | undefined {
        return String(f).endsWith('Pct') ? 100 : undefined
    }
    function stepFor(f: keyof ProgramEventT): number {
        return String(f).endsWith('Pct') ? 1 : 10
    }
    function bandTitle(s: Seg): string {
        const dur = s.terminal ? ' · holds (∞)' : ' · ' + s.ev.durationMs + ' ms'
        return s.ev.kind + dur + ' — click to edit'
    }
    function bandKindLabel(s: Seg): string {
        return s.ev.kind + (s.terminal ? ' ∞' : '')
    }
    function setKind(e: Event) {
        const kind = (e.target as HTMLSelectElement).value as LightEventKindT
        onSet(selected, { kind })
    }
    function removeSelected() {
        onRemove(selected)
        selected = -1
    }
</script>

<div class="tl">
    <!-- Strip: brightness area + clickable per-event bands -->
    <div class="strip" class:looping={loop}>
        <svg class="curve" viewBox="0 0 1000 100" preserveAspectRatio="none" aria-hidden="true">
            <polygon points={areaPoints} />
        </svg>
        <div class="bands">
            {#each segs as s (s.i)}
                <button class="band" class:sel={selected === s.i} class:terminal={s.terminal}
                        style="left:{(s.x0 * 100).toFixed(2)}%; width:{(s.w * 100).toFixed(2)}%; --kc:{KIND_COLOR[s.ev.kind]}"
                        on:click={() => selectSeg(s.i)} disabled={disabled}
                        title={bandTitle(s)}>
                    <span class="band-glyph">{KIND_GLYPH[s.ev.kind]}</span>
                    <span class="band-kind">{bandKindLabel(s)}</span>
                </button>
            {/each}
        </div>
    </div>

    <div class="tl-foot">
        <span class="period">{periodLabel}</span>
        <button class="small add-evt" on:click={onAdd} disabled={disabled} title="Append an event to this channel">+ event</button>
    </div>

    <!-- Inline editor for the selected event: kind + only its fields -->
    {#if sel}
        <div class="evt-edit">
            <span class="ee-idx">#{selected + 1}</span>
            <select class="field-input" value={sel.kind} disabled={disabled}
                    on:change={setKind}
                    title="What this event does">
                {#each KIND_OPTIONS as k}<option value={k.id}>{k.label}</option>{/each}
            </select>
            {#each FIELD_MAP[sel.kind] as f}
                <span class="ee-field">
                    <span class="ee-label">{FIELD_LABEL[f]}</span>
                    <input class="field-input narrow" type="number" min="0"
                           max={maxFor(f)} step={stepFor(f)}
                           value={sel[f]} disabled={disabled}
                           on:change={(e) => setField(f, e)} />
                    <span class="ee-unit">{FIELD_UNIT[f]}</span>
                </span>
            {/each}
            <button class="small danger ee-rm" on:click={removeSelected} disabled={disabled}
                    title="Remove this event">×</button>
        </div>
    {:else}
        <div class="evt-hint">click a band above to edit its event</div>
    {/if}
</div>

<style>
    .tl { display: flex; flex-direction: column; gap: 4px; }

    .strip { position: relative; height: 52px; background: var(--bg-input);
             border: 1px solid var(--border); border-radius: 4px; overflow: hidden; }
    .strip.looping { border-left: 3px solid var(--accent); }
    .curve { position: absolute; inset: 0; width: 100%; height: 100%; }
    .curve polygon { fill: color-mix(in srgb, var(--accent) 30%, transparent);
                     stroke: var(--accent); stroke-width: 0; }

    .bands { position: absolute; inset: 0; display: flex; }
    .band { position: absolute; top: 0; bottom: 0; padding: 2px 3px;
            background: transparent; border: none; border-left: 1px solid var(--border);
            display: flex; flex-direction: column; align-items: flex-start; justify-content: flex-start;
            gap: 1px; cursor: pointer; overflow: hidden; min-width: 0; }
    .band:first-child { border-left: none; }
    .band::before { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 3px; background: var(--kc); opacity: 0.85; }
    .band:hover { background: color-mix(in srgb, var(--accent) 10%, transparent); }
    .band.sel { background: color-mix(in srgb, var(--accent) 18%, transparent); box-shadow: inset 0 0 0 1px var(--accent); }
    .band.terminal { background: repeating-linear-gradient(45deg, transparent, transparent 5px, color-mix(in srgb, var(--text-dim) 14%, transparent) 5px, color-mix(in srgb, var(--text-dim) 14%, transparent) 10px); }
    .band-glyph { font-size: 9px; color: var(--kc); line-height: 1; margin-top: 4px; }
    .band-kind { font-size: 8px; color: var(--text-dim); white-space: nowrap; text-transform: uppercase; letter-spacing: 0.3px; }

    .tl-foot { display: flex; align-items: center; gap: 8px; }
    .period { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
    .add-evt { margin-left: auto; padding: 0 8px; font-size: 11px; }

    .evt-edit { display: flex; align-items: center; gap: 8px; flex-wrap: wrap;
                padding: 5px 8px; background: var(--bg-raised); border: 1px solid var(--border); border-radius: 4px; }
    .ee-idx { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); }
    .ee-field { display: inline-flex; align-items: center; gap: 4px; }
    .ee-label { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); }
    .ee-unit { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
    .evt-edit .field-input.narrow { width: 64px; }
    .ee-rm { margin-left: auto; width: 24px; min-width: 24px; padding: 0; }
    .evt-hint { font-size: 10px; font-style: italic; color: var(--text-dim); padding: 2px; }
</style>
