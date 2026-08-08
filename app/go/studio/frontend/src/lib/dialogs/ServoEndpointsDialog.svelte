<!-- ServoEndpointsDialog — set a function's two named positions (2.46.0).

     The explicit-position model: door open/close, strut deploy/retract,
     landing open/close are ABSOLUTE µs numbers stored in the effect's
     config.  This dialog live-jogs the servo INSIDE its calibrated caps
     (no widen — positions must live inside the caps by definition),
     captures the two positions, and Save persists them immediately via
     the owner's onSave.

     Workflow: jog to where the mechanism should sit for A → "Set as A" →
     jog to B → "Set as B" → Save.  ◈ preview buttons drive the servo to
     the current draft values for a look-before-save. -->
<script lang="ts">
    import {
        openEndpoints,
        jogEndpoint, jogEndpointTo,
        captureA, captureB, previewA, previewB,
        setEndpointField, saveEndpoints, cancelEndpoints,
    } from '../endpoint_setter'

    $: state = $openEndpoints

    function handleKeydown(e: KeyboardEvent) {
        if (!state || state.busy) return
        if (e.key === 'Escape') cancelEndpoints()
        else if (e.key === 'ArrowLeft')  jogEndpoint(e.shiftKey ? -10 : -1)
        else if (e.key === 'ArrowRight') jogEndpoint(e.shiftKey ?  10 :  1)
        else if (e.key === 'PageDown')   jogEndpoint(-50)
        else if (e.key === 'PageUp')     jogEndpoint(50)
    }

    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }

    const JOG_BUTTONS = [-100, -50, -10, -1, +1, +10, +50, +100] as const

    function pct(us: number): number {
        if (!state) return 0
        const span = state.maxUs - state.minUs
        if (span <= 0) return 0
        return Math.max(0, Math.min(100, ((us - state.minUs) / span) * 100))
    }
</script>

<svelte:window on:keydown={handleKeydown} />

{#if state}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="modal-backdrop"
         on:click|self={() => !state.busy && cancelEndpoints()}>
        <div class="modal ep-modal">
            <div class="modal-header">
                <h2>Set {state.labelA.toLowerCase()}/{state.labelB.toLowerCase()} positions</h2>
                <span class="path">{state.portLabel}</span>
                <button class="close-btn" on:click={cancelEndpoints}
                        disabled={state.busy} title="Close without saving (Esc)">✕</button>
            </div>

            {#if state.error}<div class="banner err">{state.error}</div>{/if}
            {#if state.busy}<div class="banner note">Talking to the servo…</div>{/if}

            <div class="modal-body">
                <div class="section-head">
                    Live jog
                    <span class="hint">within the calibrated {state.minUs}–{state.maxUs} µs caps · arrows ±1 µs (shift ×10) · PgUp/Dn ±50</span>
                </div>

                <div class="jog-readout">
                    <span class="jog-current">{state.currentUs} µs</span>
                </div>

                <div class="slider-wrap">
                    <div class="slider-track">
                        <div class="slider-mark mark-a" style="left:{pct(state.aUs)}%"
                             title="{state.labelA} {state.aUs} µs"></div>
                        <div class="slider-mark mark-b" style="left:{pct(state.bUs)}%"
                             title="{state.labelB} {state.bUs} µs"></div>
                    </div>
                    <input class="slider" type="range"
                           min={state.minUs} max={state.maxUs} step="1"
                           value={state.currentUs} disabled={state.busy}
                           on:input={(e) => jogEndpointTo(numValue(e))} />
                </div>

                <div class="jog-buttons">
                    {#each JOG_BUTTONS as d}
                        <button class="small jog-btn" class:neg={d < 0} class:pos={d > 0}
                                on:click={() => jogEndpoint(d)} disabled={state.busy}>
                            {d > 0 ? '+' : ''}{d}
                        </button>
                    {/each}
                </div>

                <!-- Two endpoint rows: capture / preview / numeric -->
                <div class="ep-row">
                    <span class="ep-label a">{state.labelA}</span>
                    <button class="small" on:click={captureA} disabled={state.busy}
                            title="Use the current jog position as the {state.labelA} position">⤓ Set as {state.labelA}</button>
                    <button class="small" on:click={previewA} disabled={state.busy}
                            title="Drive the servo to the saved {state.labelA} position">◈ Go</button>
                    <input class="field-input narrow" type="number"
                           min={state.minUs} max={state.maxUs} step="1"
                           value={state.aUs} disabled={state.busy}
                           on:change={(e) => setEndpointField('a', numValue(e))} />
                    <span class="unit">µs</span>
                </div>
                <div class="ep-row">
                    <span class="ep-label b">{state.labelB}</span>
                    <button class="small" on:click={captureB} disabled={state.busy}
                            title="Use the current jog position as the {state.labelB} position">⤓ Set as {state.labelB}</button>
                    <button class="small" on:click={previewB} disabled={state.busy}
                            title="Drive the servo to the saved {state.labelB} position">◈ Go</button>
                    <input class="field-input narrow" type="number"
                           min={state.minUs} max={state.maxUs} step="1"
                           value={state.bUs} disabled={state.busy}
                           on:change={(e) => setEndpointField('b', numValue(e))} />
                    <span class="unit">µs</span>
                </div>

                <div class="ep-note hint">
                    No direction flags anywhere: which number is bigger IS the
                    direction.  The servo's calibration (⚙ Calibrate on the IO
                    tab) caps the range and sets the motion speed.
                </div>
            </div>

            <div class="modal-footer">
                <button class="small" on:click={cancelEndpoints} disabled={state.busy}>Cancel</button>
                <button class="small primary" on:click={saveEndpoints} disabled={state.busy}
                        title="Persist both positions to the effect config immediately">
                    Save
                </button>
            </div>
        </div>
    </div>
{/if}

<style>
    .modal-backdrop {
        position: fixed; inset: 0;
        background: rgba(0, 0, 0, 0.65);
        display: flex; align-items: center; justify-content: center;
        z-index: 1000;
    }
    .ep-modal {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        width: min(96vw, 620px);
        max-height: 94vh;
        display: flex; flex-direction: column;
        box-shadow: 0 10px 40px rgba(0, 0, 0, 0.5);
    }
    .modal-header { display: flex; align-items: center; gap: 12px; padding: 10px 14px; border-bottom: 1px solid var(--border); }
    .modal-header h2 { margin: 0; font-size: 15px; font-weight: 600; color: var(--text-bright); }
    .modal-header .path { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); flex: 1; }
    .close-btn { background: transparent; border: 0; color: var(--text-dim); cursor: pointer; font-size: 18px; padding: 0 4px; }
    .close-btn:hover { color: var(--text-bright); }

    .modal-body { flex: 1; min-height: 0; overflow: auto; padding: 12px 14px; }
    .modal-footer { display: flex; align-items: center; gap: 8px; justify-content: flex-end; padding: 8px 14px; border-top: 1px solid var(--border); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 4px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    .section-head .hint { font-size: 9px; font-weight: 400; text-transform: none; letter-spacing: 0; color: var(--text-dim); font-style: italic; }

    .jog-readout { display: flex; align-items: baseline; gap: 8px; margin: 4px 0 6px; }
    .jog-current { font-family: var(--font-mono); font-size: 24px; font-weight: 700; color: var(--text-bright); }

    .slider-wrap { position: relative; height: 36px; margin: 6px 0 8px; }
    .slider-track { position: absolute; left: 0; right: 0; top: 14px; height: 8px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px; }
    .slider-mark { position: absolute; top: -4px; bottom: -4px; width: 3px; transform: translateX(-1px); border-radius: 1px; }
    .slider-mark.mark-a { background: var(--success); }
    .slider-mark.mark-b { background: var(--warning); }
    .slider { position: absolute; left: 0; right: 0; top: 0; width: 100%; height: 36px; background: transparent; appearance: none; cursor: pointer; }
    .slider::-webkit-slider-runnable-track { height: 36px; background: transparent; }
    .slider::-moz-range-track             { height: 36px; background: transparent; border: 0; }
    .slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 16px; height: 28px; border-radius: 3px; background: var(--text-bright); border: 1px solid var(--border); cursor: grab; }
    .slider::-moz-range-thumb     { width: 16px; height: 28px; border-radius: 3px; background: var(--text-bright); border: 1px solid var(--border); cursor: grab; }

    .jog-buttons { display: grid; grid-template-columns: repeat(8, 1fr); gap: 4px; margin: 4px 0 10px; }
    .jog-btn { font-family: var(--font-mono); font-weight: 600; }
    .jog-btn.neg { color: var(--warning); }
    .jog-btn.pos { color: var(--success); }

    .ep-row { display: flex; align-items: center; gap: 8px; margin: 6px 0; }
    .ep-label { min-width: 70px; font-size: 12px; font-weight: 600; }
    .ep-label.a { color: var(--success); }
    .ep-label.b { color: var(--warning); }
    .field-input.narrow { width: 84px; }
    .unit { font-size: 10px; color: var(--text-dim); }

    .ep-note { margin-top: 10px; }
</style>
