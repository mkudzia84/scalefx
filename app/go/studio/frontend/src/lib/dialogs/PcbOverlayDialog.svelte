<!-- ScaleFX Studio — PCB schematic overlay (tabbed).
     One tab per board: the hub is always first, then one tab for each
     connected expander AND each abandoned (configured-but-disconnected)
     board.  Each tab shows that board type's top-side photo with a marker
     on every port (positions measured by tools/analyze_<board>.go); markers
     render from the board layout, hovering highlights one, clicking opens a
     popover to set the role / name.  Abandoned boards show an OFFLINE warning
     + a "Remove from config" action. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import { showPcbOverlay, connectionInfo } from '../stores'
    import {
        deviceModel, refresh, boardKindOf, boardDisplayNames,
        claimsForPort, formatPortRail, removeAbandonedBoard, type Port,
    } from '../devicemodel'
    import { effectClaims } from '../effect-claims'
    import { pcbFor, type PortMarker } from '../pcb'
    import PortControls from '../components/PortControls.svelte'

    interface BoardTab { guid: string; kind: string; name: string; online: boolean }

    $: hubKind = $connectionInfo.controllerType
    $: names = boardDisplayNames($deviceModel.ports)

    // One tab per distinct board (by GUID).  Hub first, then expanders by GUID;
    // an abandoned board (online === false) is included like any other.
    $: boardTabs = buildTabs($deviceModel.ports)
    function buildTabs(ports: Port[]): BoardTab[] {
        const seen = new Map<string, BoardTab>()
        for (const p of ports) {
            if (seen.has(p.ref.guid)) continue
            seen.set(p.ref.guid, {
                guid: p.ref.guid,
                kind: boardKindOf(p.boardName),
                name: names[p.ref.guid] ?? p.boardName,
                online: !p.offline,
            })
        }
        const arr = [...seen.values()]
        arr.sort((a, b) => {
            const ah = a.kind === hubKind, bh = b.kind === hubKind
            if (ah !== bh) return ah ? -1 : 1
            return a.guid < b.guid ? -1 : a.guid > b.guid ? 1 : 0
        })
        return arr
    }

    let activeGuid = ''
    // Keep the active tab valid as boards connect / disconnect.
    $: if (boardTabs.length && !boardTabs.some(t => t.guid === activeGuid)) {
        activeGuid = boardTabs[0].guid
    }
    $: activeTab = boardTabs.find(t => t.guid === activeGuid)
    $: pcb = activeTab ? pcbFor(activeTab.kind) : undefined
    $: boardPorts = activeTab ? $deviceModel.ports.filter(p => p.ref.guid === activeTab.guid) : []

    let selKey = ''
    let removing = false
    let error = ''

    // Pull the model when the dialog opens so role state + the edit popover
    // are populated even if a prior refresh timed out.
    onMount(() => { refresh().catch(() => {}) })

    function mkKey(m: PortMarker): string { return `${m.kind}:${m.index}` }
    function portFor(m: PortMarker): Port | undefined {
        return boardPorts.find(p => p.ref.kind === m.kind && p.ref.index === m.index)
    }
    function functions(p: Port): string {
        // $effectClaims (merged hard + effect-draft soft claims), not the bare
        // $deviceModel.claims which only holds ApplyPreset's hard claims —
        // without the merge a port used by an effect read as "unclaimed".
        return claimsForPort($effectClaims, p.ref).map(c => `${c.domain}/${c.slot}`).join(', ')
    }
    async function removeBoard() {
        if (!activeTab) return
        removing = true; error = ''
        try {
            await removeAbandonedBoard(activeTab.guid)
            selKey = ''
        } catch (e) { error = String(e) } finally { removing = false }
    }
    function close() { $showPcbOverlay = false }
</script>

<div class="backdrop" on:click|self={close} on:keydown={(e) => { if (e.key === 'Escape') close() }} role="presentation">
    <div class="panel">
        <div class="panel-head">
            <h2>Diagram{activeTab ? ` — ${activeTab.name}` : ''}{activeTab && activeTab.guid ? ` · ${activeTab.guid}` : ''}</h2>
            <button class="small" on:click={close}>✕ Close</button>
        </div>

        {#if boardTabs.length > 1}
            <div class="tab-strip" role="tablist">
                {#each boardTabs as t (t.guid)}
                    <button class="board-tab" class:active={t.guid === activeGuid} class:offline={!t.online}
                            role="tab" aria-selected={t.guid === activeGuid}
                            on:click={() => { activeGuid = t.guid; selKey = '' }}
                            title={t.online ? t.name : `${t.name} — configured but not connected`}>
                        {t.name}
                        {#if t.guid}<span class="tab-guid" title="Board GUID">{t.guid}</span>{/if}
                        {#if !t.online}<span class="tab-badge">offline</span>{/if}
                    </button>
                {/each}
            </div>
        {/if}

        {#if error}<div class="banner err">{error}</div>{/if}
        {#if activeTab && !activeTab.online}
            <div class="banner warn">
                <span><strong>{activeTab.name}</strong> is configured in <code>/hubfx.yaml</code> but not connected.
                    Its ports are shown from the saved config so you can still edit or remove them.</span>
                <button class="small danger" on:click={removeBoard} disabled={removing}
                        title="Delete this board's entry from /hubfx.yaml (ports, roles, names). Apply to persist.">
                    {removing ? 'Removing…' : '🗑 Remove from config'}
                </button>
            </div>
        {/if}

        <div class="panel-body" on:click|self={() => (selKey = '')} role="presentation">
            {#if pcb}
                <div class="board-wrap" class:dim={activeTab && !activeTab.online} on:click|self={() => (selKey = '')} role="presentation">
                    <img class="board-img" src={pcb.image} alt="{activeTab?.name} board" />
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
                                    {#if activeTab && !activeTab.online}
                                        <div class="pop-fn">Role: {p.roleName === 'none' ? '— none —' : p.roleName} — board offline, reconnect to edit.</div>
                                    {:else}
                                        <PortControls port={p} />
                                    {/if}
                                {:else}
                                    <div class="pop-fn">Port not enumerated yet — <button class="link" on:click={() => refresh().catch(() => {})}>refresh</button>.</div>
                                {/if}
                            </div>
                        {/if}
                    {/each}
                </div>
            {:else}
                <div class="empty-state">No board image bundled for “{activeTab?.kind || hubKind || 'this device'}”.</div>
            {/if}
        </div>
    </div>
</div>

<style>
    .backdrop { position: fixed; inset: 0; background: rgba(0,0,0,0.6); backdrop-filter: blur(3px); display: flex; align-items: center; justify-content: center; z-index: 200; }
    /* FIXED size so switching to a board with different schematic dimensions
       doesn't resize the popup — the image scales to fit + centres inside. */
    .panel { background: var(--bg-base); border: 1px solid var(--border); border-radius: 10px; width: min(1180px, 95vw); height: min(860px, 93vh); display: flex; flex-direction: column; box-shadow: 0 8px 32px var(--shadow); }
    .panel-head { display: flex; align-items: center; justify-content: space-between; gap: 24px; padding: 12px 16px; border-bottom: 1px solid var(--border); flex-shrink: 0; }
    .panel-head h2 { font-size: 16px; color: var(--text-bright); }

    /* Board tab strip — one tab per board (hub first). */
    .tab-strip { display: flex; gap: 4px; padding: 8px 16px 0; border-bottom: 1px solid var(--border); flex-shrink: 0; flex-wrap: wrap; }
    .board-tab { display: inline-flex; align-items: center; gap: 6px; background: var(--bg-surface); border: 1px solid var(--border); border-bottom: none; border-radius: 6px 6px 0 0; padding: 6px 12px; font-size: 12px; color: var(--text-dim); cursor: pointer; }
    .board-tab:hover { color: var(--text); background: var(--bg-raised); }
    .board-tab.active { color: var(--text-bright); background: var(--bg-base); border-color: var(--border); position: relative; top: 1px; }
    .board-tab.offline { color: var(--warning); }
    .tab-guid { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); padding: 1px 5px; border-radius: 3px; background: var(--bg-raised); }
    .tab-badge { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; padding: 1px 5px; border-radius: 3px; background: color-mix(in srgb, var(--warning) 22%, transparent); border: 1px solid var(--warning); color: var(--warning); }

    .banner { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 8px 16px; font-size: 12px; flex-shrink: 0; }
    .banner.err { background: rgba(255,80,80,0.12); border-bottom: 1px solid var(--error); color: var(--error); }
    .banner.warn { background: color-mix(in srgb, var(--warning) 12%, transparent); border-bottom: 1px solid var(--warning); color: var(--text); }
    .banner code { font-family: var(--font-mono); font-size: 11px; }

    .panel-body { flex: 1; overflow: auto; padding: 16px; display: flex; align-items: center; justify-content: center; min-height: 0; }

    /* The wrap shrink-wraps the scaled image so markers (positioned in %) stay
       aligned; the image is capped to a CONSTANT box (derived from the fixed
       panel minus chrome) so every tab's schematic centres inside the same
       footprint regardless of its native aspect ratio. */
    .board-wrap { position: relative; display: inline-block; flex-shrink: 0; }
    .board-wrap.dim .board-img { opacity: 0.72; filter: saturate(0.7); }
    .board-img { display: block; max-width: min(1130px, calc(95vw - 50px)); max-height: calc(min(860px, 93vh) - 200px); width: auto; height: auto; border-radius: 6px; }

    /* Unassigned = red, assigned = vibrant blue (both pop against the green PCB). */
    .marker { position: absolute; transform: translate(-50%, -50%); font-family: var(--font-mono); font-size: 13px; font-weight: 700; line-height: 1; padding: 4px 8px; border-radius: 4px; border: 2px solid #ff4d44; background: rgba(224,40,32,0.88); color: #fff; cursor: pointer; white-space: nowrap; box-shadow: 0 1px 4px rgba(0,0,0,0.5); transition: transform 0.08s, background 0.1s; }
    .marker:hover { background: #ff4d44; transform: translate(-50%, -50%) scale(1.12); z-index: 4; }
    .marker.assigned { background: #1f7aff; border-color: #4f9bff; }
    .marker.assigned:hover { background: #3d8bff; }
    .marker.open { z-index: 6; outline: 2px solid #fff; }

    /* Informational (non-port) markers — speaker / IN headers etc. Read-only. */
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
