<!-- ScaleFX Studio — generic functional-domain tab.
     Renders ONE domain (landing lights, gear, …) from the device-model
     catalog: each slot shows the ports currently claimed plus a picker of
     legal candidates (ports whose attached role satisfies the slot).  All
     selection validation happens in the Go backend; this tab only renders
     the model and issues claim/unclaim calls.  New functional tabs need no
     bespoke code — they pass a different domain in. -->
<script lang="ts">
    import {
        deviceModel, claim, unclaim, candidates,
        portKindName, claimsForSlot, issuesFor, boardLabel,
        type Domain, type Slot, type Port, type Claim, type PortRef,
    } from '../devicemodel'

    export let domain: Domain

    let busy = false
    let error = ''
    // Candidate ports per slot, refreshed whenever the model changes.
    let cands: Record<string, Port[]> = {}

    // Recompute candidates when the model or domain changes.
    $: void refreshCandidates($deviceModel, domain)
    async function refreshCandidates(_dm: unknown, d: Domain) {
        if (!d) return
        const next: Record<string, Port[]> = {}
        for (const s of d.slots) {
            try { next[s.key] = await candidates(d.id, s.key) } catch { next[s.key] = [] }
        }
        cands = next
    }

    async function onClaim(slot: string, ref: PortRef) {
        busy = true; error = ''
        try { await claim(domain.id, slot, ref) } catch (e) { error = String(e) } finally { busy = false }
    }

    // Picker change handler — parse the chosen port ref, claim it, reset
    // the select to its placeholder.  Lives in script so the template
    // expression stays cast-free (Svelte can't parse inline TS `as`).
    function onPick(slotKey: string, e: Event) {
        const sel = e.target as HTMLSelectElement
        const v = sel.value
        if (v) { onClaim(slotKey, JSON.parse(v) as PortRef); sel.selectedIndex = 0 }
    }
    async function onUnclaim(c: Claim) {
        busy = true; error = ''
        try { await unclaim(c.domain, c.slot, c.port) } catch (e) { error = String(e) } finally { busy = false }
    }

    function portLabel(ref: PortRef): string {
        return `${boardLabel(ref.guid)} ${portKindName[ref.kind]}${ref.index}`
    }
    function slotClaims(slot: Slot): Claim[] {
        return claimsForSlot($deviceModel.claims, domain.id, slot.key)
    }
    function slotFull(slot: Slot): boolean {
        return slot.max > 0 && slotClaims(slot).length >= slot.max
    }
</script>

<div class="tab">
    <header class="tab-head">
        <div>
            <h2>{domain.label}</h2>
            <p class="sub">Assign ports to this function. Only ports with a matching role appear in each picker — set roles on the Ports &amp; Roles tab.</p>
        </div>
    </header>

    {#if error}<div class="err">{error}</div>{/if}

    {#each domain.slots as slot (slot.key)}
        {@const claimed = slotClaims(slot)}
        {@const slotIssues = issuesFor($deviceModel.issues, domain.id, slot.key)}
        <section class="slot">
            <div class="slot-head">
                <h3>{slot.label}</h3>
                <span class="card" class:req={!slot.optional}>
                    {claimed.length}{slot.max > 0 ? `/${slot.max}` : ''}
                    {slot.optional ? 'optional' : `min ${slot.min}`}
                    {slot.shared ? '· shared' : ''}
                </span>
            </div>

            {#if claimed.length > 0}
                <ul class="claimed">
                    {#each claimed as c (c.port.guid + c.port.kind + c.port.index)}
                        <li>
                            <span class="mono">{portLabel(c.port)}</span>
                            <button class="x" title="Release this port" on:click={() => onUnclaim(c)} disabled={busy}>✕</button>
                        </li>
                    {/each}
                </ul>
            {/if}

            {#if !slotFull(slot)}
                <div class="picker">
                    <select disabled={busy || (cands[slot.key]?.length ?? 0) === 0}
                            on:change={(e) => onPick(slot.key, e)}>
                        <option value="">{(cands[slot.key]?.length ?? 0) === 0 ? '— no eligible ports —' : '+ add port…'}</option>
                        {#each cands[slot.key] ?? [] as p (p.ref.guid + p.ref.kind + p.ref.index)}
                            <option value={JSON.stringify(p.ref)}>{portLabel(p.ref)} ({p.roleName})</option>
                        {/each}
                    </select>
                </div>
            {/if}

            {#each slotIssues as iss}
                <div class="issue {iss.severity}">{iss.message}</div>
            {/each}
        </section>
    {/each}
</div>

<style>
    .tab { padding: 16px; overflow: auto; }
    .tab-head { margin-bottom: 12px; }
    h2 { margin: 0; font-size: 18px; color: var(--text-bright); }
    .sub { margin: 4px 0 0; color: var(--text-dim); font-size: 12px; }
    .err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); padding: 8px 10px; border-radius: 6px; margin-bottom: 10px; font-size: 12px; }
    .slot { border: 1px solid var(--border); border-radius: 8px; padding: 10px 12px; margin-bottom: 12px; background: var(--bg-surface); }
    .slot-head { display: flex; align-items: center; justify-content: space-between; }
    h3 { font-size: 14px; color: var(--text); margin: 0; }
    .card { font-size: 11px; color: var(--text-dim); font-family: var(--font-mono); }
    .card.req { color: var(--text); }
    .claimed { list-style: none; margin: 8px 0 6px; padding: 0; display: flex; flex-wrap: wrap; gap: 6px; }
    .claimed li { display: flex; align-items: center; gap: 6px; background: var(--bg-raised); border: 1px solid var(--border); border-radius: 6px; padding: 3px 8px; }
    .mono { font-family: var(--font-mono); font-size: 12px; }
    .x { background: transparent; border: none; color: var(--text-dim); cursor: pointer; font-size: 11px; }
    .x:hover { color: var(--error); }
    .picker select { background: var(--bg-input); border: 1px solid var(--border); border-radius: 4px; color: var(--text); padding: 3px 8px; min-width: 220px; }
    .issue { font-size: 11px; margin-top: 6px; padding: 4px 8px; border-radius: 4px; }
    .issue.error { color: var(--error); background: rgba(255,80,80,0.1); }
    .issue.warn { color: var(--warning); background: rgba(255,200,0,0.08); }
</style>
