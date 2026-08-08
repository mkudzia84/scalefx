<!-- ServoCalibrationDialog — live-jog + persist for one servo port.

     Opened from any feature panel via `openServoCalibrationFor(...)`.
     Replaces the inline ServoProfileEditor for landing-light /
     gun-yaw-pitch / future actuator pickers (Rule 44: per-feature
     editing surface, but with the popup instead of inline so the
     parent form stays compact).

     UX flow:
       1. Open — captures the original profile, pushes a "widened"
          envelope (300–2700 µs, fast slew) so the operator can jog
          across the full physical range without being clamped by
          the operator's prior min/max.  Parks at the current draft
          center.
       2. Jog (+/- buttons OR slider) — sends SERVO_SET_TARGET.
          Jogging never changes the draft limits — sweeping the range
          to explore a mechanism is free.
       3. ⤓ Set as min / ⤒ Set as max / ◼ Set as center — capture
          the current jog target into the corresponding draft field.
          These (plus the numeric inputs) are the ONLY limit writers.
       4. Edit speed / accel / jerk / reversed inline (no wire push;
          batched at Save).
       5. Save — pushes the draft via SetPortProfile (live to role +
          marks /hubfx.yaml dirty); parks at center.
       6. Cancel — restores the original profile + parks at center.
          The next operator action runs against the unchanged baseline.

     Cross-board servos are flagged "not yet supported" — the dialog
     still opens for read-only inspection but jog buttons are disabled
     (the Wails layer NACKs `guid != ''`). -->
<script lang="ts">
    import {
        openServoCalibration,
        cancelServoCalibration, saveServoCalibration,
        jogServo, jogServoTo,
        captureAsMin, captureAsMax, captureAsCenter,
        setDraftField,
    } from '../servo_calibration'

    $: state = $openServoCalibration

    function handleKeydown(e: KeyboardEvent) {
        if (!state || state.busy) return
        // Esc cancels (restores origin).  Save shortcut intentionally
        // not bound — Save is a deliberate "I'm done" action.
        if (e.key === 'Escape') cancelServoCalibration()
        // Arrow / Page keys jog in the dialog.  ←/→ = ±1 µs (fine),
        // shift = ±10, page = ±50.  Operator can also click the
        // buttons; keyboard is for precision tweaks.
        else if (e.key === 'ArrowLeft')  jogServo(e.shiftKey ? -10 : -1)
        else if (e.key === 'ArrowRight') jogServo(e.shiftKey ?  10 :  1)
        else if (e.key === 'PageDown')   jogServo(-50)
        else if (e.key === 'PageUp')     jogServo(50)
    }

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    function checkValue(e: Event): boolean { return (e.target as HTMLInputElement).checked }

    // Jog deltas — chosen to span "fine" (1, 10) and "coarse" (50, 100)
    // tweaks.  Operators jog by 100 to find the rough end-stop, then
    // 10 / 1 to nail the precise limit.
    const JOG_BUTTONS = [-100, -50, -10, -1, +1, +10, +50, +100] as const

    // Slider scale — 800–2200 brackets the normal 1000–2000 RC band with a
    // little headroom for end-stop trim, without letting the operator jog the
    // servo into the far mechanical extremes.  Step = 1 µs (operator can also
    // type into the numeric inputs for exact values).
    const SLIDER_MIN = 800
    const SLIDER_MAX = 2200

    // Visual % for the slider underlay markers (draft min/max bars).
    function pct(us: number): number {
        const p = ((us - SLIDER_MIN) / (SLIDER_MAX - SLIDER_MIN)) * 100
        return Math.max(0, Math.min(100, p))
    }

    $: dirty = state ? JSON.stringify(state.origin) !== JSON.stringify(state.draft) : false
    $: rangeError = state ? state.draft.minUs >= state.draft.maxUs : false
    $: centerOOB  = state ? (state.draft.centerUs < state.draft.minUs
                          || state.draft.centerUs > state.draft.maxUs) : false
    $: travelMs   = state && state.draft.maxSpeedUsPerSec > 0
        ? Math.round((state.draft.maxUs - state.draft.minUs) * 1000 / state.draft.maxSpeedUsPerSec)
        : 0
</script>

<svelte:window on:keydown={handleKeydown} />

{#if state}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="modal-backdrop"
         on:click|self={() => !state.busy && cancelServoCalibration()}>
        <div class="modal cal-modal">
            <div class="modal-header">
                <h2>Calibrate servo</h2>
                <span class="path">{state.portLabel}</span>
                <button class="close-btn" on:click={cancelServoCalibration}
                        disabled={state.busy} title="Cancel (Esc) — restores the original profile">✕</button>
            </div>

            {#if state.error}<div class="banner err">{state.error}</div>{/if}
            {#if state.busy}<div class="banner note">Talking to the servo…</div>{/if}

            <div class="modal-body">

                <!-- ─── Live jog ─────────────────────────────────── -->
                <div class="section-head">
                    Live jog
                    <span class="hint">arrow keys jog ±1 µs (shift ×10) · pageUp/Down jog ±50 µs</span>
                </div>

                <div class="jog-readout">
                    <span class="jog-current">{state.currentUs} µs</span>
                    <span class="jog-of">of {SLIDER_MIN}–{SLIDER_MAX} µs (calibration envelope)</span>
                </div>

                <!-- Slider + min/max underlay markers -->
                <div class="slider-wrap">
                    <div class="slider-track">
                        <div class="slider-range"
                             style="left:{pct(state.draft.minUs)}%; width:{Math.max(0.5, pct(state.draft.maxUs) - pct(state.draft.minUs))}%"
                             title="Draft min/max — current Save target"></div>
                        <div class="slider-mark mark-center"
                             style="left:{pct(state.draft.centerUs)}%"
                             title="Center {state.draft.centerUs} µs"></div>
                    </div>
                    <input class="slider" type="range"
                           min={SLIDER_MIN} max={SLIDER_MAX} step="1"
                           value={state.currentUs} disabled={state.busy}
                           on:input={(e) => jogServoTo(numValue(e))} />
                </div>

                <!-- ±µs jog buttons + capture actions -->
                <div class="jog-buttons">
                    {#each JOG_BUTTONS as d}
                        <button class="small jog-btn"
                                class:neg={d < 0} class:pos={d > 0}
                                on:click={() => jogServo(d)}
                                disabled={state.busy}>
                            {d > 0 ? '+' : ''}{d}
                        </button>
                    {/each}
                </div>

                <div class="capture-row">
                    <button class="small" on:click={captureAsMin}      disabled={state.busy}
                            title="Use the current jog position as the new minimum">⤓ Set as min</button>
                    <button class="small" on:click={captureAsCenter}   disabled={state.busy}
                            title="Use the current jog position as the new center / failsafe">◼ Set as center</button>
                    <button class="small" on:click={captureAsMax}      disabled={state.busy}
                            title="Use the current jog position as the new maximum">⤒ Set as max</button>
                </div>

                <!-- ─── Limits & motion profile ─────────────────── -->
                <div class="section-head">
                    Limits &amp; motion profile
                    <span class="hint">jog to the end-stop, then ⤓/⤒ capture it — jogging alone never changes the limits</span>
                </div>

                <div class="form-grid cols-3">
                    <div class="form-field">
                        <span class="field-label">Min µs</span>
                        <input class="field-input narrow" type="number" min={SLIDER_MIN} max={SLIDER_MAX} step="1"
                               value={state.draft.minUs}
                               on:change={(e) => setDraftField('minUs', numValue(e))} />
                    </div>
                    <div class="form-field">
                        <span class="field-label">Center µs</span>
                        <input class="field-input narrow" type="number" min={SLIDER_MIN} max={SLIDER_MAX} step="1"
                               value={state.draft.centerUs}
                               on:change={(e) => setDraftField('centerUs', numValue(e))} />
                    </div>
                    <div class="form-field">
                        <span class="field-label">Max µs</span>
                        <input class="field-input narrow" type="number" min={SLIDER_MIN} max={SLIDER_MAX} step="1"
                               value={state.draft.maxUs}
                               on:change={(e) => setDraftField('maxUs', numValue(e))} />
                    </div>

                    <div class="form-field">
                        <span class="field-label" title="Max slew rate; 0 = unlimited (snap to target)">Speed µs/s</span>
                        <input class="field-input narrow" type="number" min="0" max="20000" step="100"
                               value={state.draft.maxSpeedUsPerSec}
                               on:change={(e) => setDraftField('maxSpeedUsPerSec', numValue(e))} />
                    </div>
                    <div class="form-field">
                        <span class="field-label" title="Ramp accel / decel (symmetric); 0 = full speed instantly">Accel µs/s²</span>
                        <input class="field-input narrow" type="number" min="0" max="100000" step="100"
                               value={state.draft.maxAccelUsPerSec2}
                               on:change={(e) => setDraftField('maxAccelUsPerSec2', numValue(e))} />
                    </div>
                    <div class="form-field">
                        <span class="field-label" title="S-curve jerk; 0 = trapezoidal profile (most servos don't need this)">Jerk µs/s³</span>
                        <input class="field-input narrow" type="number" min="0" max="100000" step="100"
                               value={state.draft.maxJerkUsPerSec3}
                               on:change={(e) => setDraftField('maxJerkUsPerSec3', numValue(e))} />
                    </div>
                </div>

                <div class="form-row">
                    <label class="rev-toggle" title="Swap the role's open/close direction — applies after Save.">
                        <input type="checkbox" checked={state.draft.reversed}
                               on:change={(e) => setDraftField('reversed', checkValue(e))} />
                        ↔ Reversed (mirror around center)
                    </label>
                    <span class="hint">open / deploy drives the {state.draft.reversed ? 'MIN' : 'MAX'} µs end after Save — flip this if the mechanism moves the wrong way</span>
                </div>

                {#if rangeError}
                    <div class="row-err">⚠ Min must be less than Max (currently {state.draft.minUs} ≥ {state.draft.maxUs}).</div>
                {/if}
                {#if centerOOB}
                    <div class="row-warn">⚐ Center {state.draft.centerUs} µs is outside [{state.draft.minUs}, {state.draft.maxUs}] — the role will clamp it.</div>
                {/if}

                <!-- ─── Summary ─────────────────────────────────── -->
                <div class="summary">
                    <span class="sum-line"><strong>Travel:</strong> {state.draft.maxUs - state.draft.minUs} µs ({travelMs > 0 ? `~${travelMs} ms` : 'instant'} at the chosen speed)</span>
                    <span class="sum-line"><strong>Origin:</strong> {state.origin.minUs}–{state.origin.maxUs} µs · {state.origin.maxSpeedUsPerSec} µs/s{state.origin.reversed ? ' · rev' : ''}</span>
                </div>
            </div>

            <div class="modal-footer">
                <span class="dirty-flag" class:on={dirty} class:err={rangeError}>
                    {rangeError ? 'resolve range error above' : dirty ? 'unapplied changes — Save to persist' : 'in sync with origin'}
                </span>
                <button class="small" on:click={cancelServoCalibration} disabled={state.busy}
                        title="Restore the original profile and close">Cancel</button>
                <button class="small primary" on:click={saveServoCalibration}
                        disabled={state.busy || rangeError}
                        title={rangeError ? 'Fix the range error first'
                             : dirty ? 'Persist the new profile to the device (/hubfx.yaml) immediately'
                             : 'The draft equals the saved profile — jogging alone does not change limits; use ⤓/⤒ Set as min/max or the numeric fields'}>
                    {dirty ? 'Save' : 'Save (no changes)'}
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
    .cal-modal {
        /* Solid bg-surface — `var(--bg)` was undefined, so the modal
           appeared semi-transparent over the backdrop. */
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        width: min(96vw, 720px);
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
    .dirty-flag { font-size: 11px; color: var(--text-dim); margin-right: auto; }
    .dirty-flag.on  { color: var(--warning); font-weight: 600; }
    .dirty-flag.err { color: var(--error);   font-weight: 600; }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    .section-head .hint { font-size: 9px; font-weight: 400; text-transform: none; letter-spacing: 0; color: var(--text-dim); font-style: italic; }

    /* Big live readout — operator's anchor while jogging. */
    .jog-readout { display: flex; align-items: baseline; gap: 8px; margin: 4px 0 6px; }
    .jog-current { font-family: var(--font-mono); font-size: 24px; font-weight: 700; color: var(--text-bright); }
    .jog-of      { font-size: 11px; color: var(--text-dim); }

    /* Slider with draft-range underlay + center marker. */
    .slider-wrap { position: relative; height: 36px; margin: 6px 0 8px; }
    .slider-track { position: absolute; left: 0; right: 0; top: 14px; height: 8px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px; overflow: hidden; }
    .slider-range { position: absolute; top: 0; bottom: 0; background: color-mix(in srgb, var(--accent) 35%, transparent); border-left: 1px solid color-mix(in srgb, var(--accent) 70%, transparent); border-right: 1px solid color-mix(in srgb, var(--accent) 70%, transparent); }
    .slider-mark { position: absolute; top: -3px; bottom: -3px; width: 2px; transform: translateX(-1px); }
    .slider-mark.mark-center { background: var(--warning); }
    .slider { position: absolute; left: 0; right: 0; top: 0; width: 100%; height: 36px; background: transparent; appearance: none; cursor: pointer; }
    .slider::-webkit-slider-runnable-track { height: 36px; background: transparent; }
    .slider::-moz-range-track             { height: 36px; background: transparent; border: 0; }
    .slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 16px; height: 28px; border-radius: 3px; background: var(--text-bright); border: 1px solid var(--border); cursor: grab; }
    .slider::-moz-range-thumb     { width: 16px; height: 28px; border-radius: 3px; background: var(--text-bright); border: 1px solid var(--border); cursor: grab; }

    /* Jog button row — symmetric ± grid; negative deltas tint warning,
       positives tint success so the operator clocks the direction without
       reading the number. */
    .jog-buttons { display: grid; grid-template-columns: repeat(8, 1fr); gap: 4px; margin: 4px 0 6px; }
    .jog-btn { font-family: var(--font-mono); font-weight: 600; }
    .jog-btn.neg { color: var(--warning); }
    .jog-btn.pos { color: var(--success); }

    .capture-row { display: flex; gap: 8px; margin: 6px 0 12px; flex-wrap: wrap; }

    /* Inline form grid for the numeric profile fields. */
    .form-grid.cols-3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px 12px; margin-bottom: 8px; }
    .form-field { display: flex; flex-direction: column; gap: 2px; }
    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .field-input.narrow { width: 100%; }

    .rev-toggle { display: inline-flex; align-items: center; gap: 4px; font-size: 11px; color: var(--text); cursor: pointer; }
    .rev-toggle input { margin: 0; }

    .row-err  { font-size: 11px; color: var(--error);   margin: 4px 0 0; }
    .row-warn { font-size: 11px; color: var(--warning); margin: 4px 0 0; }

    .summary { display: flex; flex-direction: column; gap: 2px; margin-top: 12px; padding: 6px 8px; background: var(--bg-raised); border-radius: 4px; font-size: 11px; }
    .sum-line { color: var(--text-dim); }
    .sum-line strong { color: var(--text); font-weight: 600; }
</style>
