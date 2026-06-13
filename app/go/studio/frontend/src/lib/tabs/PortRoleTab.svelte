<!-- ScaleFX Studio — Ports & Roles (right column of the Input & Ports tab).
     One card per board; each port shows its role (a picker limited to the
     roles that port kind can host, or a fixed "Servo" tag for servos) and
     an operator name.  Uses the shared design system (.card, .form-row,
     .field-input, button) so control heights line up. -->
<script lang="ts">
    import {
        deviceModel, attachRole, detachRole, markHubDirty,
        setPortName, portKindName, boardDisplayNames, claimsForPort,
        formatPortRail, removeAbandonedBoard, RoleKind,
        type Port, type PortRef,
    } from '../devicemodel'
    import { effectClaims } from '../effect-claims'
    import PortRoleConfig from '../components/PortRoleConfig.svelte'
    import { openServoCalibrationFor, defaultServoProfile } from '../servo_calibration'
    import { SetPortProfile } from '../../../wailsjs/go/main/App'

    // Servo settings (calibrate) + reset — narrow icon buttons on the servo row.
    // Both route by the port's real GUID (hub-local ports carry the hub GUID,
    // e.g. 6D60; the hub self-routes a topology forward to itself).  Reset writes
    // the default profile + marks /hubfx.yaml dirty so Apply persists it.
    function calibrateServo(p: Port): void {
        const prof = p.profile ?? defaultServoProfile()
        openServoCalibrationFor(
            p.ref.guid, p.ref.index,
            `${p.boardName} · ${p.name || p.hardwareName}`,
            prof, prof.centerUs,
        )
    }
    async function resetServo(p: Port): Promise<void> {
        busy = true; error = ''
        try {
            await SetPortProfile(p.ref.guid, /*ServoKind=*/1, p.ref.index, defaultServoProfile() as any)
            markHubDirty()
        } catch (e) { error = String(e) } finally { busy = false }
    }

    // Per-port "show inline role-config editor" toggle.  Open one at a
    // time keeps the visual list tractable; a small map keyed by port
    // ref preserves state across re-renders.
    const expanded = new Set<string>()
    let expandedTick = 0
    function portKey(p: Port): string { return `${p.ref.guid}|${p.ref.kind}|${p.ref.index}` }
    // `_tick` is the reactivity trigger only — the template passes `expandedTick`
    // so Svelte re-runs this when toggleExpand bumps it (the Set itself isn't
    // tracked).  The value is intentionally unused (was a `(tick, …)` comma
    // operator, which svelte-check flags as an unused expression).
    function isExpanded(p: Port, _tick: number = expandedTick): boolean { return expanded.has(portKey(p)) }
    function toggleExpand(p: Port): void {
        const k = portKey(p)
        if (expanded.has(k)) expanded.delete(k)
        else expanded.add(k)
        expandedTick++   // force Svelte reactivity
    }
    // Roles that have an inline ⚙ Tune editor (element scaling).  Servos are
    // NOT here — their calibrate affordance is rendered inline on the row via
    // ServoWidget, not behind the expander.
    function hasRoleConfig(p: Port): boolean {
        // Hub-local AND expander ports (Rule 58 — element tune routes by GUID).
        // Offline ghost ports have no live role to tune, so gate those out.
        if (p.offline) return false
        return p.roleKind === RoleKind.DcMotor
            || p.roleKind === RoleKind.Heater
    }
    function portKindForConfig(p: Port): 'servo' | 'pwm' | null {
        if (p.kindName === 'servo') return 'servo'
        if (p.kindName === 'pwm') return 'pwm'
        return null
    }

    let busy = false
    let error = ''

    // No on-mount refresh — App.svelte refreshes on connect and the store
    // drives this view, so there's no transient "not connected" flash.
    $: names = boardDisplayNames($deviceModel.ports)

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }

    async function onRole(p: PortRef, roleKind: number) {
        busy = true; error = ''
        try {
            if (roleKind === RoleKind.None) await detachRole(p)
            else await attachRole(p, roleKind)
        } catch (e) { error = String(e) } finally { busy = false }
    }

    async function onName(p: PortRef, name: string) {
        busy = true; error = ''
        try { await setPortName(p, name.trim()) } catch (e) { error = String(e) } finally { busy = false }
    }

    // Output ports only — input ports are configured in the left column
    // (InputPanel), so they're excluded here to avoid duplicating them.
    // Offline boards (configured-but-disconnected expanders) come through as
    // ghost ports flagged `offline`; they render as dimmed cards with a
    // warning + remove action.
    $: boards = groupByBoard($deviceModel.ports.filter(p => p.direction === 'output'))
    interface BoardGroup { guid: string; name: string; ports: Port[]; offline: boolean }
    function groupByBoard(ports: Port[]): BoardGroup[] {
        const by = new Map<string, BoardGroup>()
        for (const p of ports) {
            const g = by.get(p.ref.guid) ?? { guid: p.ref.guid, name: p.boardName, ports: [], offline: !!p.offline }
            g.ports.push(p)
            by.set(p.ref.guid, g)
        }
        return [...by.values()]
    }

    async function removeBoard(guid: string): Promise<void> {
        busy = true; error = ''
        try { await removeAbandonedBoard(guid) } catch (e) { error = String(e) } finally { busy = false }
    }

    function isServo(p: Port): boolean { return p.kindName === 'servo' }
    function fanout(p: Port): string {
        // $effectClaims (merged), not bare $deviceModel.claims — so a port an
        // effect uses shows its owner instead of reading "unclaimed".
        return claimsForPort($effectClaims, p.ref).map(c => `${c.domain}/${c.slot}`).join(', ')
    }
</script>

<div class="tab-content">
    <!-- Header (Diagram + Apply + Defaults + Refresh) lives in the
         parent IoTab so it spans both columns. -->
    {#if error}<div class="banner err">{error}</div>{/if}
    {#if boards.length === 0}<div class="empty-state">No output ports on the connected boards.</div>{/if}

    {#each boards as b (b.guid)}
        <div class="card board-card" class:offline={b.offline}>
            <div class="board-head">
                <span class="board-name">{names[b.guid] ?? b.name}</span>
                {#if b.guid}<span class="board-guid" title="Board GUID — distinguishes two boards of the same type">{b.guid}</span>{/if}
                {#if b.offline}
                    <span class="offline-badge" title="Configured in /hubfx.yaml but not connected">offline</span>
                    <button class="small danger remove-btn" on:click={() => removeBoard(b.guid)} disabled={busy}
                            title="Delete this board's entry from /hubfx.yaml (ports, roles, names). Apply to persist.">
                        🗑 Remove from config
                    </button>
                {/if}
            </div>
            {#if b.offline}
                <div class="offline-warn">
                    Configured but not connected — ports below are shown from the saved config. Reconnect the board to edit roles, or remove it.
                </div>
            {/if}
            <div class="port-list">
                {#each b.ports as p (p.kindName + p.ref.index)}
                    <div class="port-row">
                        <span class="port-id" title="{portKindName[p.ref.kind]} {p.ref.index}">{p.hardwareName}</span>
                        <span class="dir-badge {p.direction}">{p.direction}</span>
                        <span class="caps">{p.caps.filter(c => !c.startsWith('VOLTAGE_')).join(' ') || ''}</span>
                        {#if p.voltageMv}
                            <span class="rail-chip" title="Rail voltage declared by the board's port descriptor">{formatPortRail(p.voltageMv)}</span>
                        {/if}

                        {#if b.offline}
                            <!-- Offline board: role + name are read-only (no wire to
                                 push to until the board reconnects). -->
                            <span class="role-fixed" title="Saved role (board offline)">{p.roleName === 'none' ? '— none —' : p.roleName}</span>
                            <span class="name-ro" title="Saved name (board offline)">{p.name || '—'}</span>
                        {:else}
                            {#if isServo(p)}
                                <span class="role-fixed" title="Servo ports can only host a servo actuator">Servo</span>
                            {:else}
                                <select class="field-input role-select"
                                        value={p.roleKind}
                                        on:change={(e) => onRole(p.ref, Number(selValue(e)))}
                                        disabled={busy}
                                        title="Role this port performs">
                                    <option value={RoleKind.None}>— none —</option>
                                    {#each p.allowedRoles as r}
                                        <option value={r.kind}>{r.label}</option>
                                    {/each}
                                </select>
                            {/if}

                            <input class="field-input name-input" type="text" placeholder="name…"
                                   value={p.name} disabled={busy}
                                   on:change={(e) => onName(p.ref, inputValue(e))} />

                            {#if isServo(p)}
                                <!-- Narrow icon buttons — settings (calibrate) + reset —
                                     right next to the name box.  Routed by the port's
                                     real GUID. -->
                                <button class="small icon-btn" on:click={() => calibrateServo(p)}
                                        disabled={busy}
                                        title="Servo settings — open the calibration popup (live jog, limits, speed / accel / jerk).">⚙</button>
                                <button class="small icon-btn" on:click={() => resetServo(p)}
                                        disabled={busy}
                                        title="Reset this servo's motion profile to defaults (normal, 1000–2000 µs). Apply to persist.">↺</button>
                            {/if}
                        {/if}

                        <span class="fanout" title="Functions using this port">{fanout(p)}</span>

                        {#if !b.offline && hasRoleConfig(p)}
                            <!-- Heater / DC-motor: element scaling under the
                                 ⚙ Tune expander (denser, less-used than calibrate). -->
                            <button class="small cfg-btn" class:open={isExpanded(p, expandedTick)}
                                    on:click={() => toggleExpand(p)}
                                    title="Tune element scaling — live push, no re-attach">
                                {isExpanded(p, expandedTick) ? '× Close' : '⚙ Tune'}
                            </button>
                        {/if}
                    </div>

                    {#if isExpanded(p, expandedTick) && hasRoleConfig(p)}
                        {@const pk = portKindForConfig(p)}
                        {#if pk}
                            <PortRoleConfig
                                portKind={pk}
                                portIdx={p.ref.index}
                                roleKind={p.roleKind}
                                portRailMv={p.voltageMv}
                                guid={p.ref.guid} />
                        {/if}
                    {/if}
                {/each}
            </div>
        </div>
    {/each}
</div>

<style>
    .banner { padding: 7px 10px; border-radius: 4px; margin-bottom: 10px; font-size: 12px; }
    .banner.err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); }
    .banner.note { background: color-mix(in srgb, var(--accent) 12%, transparent); border: 1px solid var(--accent); color: var(--text); }
    /* Uniform header control height; narrow preset select, wider buttons. */
    .header-actions { align-items: center; }
    .header-actions button { height: 28px; min-width: 108px; box-sizing: border-box; padding: 0 12px; }
    .header-actions .field-input { height: 28px; width: 116px; box-sizing: border-box; padding: 0 6px; }

    .board-card { margin-bottom: 12px; }
    /* Offline (configured-but-disconnected) board — dimmed + warning-tinted border. */
    .board-card.offline { border-color: color-mix(in srgb, var(--warning) 55%, var(--border)); opacity: 0.92; }
    .board-card.offline .port-id, .board-card.offline .board-name { color: var(--text-dim); }
    .board-head { display: flex; align-items: baseline; gap: 8px; margin-bottom: 8px; }
    .board-name { font-size: 13px; font-weight: 600; color: var(--text-bright); }
    .board-guid { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); padding: 1px 6px; border-radius: 3px; background: var(--bg-raised); }
    .offline-badge { font-size: 9px; text-transform: uppercase; letter-spacing: 0.5px; padding: 1px 6px; border-radius: 3px; background: color-mix(in srgb, var(--warning) 22%, transparent); border: 1px solid var(--warning); color: var(--warning); }
    .remove-btn { margin-left: auto; }
    .offline-warn { font-size: 11px; font-style: italic; color: var(--warning); margin: -2px 0 8px; }
    /* Read-only role/name on offline rows — match the name-input footprint. */
    .name-ro { flex: 0 0 160px; font-family: var(--font-ui); font-size: 12px; color: var(--text-dim); padding: 4px 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

    .port-list { display: flex; flex-direction: column; gap: 4px; }
    .port-row { display: flex; align-items: center; gap: 8px; }
    .port-id { font-family: var(--font-mono); font-size: 12px; color: var(--text); width: 52px; flex-shrink: 0; }
    .dir-badge { font-size: 9px; text-transform: uppercase; letter-spacing: 0.5px; padding: 1px 6px; border-radius: 3px; border: 1px solid var(--border); flex-shrink: 0; width: 48px; text-align: center; }
    .dir-badge.input { color: var(--accent); border-color: color-mix(in srgb, var(--accent) 40%, var(--border)); }
    .dir-badge.output { color: var(--text-dim); }
    .caps { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); width: 56px; flex-shrink: 0; }
    .rail-chip { font-family: var(--font-mono); font-size: 10px; color: var(--text); padding: 1px 6px; border: 1px solid var(--border); border-radius: 3px; flex-shrink: 0; }
    .cfg-btn { flex-shrink: 0; min-width: 0; padding: 0 8px; font-size: 11px; }
    .cfg-btn.open { background: color-mix(in srgb, var(--accent) 25%, var(--bg-input)); border-color: var(--accent); }
    /* Icon-only servo buttons (settings + reset) — a square whose side equals the
       text-box height (26px), so they line up with the name input and don't widen
       the row (flex-shrink:0; the fanout absorbs them). */
    .icon-btn { flex-shrink: 0; width: 26px; height: 26px; min-width: 26px; padding: 0; box-sizing: border-box; font-size: 14px; line-height: 1; display: inline-flex; align-items: center; justify-content: center; }
    .role-select { flex: 0 0 150px; }
    .role-fixed { flex: 0 0 150px; font-size: 12px; color: var(--text-dim); padding: 4px 8px; border: 1px dashed var(--border); border-radius: 3px; text-align: center; }
    /* Constant width — does NOT flex, so adding the trailing servo buttons can't
       resize it (the fanout absorbs the difference instead).  Explicit height so
       the square icon buttons can match it exactly. */
    .name-input { flex: 0 0 160px; height: 26px; box-sizing: border-box; font-family: var(--font-ui); }
    .fanout { flex: 1; min-width: 0; font-size: 11px; color: var(--text-dim); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

</style>
