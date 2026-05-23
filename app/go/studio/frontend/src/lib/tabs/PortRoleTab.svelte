<!-- ScaleFX Studio — Ports & Roles (right column of the Input & Ports tab).
     One card per board; each port shows its role (a picker limited to the
     roles that port kind can host, or a fixed "Servo" tag for servos) and
     an operator name.  Uses the shared design system (.card, .form-row,
     .field-input, button) so control heights line up. -->
<script lang="ts">
    import {
        deviceModel, refresh, attachRole, detachRole, applyPreset, applyDefaults,
        applyHubConfig, setPortName, portKindName, boardDisplayNames, claimsForPort,
        validationCounts, RoleKind, type Port, type PortRef,
    } from '../devicemodel'
    import { showPcbOverlay } from '../stores'

    let busy = false
    let error = ''
    let note = ''
    let presetSel = ''

    // No on-mount refresh — App.svelte refreshes on connect and the store
    // drives this view, so there's no transient "not connected" flash.
    $: names = boardDisplayNames($deviceModel.ports)

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }

    async function doRefresh() {
        busy = true; error = ''
        try { await refresh() } catch (e) { error = String(e) } finally { busy = false }
    }

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

    async function onApplyPreset() {
        if (!presetSel) return
        busy = true; error = ''; note = ''
        try {
            const w = await applyPreset(presetSel)
            note = w.length ? `Applied "${presetSel}" (${w.length} note(s)): ${w.join('; ')}` : `Applied "${presetSel}".`
        } catch (e) { error = String(e) } finally { busy = false; presetSel = '' }
    }

    async function onApplyDefaults() {
        busy = true; error = ''; note = ''
        try {
            const w = await applyDefaults()
            note = w.length ? `Applied defaults (${w.length} note(s)): ${w.join('; ')}` : 'Applied sensible defaults.'
        } catch (e) { error = String(e) } finally { busy = false }
    }

    async function onApply() {
        busy = true; error = ''; note = ''
        try {
            await applyHubConfig()
            note = 'Applied — /hubfx.yaml written and firmware reloaded.'
        } catch (e) { error = String(e) } finally { busy = false }
    }

    // Output ports only — input ports are configured in the left column
    // (InputPanel), so they're excluded here to avoid duplicating them.
    $: boards = groupByBoard($deviceModel.ports.filter(p => p.direction === 'output'))
    function groupByBoard(ports: Port[]): { guid: string; name: string; ports: Port[] }[] {
        const by = new Map<string, { guid: string; name: string; ports: Port[] }>()
        for (const p of ports) {
            const g = by.get(p.ref.guid) ?? { guid: p.ref.guid, name: p.boardName, ports: [] }
            g.ports.push(p)
            by.set(p.ref.guid, g)
        }
        return [...by.values()]
    }

    function isServo(p: Port): boolean { return p.kindName === 'servo' }
    function fanout(p: Port): string {
        return claimsForPort($deviceModel.claims, p.ref).map(c => `${c.domain}/${c.slot}`).join(', ')
    }
</script>

<div class="tab-content">
    <div class="card-header">
        <h3>Ports &amp; Roles</h3>
        <div class="header-actions">
            {#if $deviceModel.presets.length > 0}
                <select class="field-input" bind:value={presetSel} disabled={busy} title="Apply a named port/role mapping">
                    <option value="">Presets…</option>
                    {#each $deviceModel.presets as p}
                        <option value={p.name} title={p.description}>{p.name}</option>
                    {/each}
                </select>
                <button class="small apply-btn" on:click={onApplyPreset} disabled={busy || !presetSel}>Apply</button>
            {/if}
            <button class="small" on:click={() => ($showPcbOverlay = true)} title="Open the board diagram — assign roles on the PCB photo">▣ Diagram</button>
            <button class="small primary" on:click={onApply} disabled={busy || $validationCounts.errors > 0}
                    title={$validationCounts.errors > 0 ? 'Resolve validation errors first (see badge in the tab strip)' : 'Write /hubfx.yaml + reload — pushes current channel functions, port names and role attachments to the firmware live'}>✓ Apply</button>
            <button class="small" on:click={onApplyDefaults} disabled={busy} title="Apply the bundled sensible defaults for the connected boards">★ Defaults</button>
            <button class="small" on:click={doRefresh} disabled={busy}>↻ Refresh</button>
        </div>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}
    {#if note}<div class="banner note">{note}</div>{/if}
    {#if boards.length === 0}<div class="empty-state">No output ports on the connected boards.</div>{/if}

    {#each boards as b (b.guid)}
        <div class="card board-card">
            <div class="board-head">
                <span class="board-name">{names[b.guid] ?? b.name}</span>
            </div>
            <div class="port-list">
                {#each b.ports as p (p.kindName + p.ref.index)}
                    <div class="port-row">
                        <span class="port-id" title="{portKindName[p.ref.kind]} {p.ref.index}">{p.hardwareName}</span>
                        <span class="dir-badge {p.direction}">{p.direction}</span>
                        <span class="caps">{p.caps.join(' ') || ''}</span>

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

                        <span class="fanout" title="Functions using this port">{fanout(p)}</span>
                    </div>
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
    /* The preset Apply button is a verb, keep it compact. */
    .header-actions button.apply-btn { min-width: 64px; }

    .board-card { margin-bottom: 12px; }
    .board-head { display: flex; align-items: baseline; gap: 8px; margin-bottom: 8px; }
    .board-name { font-size: 13px; font-weight: 600; color: var(--text-bright); }

    .port-list { display: flex; flex-direction: column; gap: 4px; }
    .port-row { display: flex; align-items: center; gap: 8px; }
    .port-id { font-family: var(--font-mono); font-size: 12px; color: var(--text); width: 52px; flex-shrink: 0; }
    .dir-badge { font-size: 9px; text-transform: uppercase; letter-spacing: 0.5px; padding: 1px 6px; border-radius: 3px; border: 1px solid var(--border); flex-shrink: 0; width: 48px; text-align: center; }
    .dir-badge.input { color: var(--accent); border-color: color-mix(in srgb, var(--accent) 40%, var(--border)); }
    .dir-badge.output { color: var(--text-dim); }
    .caps { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); width: 56px; flex-shrink: 0; }
    .role-select { flex: 0 0 150px; }
    .role-fixed { flex: 0 0 150px; font-size: 12px; color: var(--text-dim); padding: 4px 8px; border: 1px dashed var(--border); border-radius: 3px; text-align: center; }
    .name-input { flex: 1; min-width: 80px; font-family: var(--font-ui); }
    .fanout { flex: 1; font-size: 11px; color: var(--text-dim); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
</style>
