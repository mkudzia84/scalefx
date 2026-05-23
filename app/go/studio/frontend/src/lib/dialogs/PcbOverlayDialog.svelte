<!-- ScaleFX Studio — PCB schematic overlay.
     The hub's board photo, ALONE, with a marker on each port (positions
     measured by tools/analyze_hubfx.go).  Markers always render from the
     board layout; hovering highlights one and shows its role, clicking
     opens a popover to set the role / name.  Expander ports are configured
     in the main Ports & Roles list, not here. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import { showPcbOverlay, connectionInfo } from '../stores'
    import { deviceModel, refresh, boardKindOf, claimsForPort, formatPortRail, type Port } from '../devicemodel'
    import { pcbFor, type PortMarker } from '../pcb'
    import PortControls from '../components/PortControls.svelte'

    $: hubKind = $connectionInfo.controllerType
    $: pcb = pcbFor(hubKind)
    $: hubName = $connectionInfo.controllerName || 'Hub'
    // The hub's own ports come back tagged with its GUID (not ""), so find
    // the board whose kind matches the connected controller and use its
    // ports for the markers.
    $: hubGuid = $deviceModel.ports.find(p => boardKindOf(p.boardName) === hubKind)?.ref.guid ?? ''
    $: hubPorts = $deviceModel.ports.filter(p => p.ref.guid === hubGuid)

    let selKey = ''

    // Pull the model when the dialog opens so role state + the edit popover
    // are populated even if a prior refresh timed out.
    onMount(() => { refresh().catch(() => {}) })

    function mkKey(m: PortMarker): string { return `${m.kind}:${m.index}` }
    function portFor(m: PortMarker): Port | undefined {
        return hubPorts.find(p => p.ref.kind === m.kind && p.ref.index === m.index)
    }
    function functions(p: Port): string {
        return claimsForPort($deviceModel.claims, p.ref).map(c => `${c.domain}/${c.slot}`).join(', ')
    }
    function close() { $showPcbOverlay = false }
</script>

<div class="backdrop" on:click|self={close} on:keydown={(e) => { if (e.key === 'Escape') close() }} role="presentation">
    <div class="panel">
        <div class="panel-head">
            <h2>Diagram — {hubName}</h2>
            <button class="small" on:click={close}>✕ Close</button>
        </div>

        <div class="panel-body" on:click|self={() => (selKey = '')} role="presentation">
            {#if pcb}
                <div class="board-wrap" on:click|self={() => (selKey = '')} role="presentation">
                    <img class="board-img" src={pcb.image} alt="{hubName} board" />
                    {#each pcb.info as info (info.label)}
                    <span class="info-marker" style="left:{info.x}%; top:{info.y}%" title={info.title}>{info.label}</span>
                {/each}
                {#each pcb.markers as m (mkKey(m))}
                        {@const p = portFor(m)}
                        {@const assigned = !!p && p.roleKind !== 0}
                        {@const rail = p ? formatPortRail(p.voltageMv) : ''}
                        <button class="marker" class:assigned class:open={selKey === mkKey(m)}
                                style="left:{m.x}%; top:{m.y}%"
                                title={p ? `${m.label} · ${p.kindName}${rail ? ` · ${rail} rail` : ''}${p.roleName !== 'none' ? ` — ${p.roleName}` : ''}` : m.label}
                                on:click|stopPropagation={() => (selKey = selKey === mkKey(m) ? '' : mkKey(m))}>
                            {m.label}
                        </button>
                        {#if selKey === mkKey(m)}
                            <div class="popover" class:right={m.x > 50} style="left:{m.x}%; top:{m.y}%"
                                 on:click|stopPropagation role="presentation">
                                <div class="pop-head">
                                    <span class="pop-label">{m.label}</span>
                                    <span class="pop-right">
                                        {#if rail}<span class="pop-rail" title="Rail voltage declared by the board">{rail}</span>{/if}
                                        {#if p}<span class="pop-role">{p.roleName === 'none' ? 'no role' : p.roleName}</span>{/if}
                                        <button class="pop-x" title="Close" on:click={() => (selKey = '')}>✕</button>
                                    </span>
                                </div>
                                {#if p}
                                    {#if functions(p)}<div class="pop-fn">used by: {functions(p)}</div>{/if}
                                    <PortControls port={p} />
                                {:else}
                                    <div class="pop-fn">Port not enumerated yet — <button class="link" on:click={() => refresh().catch(() => {})}>refresh</button>.</div>
                                {/if}
                            </div>
                        {/if}
                    {/each}
                </div>
            {:else}
                <div class="empty-state">No board image bundled for “{hubKind || 'this device'}”.</div>
            {/if}
        </div>
    </div>
</div>

<style>
    .backdrop { position: fixed; inset: 0; background: rgba(0,0,0,0.6); backdrop-filter: blur(3px); display: flex; align-items: center; justify-content: center; z-index: 200; }
    .panel { background: var(--bg-base); border: 1px solid var(--border); border-radius: 10px; max-width: 96vw; max-height: 94vh; display: flex; flex-direction: column; box-shadow: 0 8px 32px var(--shadow); }
    .panel-head { display: flex; align-items: center; justify-content: space-between; gap: 24px; padding: 12px 16px; border-bottom: 1px solid var(--border); flex-shrink: 0; }
    .panel-head h2 { font-size: 16px; color: var(--text-bright); }
    .panel-body { flex: 1; overflow: auto; padding: 16px; display: flex; align-items: center; justify-content: center; min-height: 0; }

    .board-wrap { position: relative; display: inline-block; }
    .board-img { display: block; max-width: calc(96vw - 32px); max-height: calc(94vh - 90px); width: auto; height: auto; border-radius: 6px; }

    /* Unassigned = red, assigned = vibrant blue (both pop against the green PCB). */
    .marker { position: absolute; transform: translate(-50%, -50%); font-family: var(--font-mono); font-size: 13px; font-weight: 700; line-height: 1; padding: 4px 8px; border-radius: 4px; border: 2px solid #ff4d44; background: rgba(224,40,32,0.88); color: #fff; cursor: pointer; white-space: nowrap; box-shadow: 0 1px 4px rgba(0,0,0,0.5); transition: transform 0.08s, background 0.1s; }
    .marker:hover { background: #ff4d44; transform: translate(-50%, -50%) scale(1.12); z-index: 4; }
    .marker.assigned { background: #1f7aff; border-color: #4f9bff; }
    .marker.assigned:hover { background: #3d8bff; }
    .marker.open { z-index: 6; outline: 2px solid #fff; }

    /* Informational (non-port) markers — speaker outputs etc. Read-only. */
    .info-marker { position: absolute; transform: translate(-50%, -50%); font-family: var(--font-mono); font-size: 11px; font-weight: 700; line-height: 1; padding: 3px 7px; border-radius: 4px; border: 2px solid #3b82f6; background: rgba(40,90,200,0.8); color: #fff; white-space: nowrap; box-shadow: 0 1px 4px rgba(0,0,0,0.5); pointer-events: none; }

    .popover { position: absolute; transform: translate(-10%, 12px); z-index: 10; background: var(--bg-surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; min-width: 230px; box-shadow: 0 4px 18px var(--shadow); }
    /* Right-side markers open toward the left so the popover stays on-screen. */
    .popover.right { transform: translate(-90%, 12px); }
    .pop-head { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; margin-bottom: 6px; }
    .pop-label { font-family: var(--font-mono); font-size: 12px; font-weight: 700; color: var(--text-bright); }
    .pop-right { display: flex; align-items: center; gap: 8px; }
    .pop-role { font-size: 10px; color: var(--accent); text-transform: uppercase; letter-spacing: 0.4px; }
    .pop-rail { font-size: 10px; font-family: var(--font-mono); color: var(--text-dim); padding: 1px 6px; border: 1px solid var(--border); border-radius: 3px; }
    .pop-x { background: none; border: none; color: var(--text-dim); cursor: pointer; font-size: 12px; line-height: 1; padding: 2px 4px; border-radius: 3px; }
    .pop-x:hover { color: var(--text-bright); background: var(--bg-raised); }
    .pop-fn { font-size: 10px; color: var(--text-dim); margin-bottom: 6px; }
    .link { background: none; border: none; color: var(--accent); cursor: pointer; padding: 0; font-size: 10px; text-decoration: underline; }
</style>
