<!-- ScaleFX Studio — GearControl Diagnostics Tab -->
<!--
  Low-level role bench-test surface for a directly-connected generic expander
  (gated to controllerType === 'gearcontrol').  Drives the board's role layer
  with NO hub: attach / detach roles on ports, then drive + inspect them —
  servo travel, gear-motor (BiDcMotor) seek/endstop with settable timeout,
  one-button calibration sweep, and verbose live status (duty / voltage /
  stall current / position) plus the awaited move outcome (reached/timeout +
  travel time + peak current).  Backed by the App.Diag* Wails bindings.
-->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { connectionInfo } from '../stores'
    import {
        DiagInit, DiagRoleList, DiagRoleAttach, DiagRoleDetach,
        DiagBiMotorStatus, DiagBiMotorMoveEnd, DiagBiMotorCalibrate,
        DiagBiMotorStop, DiagBiMotorJog,
        DiagBiMotorGuardLiveRatio, DiagBiMotorGuardFixed,
        DiagServoProfileGet, DiagServoSetTarget, SendCommand,
    } from '../../../wailsjs/go/main/App'
    import { diag } from '../diag'

    // Wire constants — mirror protocol/ports (PortKind) + protocol/roles (RoleKind).
    const PORT_KINDS = [
        { v: 1, label: 'servo' },
        { v: 3, label: 'hbridge' },
    ]
    const ROLE_KINDS = [
        { v: 0x01, label: 'servo-actuator', ports: [1] },
        { v: 0x20, label: 'bi-dc-motor',    ports: [3] },
    ]
    const KIND_SERVO = 0x01
    const KIND_BIMOTOR = 0x20

    let roleRows: any[] = []
    let err = ''
    let busyGlobal = false

    // ── Attach form ──────────────────────────────────────────────────
    let aPortKind = 3
    let aPortIdx = 0
    let aRoleKind = KIND_BIMOTOR
    $: roleOpts = ROLE_KINDS.filter(r => r.ports.includes(aPortKind))
    $: if (roleOpts.length && !roleOpts.some(r => r.v === aRoleKind)) aRoleKind = roleOpts[0].v

    function fail(e: any) { err = e?.message || String(e); diag.warn('GEARDIAG', err) }

    async function refresh() {
        err = ''
        try { roleRows = await DiagRoleList() } catch (e) { fail(e); roleRows = [] }
    }
    async function doInit() {
        busyGlobal = true
        try { await DiagInit(); await refresh() } catch (e) { fail(e) } finally { busyGlobal = false }
    }
    async function attach() {
        busyGlobal = true
        try {
            await DiagRoleAttach(aPortKind, aPortIdx, aRoleKind)
            // A fresh BiDcMotor defaults to Fixed 2000 mA — too high for most
            // gear motors, so the seek never trips. Default it to LiveRatio
            // (auto-baseline + ratio spike) so "go to endstop" works out of box.
            if (aRoleKind === KIND_BIMOTOR) {
                guardMode[aPortIdx] = 'live'
                try { await DiagBiMotorGuardLiveRatio(aPortIdx, 250, 200, 150, 80, 0, 0) } catch (e) { fail(e) }
            }
            await refresh()
        } catch (e) { fail(e) } finally { busyGlobal = false }
    }
    async function detach(pk: number, pi: number) {
        busyGlobal = true
        try { await DiagRoleDetach(pk, pi); await refresh() } catch (e) { fail(e) } finally { busyGlobal = false }
    }

    // ── BiDcMotor: per-port drive params, live status, last outcome ──
    // (keyed by port idx so each row keeps its own settings)
    let bim: Record<number, any> = {}          // live status
    let outcome: Record<number, any> = {}       // last move/calibrate result
    let timeoutS: Record<number, number> = {}   // seek timeout, seconds (default 20)
    let duty: Record<number, number> = {}       // seek duty (default 600)
    let rowBusy: Record<number, string> = {}    // '' | 'moving' | 'calibrating'

    function tmo(idx: number) { return Math.round((timeoutS[idx] ?? 20) * 1000) }
    function dut(idx: number) { return Math.round(duty[idx] ?? 600) }

    // Stall-guard config (keyed by port idx). LiveRatio is the recommended
    // default — it averages running current per stroke then trips on a spike,
    // so it needs no per-motor threshold and is battery-voltage independent.
    let guardMode: Record<number, string> = {}   // 'live' | 'fixed'
    let gRatio: Record<number, number> = {}        // × (LiveRatio), default 2.5
    let gSample: Record<number, number> = {}       // ms baseline window (200)
    let gWindow: Record<number, number> = {}       // ms sustained confirm (80)
    let gThreshold: Record<number, number> = {}    // mA (Fixed), default 1000
    let gCeiling: Record<number, number> = {}      // mA absolute backstop (LiveRatio), 0 = off
    const gv = (rec: Record<number, number>, idx: number, def: number) => rec[idx] ?? def

    async function applyGuard(idx: number) {
        err = ''
        try {
            if ((guardMode[idx] ?? 'live') === 'live') {
                await DiagBiMotorGuardLiveRatio(idx, Math.round(gv(gRatio, idx, 2.5) * 100),
                    Math.round(gv(gSample, idx, 200)), 150, Math.round(gv(gWindow, idx, 80)), 0,
                    Math.round(gv(gCeiling, idx, 0)))
            } else {
                await DiagBiMotorGuardFixed(idx, Math.round(gv(gThreshold, idx, 1000)), Math.round(gv(gWindow, idx, 80)))
            }
        } catch (e) { fail(e) } finally { bimRefresh(idx) }
    }

    async function bimRefresh(idx: number) {
        try { bim[idx] = await DiagBiMotorStatus(idx); bim = bim } catch { /* transient */ }
    }
    async function moveEnd(idx: number, end: string) {
        rowBusy[idx] = 'moving'; rowBusy = rowBusy; err = ''
        try { outcome[idx] = { ...(await DiagBiMotorMoveEnd(idx, end, dut(idx), tmo(idx))), end }; outcome = outcome }
        catch (e) { fail(e) } finally { rowBusy[idx] = ''; rowBusy = rowBusy; bimRefresh(idx) }
    }
    async function calibrate(idx: number) {
        rowBusy[idx] = 'calibrating'; rowBusy = rowBusy; err = ''
        try { outcome[idx] = { calib: await DiagBiMotorCalibrate(idx, dut(idx), tmo(idx)) }; outcome = outcome }
        catch (e) { fail(e) } finally { rowBusy[idx] = ''; rowBusy = rowBusy; bimRefresh(idx) }
    }
    async function stop(idx: number) {
        try { await DiagBiMotorStop(idx) } catch (e) { fail(e) } finally { bimRefresh(idx) }
    }
    // Hold-to-jog (raw signed duty, NO stall guard) — press and hold.
    async function jogDown(idx: number, dir: number) {
        try { await DiagBiMotorJog(idx, dir * dut(idx)) } catch (e) { fail(e) }
    }
    async function jogUp(idx: number) {
        try { await DiagBiMotorJog(idx, 0) } catch (e) { fail(e) } finally { bimRefresh(idx) }
    }

    // ── Servo: profile + target (keyed by port idx) ──────────────────
    let servo: Record<number, any> = {}
    let servoUs: Record<number, number> = {}
    async function servoLoad(idx: number) {
        try {
            const p = await DiagServoProfileGet(idx)
            servo[idx] = p; servo = servo
            if (servoUs[idx] === undefined) { servoUs[idx] = p.centerUs; servoUs = servoUs }
        } catch (e) { fail(e) }
    }
    async function servoTarget(idx: number, us: number) {
        try { await DiagServoSetTarget(idx, Math.round(us)) } catch (e) { fail(e) }
    }

    // ── Raw command passthrough ──────────────────────────────────────
    let rawCmd = ''
    function runRaw() {
        const line = rawCmd.trim()
        if (!line) return
        SendCommand(line)
        rawCmd = ''
    }

    function fmtMs(ms: number) { return ms >= 1000 ? (ms / 1000).toFixed(2) + ' s' : ms + ' ms' }
    function outcomeIcon(o: string) { return o === 'reached' ? '✓' : o === 'timeout' ? '⏱' : '✕' }

    // Live poll: refresh BiDcMotor status for every attached bimotor (~500 ms
    // so the stall current updates smoothly during a move).
    let poll: any
    onMount(() => {
        refresh()
        poll = setInterval(() => {
            for (const r of roleRows) if (r.roleKind === KIND_BIMOTOR) bimRefresh(r.portIdx)
        }, 500)
    })
    onDestroy(() => clearInterval(poll))
</script>

<div class="gear-diag">
    <div class="card">
        <div class="card-header">
            <h3>GearControl Diagnostics</h3>
            <div class="header-actions">
                <span class="hint">{$connectionInfo.controllerName || 'expander'} · low-level role bench-test (no hub)</span>
                <button class="tbtn" on:click={doInit} disabled={busyGlobal} title="Activate the expander (INIT, slave mode) — required before attaching roles"><span class="ic">⏻</span> Init</button>
                <button class="tbtn" on:click={refresh} disabled={busyGlobal} title="Re-read the attached-role list"><span class="ic">⟳</span> Refresh</button>
            </div>
        </div>

        {#if err}<p class="hint err">⚠ {err}</p>{/if}

        <!-- Attach a role to a port -->
        <div class="section-head">Attach role</div>
        <p class="hint">Bind a role to a physical port, then drive it below. Roles normally attach from a hub; this is the direct path for bring-up.</p>
        <div class="attach-row">
            <label class="lbl">Port
                <select class="field-input narrow" bind:value={aPortKind}>
                    {#each PORT_KINDS as pk}<option value={pk.v}>{pk.label}</option>{/each}
                </select>
            </label>
            <label class="lbl">Index
                <input class="field-input narrow" type="number" min="0" max="11" bind:value={aPortIdx} />
            </label>
            <label class="lbl">Role
                <select class="field-input" bind:value={aRoleKind}>
                    {#each roleOpts as r}<option value={r.v}>{r.label}</option>{/each}
                </select>
            </label>
            <button class="tbtn primary" on:click={attach} disabled={busyGlobal}><span class="ic">＋</span> Attach</button>
        </div>

        <!-- Attached roles + per-role drive controls -->
        <div class="section-head">Attached roles ({roleRows.length})</div>
        {#if roleRows.length === 0}
            <p class="empty-state">No roles attached. Attach one above to test a servo or gear motor.</p>
        {:else}
            {#each roleRows as r (r.portKind + ':' + r.portIdx)}
                {@const idx = r.portIdx}
                <div class="role-card">
                    <div class="role-head">
                        <span class="role-id mono">{r.portKindName}[{idx}]</span>
                        <span class="role-kind">{r.roleKindName}</span>
                        <button class="tbtn danger" on:click={() => detach(r.portKind, idx)} disabled={busyGlobal} title="Detach this role"><span class="ic">✕</span> Detach</button>
                    </div>

                    {#if r.roleKind === KIND_BIMOTOR}
                        {@const st = bim[idx]}
                        {@const oc = outcome[idx]}
                        {@const moving = rowBusy[idx]}

                        <!-- params -->
                        <div class="params">
                            <label class="lbl">Timeout
                                <span class="inp-unit"><input class="field-input tiny" type="number" min="1" max="60" step="1" bind:value={timeoutS[idx]} placeholder="20" /><span class="unit">s</span></span>
                            </label>
                            <label class="lbl">Duty
                                <input class="field-input narrow" type="number" min="0" max="1000" step="50" bind:value={duty[idx]} placeholder="600" />
                            </label>
                        </div>

                        <!-- stall guard (how the seek decides it hit the endstop) -->
                        <div class="guard">
                            <label class="lbl">Stall guard
                                <select class="field-input guard-mode" bind:value={guardMode[idx]}>
                                    <option value="live">LiveRatio (auto baseline)</option>
                                    <option value="fixed">Fixed (mA threshold)</option>
                                </select>
                            </label>
                            {#if (guardMode[idx] ?? 'live') === 'live'}
                                <label class="lbl">Ratio
                                    <span class="inp-unit"><input class="field-input tiny" type="number" min="1.2" max="6" step="0.1" bind:value={gRatio[idx]} placeholder="2.5" /><span class="unit">×</span></span>
                                </label>
                                <label class="lbl">Sample
                                    <span class="inp-unit"><input class="field-input tiny" type="number" min="50" max="1000" step="50" bind:value={gSample[idx]} placeholder="200" /><span class="unit">ms</span></span>
                                </label>
                                <label class="lbl" title="Absolute over-current backstop — trip if current exceeds this regardless of the ratio (catches a stop the adaptive baseline missed). Set ~80% of the stall peak you see in the trace. 0 = off.">Ceiling
                                    <span class="inp-unit"><input class="field-input tiny" type="number" min="0" max="9000" step="50" bind:value={gCeiling[idx]} placeholder="0=off" /><span class="unit">mA</span></span>
                                </label>
                            {:else}
                                <label class="lbl">Threshold
                                    <span class="inp-unit"><input class="field-input narrow" type="number" min="50" max="9000" step="50" bind:value={gThreshold[idx]} placeholder="1000" /><span class="unit">mA</span></span>
                                </label>
                            {/if}
                            <label class="lbl">Confirm
                                <span class="inp-unit"><input class="field-input tiny" type="number" min="20" max="500" step="10" bind:value={gWindow[idx]} placeholder="80" /><span class="unit">ms</span></span>
                            </label>
                            <button class="tbtn" on:click={() => applyGuard(idx)} title="Push this stall-guard config to the role (live, no re-attach)"><span class="ic">✓</span> Apply guard</button>
                        </div>
                        <p class="hint compact">LiveRatio tracks the <b>trailing-minimum</b> running current (the free-running floor — a high break-away / loaded start can't poison it) and trips at ratio× that floor. <b>Ceiling</b> is an absolute over-current backstop: trips regardless of the ratio (catches a stop the adaptive baseline missed — set ~80% of the stall peak in the trace). Watch <b>floor</b>/<b>thr</b> in the Console (Ctrl+`).</p>

                        <!-- primary drive toolbar -->
                        <div class="toolbar">
                            <button class="tbtn wide accent" on:click={() => calibrate(idx)} disabled={!!moving} title="Sweep to end A then end B, measuring travel time + peak current each leg">
                                <span class="ic">⟲</span> {moving === 'calibrating' ? 'Calibrating…' : 'Calibrate'}
                            </button>
                            <button class="tbtn" on:click={() => moveEnd(idx, 'a')} disabled={!!moving} title="Drive to logical endstop A (+duty) until stall or timeout"><span class="ic">⏮</span> To End A</button>
                            <button class="tbtn" on:click={() => moveEnd(idx, 'b')} disabled={!!moving} title="Drive to logical endstop B (−duty) until stall or timeout">To End B <span class="ic">⏭</span></button>
                            <button class="tbtn danger" on:click={() => stop(idx)} title="Hard-brake (aborts an in-flight seek)"><span class="ic">■</span> Stop</button>
                        </div>

                        <!-- manual jog (raw, no stall guard) -->
                        <div class="toolbar">
                            <span class="jog-label">Manual jog (hold):</span>
                            <button class="tbtn jog" on:mousedown={() => jogDown(idx, -1)} on:mouseup={() => jogUp(idx)} on:mouseleave={() => jogUp(idx)} disabled={!!moving} title="Hold to drive negative (raw, no stall guard)"><span class="ic">«</span></button>
                            <button class="tbtn jog" on:mousedown={() => jogDown(idx, +1)} on:mouseup={() => jogUp(idx)} on:mouseleave={() => jogUp(idx)} disabled={!!moving} title="Hold to drive positive (raw, no stall guard)"><span class="ic">»</span></button>
                            {#if moving}<span class="seeking">● {moving}…</span>{/if}
                        </div>

                        <!-- verbose live status -->
                        <div class="status-grid">
                            <div class="sv"><span class="sk">duty</span><b>{st?.signedDuty ?? '—'}</b></div>
                            <div class="sv"><span class="sk">voltage</span><b>{st ? (st.voltageMv / 1000).toFixed(2) + ' V' : '—'}</b></div>
                            <div class="sv"><span class="sk">current</span><b class:warn={st?.stalled}>{st ? st.currentMa + ' mA' : '—'}</b></div>
                            <div class="sv"><span class="sk">position</span><b>{st?.positionName ?? '—'}</b></div>
                            <div class="sv"><span class="sk">stall</span>
                                {#if st}<span class="dot" class:on={st.stalled}></span><b class:warn={st.stalled}>{st.stalled ? 'STALL' : 'clear'}</b>{:else}<b>—</b>{/if}
                            </div>
                            <div class="sv"><span class="sk">guard</span><b>{st?.guardName ?? '—'}</b></div>
                        </div>

                        <!-- last move / calibration outcome -->
                        {#if oc?.calib}
                            <div class="outcome">
                                <span class="ok-tag">calibration</span>
                                <span class="leg">A: {outcomeIcon(oc.calib.legA.outcomeName)} {oc.calib.legA.outcomeName} · {fmtMs(oc.calib.legA.travelMs)} · peak {oc.calib.legA.peakMa} mA</span>
                                <span class="leg">B: {outcomeIcon(oc.calib.legB.outcomeName)} {oc.calib.legB.outcomeName} · <b>travel {fmtMs(oc.calib.legB.travelMs)}</b> · peak {oc.calib.legB.peakMa} mA</span>
                            </div>
                        {:else if oc}
                            <div class="outcome" class:bad={oc.outcomeName !== 'reached'}>
                                {outcomeIcon(oc.outcomeName)} <b>{oc.outcomeName}</b>{oc.end ? ' → end ' + oc.end.toUpperCase() : ''} · {fmtMs(oc.travelMs)} · peak {oc.peakMa} mA · pos {oc.positionName}
                            </div>
                        {/if}

                    {:else if r.roleKind === KIND_SERVO}
                        {@const p = servo[idx]}
                        <div class="toolbar">
                            <button class="tbtn" on:click={() => servoLoad(idx)} title="Read this servo's calibrated travel + profile"><span class="ic">⟳</span> Load profile</button>
                            {#if p}
                                <button class="tbtn" on:click={() => servoTarget(idx, p.minUs)} title="Drive to min"><span class="ic">⏮</span> Min</button>
                                <button class="tbtn" on:click={() => servoTarget(idx, p.centerUs)} title="Drive to center">Center</button>
                                <button class="tbtn" on:click={() => servoTarget(idx, p.maxUs)} title="Drive to max">Max <span class="ic">⏭</span></button>
                            {/if}
                        </div>
                        {#if p}
                            <div class="servo-slide">
                                <input class="slider" type="range" min={p.minUs} max={p.maxUs} step="1"
                                    bind:value={servoUs[idx]}
                                    on:input={() => servoTarget(idx, servoUs[idx])} />
                                <span class="live mono">{servoUs[idx]} µs <span class="text-dim">[{p.minUs}–{p.maxUs}{p.reversed ? ' REV' : ''}]</span></span>
                            </div>
                        {:else}
                            <p class="hint compact">Load profile to enable the travel slider.</p>
                        {/if}
                    {/if}
                </div>
            {/each}
        {/if}

        <!-- Raw command escape hatch -->
        <div class="section-head">Raw command</div>
        <p class="hint compact">Run any console command against this board (output appears in the Console drawer, Ctrl+`). e.g. <span class="mono">bimotor-status 0</span>, <span class="mono">servo-profile-set 0 max_us=1800</span>.</p>
        <div class="attach-row">
            <input class="field-input wide" placeholder="role-list-local" bind:value={rawCmd}
                on:keydown={(e) => e.key === 'Enter' && runRaw()} />
            <button class="tbtn" on:click={runRaw} disabled={!rawCmd.trim()}><span class="ic">▷</span> Run</button>
        </div>
    </div>
</div>

<style>
    /* Outer padding comes from the .tab-content wrapper (Rule 62, 12px) — only
       the content max-width is set here. */
    .gear-diag { max-width: 940px; }
    .lbl { display: flex; flex-direction: column; gap: 2px; font-size: 11px; color: var(--text-dim); }

    /* Toolbar buttons — one icon+label style, fixed metrics so rows align */
    .tbtn {
        display: inline-flex; align-items: center; justify-content: center; gap: 5px;
        height: 28px; padding: 0 10px; box-sizing: border-box;
        font-size: 11px; font-weight: 500; white-space: nowrap;
        background: var(--bg-input); color: var(--text);
        border: 1px solid var(--border); border-radius: 4px; cursor: pointer;
    }
    .tbtn:hover:not(:disabled) { border-color: var(--accent); color: var(--text-bright); }
    .tbtn:disabled { opacity: 0.45; cursor: default; }
    .tbtn .ic { font-size: 13px; line-height: 1; }
    .tbtn.primary { background: var(--accent); color: #fff; border-color: var(--accent); }
    .tbtn.accent { border-color: var(--accent); color: var(--accent); }
    .tbtn.danger:hover:not(:disabled) { border-color: var(--error); color: var(--error); }
    .tbtn.wide { min-width: 120px; }
    .tbtn.jog { min-width: 44px; font-size: 15px; }

    .attach-row, .params, .guard { display: flex; align-items: flex-end; gap: 10px; flex-wrap: wrap; margin: 6px 0; }
    .guard { padding: 6px 8px; background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px; }
    .field-input.guard-mode { min-width: 180px; }
    .inp-unit { display: inline-flex; align-items: center; gap: 3px; }
    .field-input.tiny { width: 52px; }
    .unit { font-size: 10px; font-family: var(--font-mono, monospace); color: var(--text-dim); }

    .role-card { border: 1px solid var(--border); border-radius: 6px; padding: 10px 12px; margin-bottom: 10px; background: var(--bg-elevated, rgba(255,255,255,0.02)); }
    .role-head { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }
    .role-id { font-weight: 600; color: var(--text-bright); font-size: 13px; }
    .role-kind { color: var(--accent); font-size: 12px; }
    .role-head .danger { margin-left: auto; }

    .toolbar { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; margin: 6px 0; }
    .jog-label { font-size: 11px; color: var(--text-dim); }
    .seeking { font-size: 11px; color: var(--warning); font-weight: 600; margin-left: 6px; }

    /* Verbose live-status grid — aligned key/value cells */
    .status-grid {
        display: grid; grid-template-columns: repeat(3, 1fr); gap: 4px 14px;
        margin: 8px 0; padding: 8px 10px;
        background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px;
    }
    .sv { display: flex; align-items: baseline; gap: 6px; font-size: 12px; }
    .sv .sk { font-size: 10px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); min-width: 56px; }
    .sv b { font-family: var(--font-mono, monospace); color: var(--text-bright); }
    .sv b.warn, .warn { color: var(--error); }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--success); display: inline-block; }
    .dot.on { background: var(--error); box-shadow: 0 0 4px var(--error); }

    .outcome { font-size: 11px; font-family: var(--font-mono, monospace); color: var(--success); display: flex; flex-direction: column; gap: 2px; padding: 6px 8px; border-left: 2px solid var(--success); background: rgba(0,0,0,0.12); border-radius: 0 4px 4px 0; }
    .outcome.bad { color: var(--warning); border-left-color: var(--warning); }
    .outcome .ok-tag { font-size: 9px; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); }
    .outcome .leg { color: var(--text); }

    .servo-slide { display: flex; align-items: center; gap: 10px; margin: 6px 0; }
    .slider { flex: 1; min-width: 140px; }
    .live { font-size: 11px; color: var(--text); }
    .mono { font-family: var(--font-mono, monospace); }
</style>
