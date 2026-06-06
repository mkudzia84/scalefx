<!-- ScaleFX Studio — GearControl Diagnostics Tab -->
<!--
  Low-level role bench-test surface for a directly-connected generic expander
  (gated to controllerType === 'gearcontrol').  Drives the board's role layer
  with NO hub: attach / detach roles on ports, then drive + inspect them —
  servo travel, gear-motor (BiDcMotor) seek/endstop, live stall current.  This
  is the GUI twin of the CLI role-attach-local / bimotor-* / servo-* commands.
  Backed by the App.Diag* Wails bindings (app_geardiag.go).
-->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { connectionInfo } from '../stores'
    import {
        DiagInit, DiagRoleList, DiagRoleAttach, DiagRoleDetach,
        DiagBiMotorStatus, DiagBiMotorSeek, DiagBiMotorMoveEnd,
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
        { v: 0x11, label: 'dc-motor',       ports: [2] },
        { v: 0x12, label: 'heater',         ports: [2] },
        { v: 0x10, label: 'led-animator',   ports: [2] },
    ]
    const KIND_SERVO = 0x01
    const KIND_BIMOTOR = 0x20

    let roleRows: any[] = []
    let err = ''
    let busy = false

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
        busy = true
        try { await DiagInit(); await refresh() } catch (e) { fail(e) } finally { busy = false }
    }
    async function attach() {
        busy = true
        try { await DiagRoleAttach(aPortKind, aPortIdx, aRoleKind); await refresh() }
        catch (e) { fail(e) } finally { busy = false }
    }
    async function detach(pk: number, pi: number) {
        busy = true
        try { await DiagRoleDetach(pk, pi); await refresh() } catch (e) { fail(e) } finally { busy = false }
    }

    // ── BiDcMotor drive + live status (keyed by port idx) ────────────
    let bim: Record<number, any> = {}
    async function bimRefresh(idx: number) {
        try { bim[idx] = await DiagBiMotorStatus(idx); bim = bim } catch (e) { /* transient */ }
    }
    async function bimSeek(idx: number, duty: number) {
        try { await DiagBiMotorSeek(idx, duty, 2000); bimRefresh(idx) } catch (e) { fail(e) }
    }
    async function bimMove(idx: number, end: string) {
        try { await DiagBiMotorMoveEnd(idx, end, 600, 3000); bimRefresh(idx) } catch (e) { fail(e) }
    }

    // ── Servo drive + profile (keyed by port idx) ────────────────────
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

    // ── Raw command passthrough (anything not covered above) ─────────
    let rawCmd = ''
    function runRaw() {
        const line = rawCmd.trim()
        if (!line) return
        SendCommand(line)          // output lands in the Console drawer (Ctrl+`)
        rawCmd = ''
    }

    // Live poll: refresh BiDcMotor status for every attached bimotor.
    let poll: any
    onMount(() => {
        refresh()
        poll = setInterval(() => {
            for (const r of roleRows) if (r.roleKind === KIND_BIMOTOR) bimRefresh(r.portIdx)
        }, 1000)
    })
    onDestroy(() => clearInterval(poll))
</script>

<div class="gear-diag">
    <div class="card">
        <div class="card-header">
            <h3>GearControl Diagnostics</h3>
            <div class="header-actions">
                <span class="hint">{$connectionInfo.controllerName || 'expander'} · low-level role bench-test (no hub)</span>
                <button class="small" on:click={doInit} disabled={busy} title="Activate the expander (INIT, slave mode) — required before attaching roles">Init</button>
                <button class="small" on:click={refresh} disabled={busy} title="Re-read the attached-role list">Refresh</button>
            </div>
        </div>

        {#if err}<p class="hint err">⚠ {err}</p>{/if}

        <!-- Attach a role to a port -->
        <div class="section-head">Attach role</div>
        <p class="hint">Bind a role to a physical port, then drive it below. Roles normally attach from a hub; this is the direct path for bring-up.</p>
        <div class="form-row">
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
            <button class="small primary" on:click={attach} disabled={busy}>Attach</button>
        </div>

        <!-- Attached roles + per-role drive controls -->
        <div class="section-head">Attached roles ({roleRows.length})</div>
        {#if roleRows.length === 0}
            <p class="empty-state">No roles attached. Attach one above to test a servo or gear motor.</p>
        {:else}
            {#each roleRows as r (r.portKind + ':' + r.portIdx)}
                <div class="role-row">
                    <div class="role-head">
                        <span class="role-id mono">{r.portKindName}[{r.portIdx}]</span>
                        <span class="role-kind">{r.roleKindName}</span>
                        <button class="small danger" on:click={() => detach(r.portKind, r.portIdx)} disabled={busy} title="Detach this role">Detach</button>
                    </div>

                    {#if r.roleKind === KIND_BIMOTOR}
                        {@const st = bim[r.portIdx]}
                        <div class="drive">
                            <button class="small" on:click={() => bimMove(r.portIdx, 'a')} title="Drive to logical endstop A (+duty)">→ End A</button>
                            <button class="small" on:click={() => bimMove(r.portIdx, 'b')} title="Drive to logical endstop B (−duty)">→ End B</button>
                            <button class="small" on:click={() => bimSeek(r.portIdx, 600)} title="Seek at +600 duty">Seek +</button>
                            <button class="small" on:click={() => bimSeek(r.portIdx, -600)} title="Seek at −600 duty">Seek −</button>
                            <button class="small" on:click={() => bimSeek(r.portIdx, 0)} title="Stop (duty 0)">Stop</button>
                        </div>
                        <div class="live mono">
                            {#if st}
                                duty <b>{st.signedDuty}</b> · {st.voltageMv} mV ·
                                <span class:warn={st.stalled}>{st.currentMa} mA{st.stalled ? ' STALL' : ''}</span> ·
                                pos <b>{st.positionName}</b> · guard {st.guardMode}
                            {:else}
                                <span class="text-dim">no status yet…</span>
                            {/if}
                        </div>
                    {:else if r.roleKind === KIND_SERVO}
                        {@const p = servo[r.portIdx]}
                        <div class="drive">
                            <button class="small" on:click={() => servoLoad(r.portIdx)} title="Read this servo's calibrated travel + profile">Load profile</button>
                            {#if p}
                                <input class="slider" type="range" min={p.minUs} max={p.maxUs} step="1"
                                    bind:value={servoUs[r.portIdx]}
                                    on:input={() => servoTarget(r.portIdx, servoUs[r.portIdx])} />
                                <span class="live mono">{servoUs[r.portIdx]} µs <span class="text-dim">[{p.minUs}–{p.maxUs}{p.reversed ? ' REV' : ''}]</span></span>
                                <button class="small" on:click={() => servoTarget(r.portIdx, p.minUs)} title="Drive to min">Min</button>
                                <button class="small" on:click={() => servoTarget(r.portIdx, p.centerUs)} title="Drive to center">Center</button>
                                <button class="small" on:click={() => servoTarget(r.portIdx, p.maxUs)} title="Drive to max">Max</button>
                            {:else}
                                <span class="text-dim">Load profile to enable the travel slider.</span>
                            {/if}
                        </div>
                    {/if}
                </div>
            {/each}
        {/if}

        <!-- Raw command escape hatch -->
        <div class="section-head">Raw command</div>
        <p class="hint compact">Run any console command against this board (output appears in the Console drawer, Ctrl+`). e.g. <span class="mono">bimotor-status 0</span>, <span class="mono">servo-profile-set 0 max_us=1800</span>.</p>
        <div class="form-row">
            <input class="field-input wide" placeholder="role-list-local" bind:value={rawCmd}
                on:keydown={(e) => e.key === 'Enter' && runRaw()} />
            <button class="small" on:click={runRaw} disabled={!rawCmd.trim()}>Run</button>
        </div>
    </div>
</div>

<style>
    .gear-diag { padding: 12px; max-width: 920px; }
    .lbl { display: flex; flex-direction: column; gap: 2px; font-size: 11px; color: var(--text-dim); }
    .role-row { border: 1px solid var(--border); border-radius: 6px; padding: 8px 10px; margin-bottom: 8px; background: var(--bg-elevated, rgba(255,255,255,0.02)); }
    .role-head { display: flex; align-items: center; gap: 10px; }
    .role-id { font-weight: 600; color: var(--text-bright); }
    .role-kind { color: var(--accent); font-size: 12px; }
    .role-head .danger { margin-left: auto; }
    .drive { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; margin-top: 6px; }
    .slider { flex: 1; min-width: 120px; }
    .live { font-size: 11px; color: var(--text); margin-top: 4px; }
    .live .warn, .warn { color: var(--error); font-weight: 600; }
    .mono { font-family: var(--font-mono, monospace); }
</style>
