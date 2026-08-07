<!-- MotorCalibrationDialog — live drive + calibration for one gear motor
     (BiDcMotor / H-bridge).  The H-bridge analogue of ServoCalibrationDialog,
     opened from the GearPanel motor card via `openMotorCalibrationFor(...)`.

     A gear motor has no position feedback, so calibration is about three
     things, top to bottom:
       1. LIVE STATUS — duty / voltage / CURRENT mA / position / stall / guard,
          polled ~2 Hz while open (the mA is the operator's stall-current anchor).
       2. DRIVE — a seek duty + timeout, then Calibrate (sweep A→B, measuring
          travel-time + peak current per leg), To End A / B, Stop, hold-jog.
       3. STALL GUARD — LiveRatio (auto-baseline, recommended) or Fixed (mA
          threshold) that decides when the motor has hit the endstop.

     Save to strut writes the working duty (deploy +, retract −) and the
     suggested travel timeout (full-stroke travel × 1.5) back into the gear
     channel draft; the stall guard is a live push only (not a YAML field). -->
<script lang="ts">
    import { onDestroy } from 'svelte'
    import {
        openMotorCalibration,
        closeMotorCalibration, saveMotorToStrut,
        motorCalibrate, motorMoveEnd, motorStop,
        motorJogStart, motorJogStop, motorApplyGuard,
        setMotorField, refreshMotorStatus, suggestedTimeoutMs,
    } from '../motor_calibration'
    import { motorStatus, motorStatusKey } from '../motor_status'

    $: state = $openMotorCalibration
    $: live  = state ? $motorStatus[motorStatusKey(state.guid, state.portIdx)] ?? null : null

    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }

    // Poll the role ~2 Hz while the dialog is open so the mA / duty / stall
    // readout tracks during a move (the diagnostics-tab cadence).
    let poll: ReturnType<typeof setInterval> | undefined
    $: if (state && !poll) {
        poll = setInterval(() => { refreshMotorStatus().catch(() => {}) }, 500)
    } else if (!state && poll) {
        clearInterval(poll); poll = undefined
    }
    onDestroy(() => { if (poll) clearInterval(poll) })

    function onKeydown(e: KeyboardEvent) {
        if (!state || state.busy) return
        if (e.key === 'Escape') closeMotorCalibration()
    }

    function icon(o: string) { return o === 'reached' ? '✓' : o === 'timeout' ? '⏱' : '✕' }
    function fmtMs(ms: number) { return ms >= 1000 ? (ms / 1000).toFixed(2) + ' s' : ms + ' ms' }

    $: suggested = state ? suggestedTimeoutMs(state) : 0
</script>

<svelte:window on:keydown={onKeydown} />

{#if state}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="modal-backdrop" on:click|self={() => !state.busy && closeMotorCalibration()}>
        <div class="modal cal-modal">
            <div class="modal-header">
                <h2>Calibrate gear motor</h2>
                <span class="path">{state.label}</span>
                <button class="close-btn" on:click={closeMotorCalibration}
                        disabled={state.busy} title="Close (Esc) — brakes the motor">✕</button>
            </div>

            {#if state.error}<div class="banner err">{state.error}</div>{/if}
            {#if state.busy}<div class="banner note">Talking to the motor…</div>{/if}

            <div class="modal-body">

                <!-- ─── Live status (mA front and centre) ───────────── -->
                <div class="section-head">Live status<span class="hint">polled ~2 Hz while open</span></div>
                <div class="status-grid">
                    <div class="sv"><span class="sk">current</span>
                        <b class="big" class:warn={live?.stalled}>{live ? live.currentMa + ' mA' : '—'}</b></div>
                    <div class="sv"><span class="sk">duty</span><b>{live?.signedDuty ?? '—'}</b></div>
                    <div class="sv"><span class="sk">voltage</span><b>{live ? (live.voltageMv / 1000).toFixed(2) + ' V' : '—'}</b></div>
                    <div class="sv"><span class="sk">position</span><b>{live?.positionName ?? '—'}</b></div>
                    <div class="sv"><span class="sk">stall</span>
                        {#if live}<span class="dot" class:on={live.stalled}></span><b class:warn={live.stalled}>{live.stalled ? 'STALL' : 'clear'}</b>{:else}<b>—</b>{/if}</div>
                    <div class="sv"><span class="sk">guard</span><b>{live?.guardName ?? '—'}</b></div>
                    <div class="sv" title="Rated-voltage duty cap: |duty| ceiling = maxDuty × motor V / rail V">
                        <span class="sk">V-cap</span>
                        <b>{live ? (live.elementMv > 0 ? `±${live.capDuty} @ ${(live.railNowMv / 1000).toFixed(1)} V` : 'off') : '—'}</b></div>
                </div>

                <!-- ─── Drive ───────────────────────────────────────── -->
                <div class="section-head">Drive<span class="hint">port-native duty (same scale as the strut's deploy duty)</span></div>
                <div class="form-grid cols-3">
                    <div class="form-field">
                        <span class="field-label" title="Seek / jog duty magnitude (port-native, 0..32767)">Duty</span>
                        <input class="field-input narrow" type="number" min="0" max="32767" step="500"
                               value={state.duty} disabled={state.busy}
                               on:change={(e) => setMotorField('duty', Math.round(numValue(e)))} />
                    </div>
                    <div class="form-field">
                        <span class="field-label" title="Per-leg seek timeout — the seek aborts to TIMEOUT after this">Timeout (s)</span>
                        <input class="field-input narrow" type="number" min="1" max="60" step="1"
                               value={state.timeoutS} disabled={state.busy}
                               on:change={(e) => setMotorField('timeoutS', Math.round(numValue(e)))} />
                    </div>
                    <div class="form-field">
                        <span class="field-label" title="Motor rated voltage — caps the effective duty at maxDuty × this / supply rail (live per-motor voltage sense when wired), so a 6 V motor survives a 2S–6S pack. 0 = no cap (raw duty).">Motor V</span>
                        <input class="field-input narrow" type="number" min="0" max="48" step="0.5"
                               value={state.motorVoltageMv / 1000} disabled={state.busy}
                               on:change={(e) => setMotorField('motorVoltageMv', Math.max(0, Math.round(numValue(e) * 1000)))} />
                    </div>
                </div>

                <div class="toolbar">
                    <button class="small accent wide" on:click={motorCalibrate}
                            disabled={!!state.rowBusy || state.busy}
                            title="Sweep to end A then end B, measuring travel-time + peak current each leg">
                        ⟲ {state.rowBusy === 'calibrating' ? 'Calibrating…' : 'Calibrate (A→B sweep)'}
                    </button>
                    <button class="small" on:click={() => motorMoveEnd('a')} disabled={!!state.rowBusy || state.busy}
                            title="Drive to logical endstop A (+duty) until stall or timeout">⏮ To End A</button>
                    <button class="small" on:click={() => motorMoveEnd('b')} disabled={!!state.rowBusy || state.busy}
                            title="Drive to logical endstop B (−duty) until stall or timeout">To End B ⏭</button>
                    <button class="small danger" on:click={motorStop}
                            title="Hard-brake (aborts an in-flight seek) — always available">■ Stop</button>
                </div>

                <div class="toolbar">
                    <span class="jog-label">Manual jog (hold — raw, no stall guard):</span>
                    <button class="small jog" on:mousedown={() => motorJogStart(-1)}
                            on:mouseup={motorJogStop} on:mouseleave={motorJogStop}
                            disabled={!!state.rowBusy || state.busy} title="Hold to drive negative">«</button>
                    <button class="small jog" on:mousedown={() => motorJogStart(+1)}
                            on:mouseup={motorJogStop} on:mouseleave={motorJogStop}
                            disabled={!!state.rowBusy || state.busy} title="Hold to drive positive">»</button>
                    {#if state.rowBusy}<span class="seeking">● {state.rowBusy}…</span>{/if}
                </div>

                <!-- last move / calibration outcome -->
                {#if state.calib}
                    <div class="outcome">
                        <span class="ok-tag">calibration sweep</span>
                        <span class="leg">A: {icon(state.calib.legA.outcomeName)} {state.calib.legA.outcomeName} · {fmtMs(state.calib.legA.travelMs)} · peak {state.calib.legA.peakMa} mA</span>
                        <span class="leg">B: {icon(state.calib.legB.outcomeName)} {state.calib.legB.outcomeName} · <b>full stroke {fmtMs(state.calib.legB.travelMs)}</b> · peak {state.calib.legB.peakMa} mA</span>
                    </div>
                {:else if state.move}
                    <div class="outcome" class:bad={state.move.outcomeName !== 'reached'}>
                        {icon(state.move.outcomeName)} <b>{state.move.outcomeName}</b>{state.move.end ? ' → end ' + state.move.end.toUpperCase() : ''} · {fmtMs(state.move.travelMs)} · peak {state.move.peakMa} mA · pos {state.move.positionName}
                    </div>
                {/if}

                <!-- ─── Stall guard ─────────────────────────────────── -->
                <div class="section-head">Stall guard<span class="hint">how a seek decides it reached the endstop · live push (not persisted)</span></div>
                <div class="form-row">
                    <span class="field-label">Mode</span>
                    <div class="seg-select">
                        <button class="seg" class:on={state.guardMode === 'live'}
                                on:click={() => setMotorField('guardMode', 'live')} disabled={state.busy}
                                title="LiveRatio — averages running current per stroke, trips on a ratio spike. No per-motor threshold; battery-voltage independent.">LiveRatio</button>
                        <button class="seg" class:on={state.guardMode === 'fixed'}
                                on:click={() => setMotorField('guardMode', 'fixed')} disabled={state.busy}
                                title="Fixed — trips when |current| exceeds a known mA threshold sustained for the confirm window.">Fixed</button>
                    </div>
                </div>
                {#if state.guardMode === 'live'}
                    <div class="form-grid cols-3">
                        <div class="form-field">
                            <span class="field-label" title="Trip at ratio× the trailing-minimum running current">Ratio ×</span>
                            <input class="field-input narrow" type="number" min="1.2" max="6" step="0.1"
                                   value={state.ratio} on:change={(e) => setMotorField('ratio', numValue(e))} disabled={state.busy} />
                        </div>
                        <div class="form-field">
                            <span class="field-label" title="Baseline-sampling window after inrush">Sample ms</span>
                            <input class="field-input narrow" type="number" min="50" max="1000" step="50"
                                   value={state.sample} on:change={(e) => setMotorField('sample', numValue(e))} disabled={state.busy} />
                        </div>
                        <div class="form-field">
                            <span class="field-label" title="Absolute over-current backstop — trip regardless of ratio. ~80% of the stall peak. 0 = off.">Ceiling mA</span>
                            <input class="field-input narrow" type="number" min="0" max="9000" step="50"
                                   value={state.ceiling} on:change={(e) => setMotorField('ceiling', numValue(e))} disabled={state.busy} />
                        </div>
                    </div>
                {:else}
                    <div class="form-grid cols-2">
                        <div class="form-field">
                            <span class="field-label" title="Trip when |current| stays above this">Threshold mA</span>
                            <input class="field-input narrow" type="number" min="50" max="9000" step="50"
                                   value={state.threshold} on:change={(e) => setMotorField('threshold', numValue(e))} disabled={state.busy} />
                        </div>
                    </div>
                {/if}
                <div class="form-row">
                    <span class="field-label" title="Sustained-over-threshold confirm filter (both modes)">Confirm ms</span>
                    <input class="field-input narrow" type="number" min="20" max="500" step="10"
                           value={state.window} on:change={(e) => setMotorField('window', numValue(e))} disabled={state.busy} />
                    <button class="small" on:click={motorApplyGuard} disabled={state.busy}
                            title="Push this stall-guard config to the role (live, no re-attach)">✓ Apply guard</button>
                </div>
                <p class="hint compact">LiveRatio tracks the trailing-minimum running current (a loaded start can't poison it) and trips at ratio× that floor; Ceiling is an absolute backstop. Watch the live current above while you sweep.</p>
            </div>

            <div class="modal-footer">
                <span class="commit-hint">
                    Save writes deploy +{Math.abs(state.duty)} / retract −{Math.abs(state.duty)} duty{suggested > 0 ? ` · timeout ${suggested} ms` : ''} to the strut
                </span>
                <button class="small" on:click={closeMotorCalibration} disabled={state.busy}
                        title="Brake the motor and close without changing the strut">Close</button>
                <button class="small primary" on:click={saveMotorToStrut} disabled={state.busy}
                        title="Apply the working duty + travel timeout to the strut (persisted on Apply)">Save to strut</button>
            </div>
        </div>
    </div>
{/if}

<style>
    .modal-backdrop { position: fixed; inset: 0; background: rgba(0,0,0,0.65); display: flex; align-items: center; justify-content: center; z-index: 1000; }
    .cal-modal { background: var(--bg-surface); border: 1px solid var(--border); border-radius: 6px; width: min(96vw, 660px); max-height: 94vh; display: flex; flex-direction: column; box-shadow: 0 10px 40px rgba(0,0,0,0.5); }
    .modal-header { display: flex; align-items: center; gap: 12px; padding: 10px 14px; border-bottom: 1px solid var(--border); }
    .modal-header h2 { margin: 0; font-size: 15px; font-weight: 600; color: var(--text-bright); }
    .modal-header .path { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); flex: 1; }
    .close-btn { background: transparent; border: 0; color: var(--text-dim); cursor: pointer; font-size: 18px; padding: 0 4px; }
    .close-btn:hover { color: var(--text-bright); }

    .modal-body { flex: 1; min-height: 0; overflow: auto; padding: 12px 14px; }
    .modal-footer { display: flex; align-items: center; gap: 8px; justify-content: flex-end; padding: 8px 14px; border-top: 1px solid var(--border); }
    .commit-hint { font-size: 11px; color: var(--text-dim); margin-right: auto; font-family: var(--font-mono); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    .section-head .hint { font-size: 9px; font-weight: 400; text-transform: none; letter-spacing: 0; color: var(--text-dim); font-style: italic; }

    /* Verbose live-status grid — current first + emphasised. */
    .status-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px 14px; margin: 6px 0; padding: 10px 12px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px; }
    .sv { display: flex; align-items: baseline; gap: 6px; font-size: 12px; }
    .sv .sk { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); min-width: 52px; }
    /* Harmonised + compact metric readout: every value shares ONE mono
       size/weight so the grid reads as an even table (current was an 18px
       outlier that skewed the baseline + dwarfed the others). Current keeps a
       touch more weight + the stall-warn colour for emphasis, NOT a bigger
       font. */
    .sv b { font-family: var(--font-mono); font-size: 12px; font-weight: 600; color: var(--text-bright); }
    .sv b.big { font-size: 12px; font-weight: 700; }
    .sv b.warn, .warn { color: var(--error); }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--success); display: inline-block; }
    .dot.on { background: var(--error); box-shadow: 0 0 4px var(--error); }

    .form-grid.cols-2 { display: grid; grid-template-columns: repeat(2, 1fr); gap: 6px 12px; margin-bottom: 6px; }
    .form-grid.cols-3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px 12px; margin-bottom: 6px; }
    .form-field { display: flex; flex-direction: column; gap: 2px; }
    .form-field .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }
    .field-input.narrow { width: 100%; }
    .form-row { display: flex; align-items: center; gap: 8px; margin: 6px 0; }
    .form-row .field-label { font-size: 11px; color: var(--text-dim); min-width: 64px; }

    .toolbar { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; margin: 8px 0; }
    .toolbar .accent { border-color: var(--accent); color: var(--accent); }
    .toolbar .wide { min-width: 150px; }
    .toolbar .jog { min-width: 44px; font-size: 15px; font-weight: 700; }
    .jog-label { font-size: 11px; color: var(--text-dim); }
    .seeking { font-size: 11px; color: var(--warning); font-weight: 600; margin-left: 6px; }

    .outcome { font-size: 11px; font-family: var(--font-mono); color: var(--success); display: flex; flex-direction: column; gap: 2px; padding: 6px 8px; border-left: 2px solid var(--success); background: rgba(0,0,0,0.12); border-radius: 0 4px 4px 0; margin: 6px 0; }
    .outcome.bad { color: var(--warning); border-left-color: var(--warning); }
    .outcome .ok-tag { font-size: 9px; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); }
    .outcome .leg { color: var(--text); }

    .seg-select { display: inline-flex; border: 1px solid var(--border); border-radius: 4px; overflow: hidden; }
    .seg-select .seg { background: var(--bg-input); color: var(--text-dim); border: 0; border-right: 1px solid var(--border); padding: 4px 12px; font-size: 11px; cursor: pointer; }
    .seg-select .seg:last-child { border-right: 0; }
    .seg-select .seg.on { background: var(--accent); color: #fff; font-weight: 600; }

    .hint.compact { font-size: 10px; color: var(--text-dim); }
    .banner.err  { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); padding: 6px 10px; border-radius: 4px; font-size: 12px; margin: 8px 0; }
    .banner.note { background: var(--bg-input); border: 1px solid var(--border); color: var(--text-dim); padding: 6px 10px; border-radius: 4px; font-size: 12px; margin: 8px 0; }
</style>
