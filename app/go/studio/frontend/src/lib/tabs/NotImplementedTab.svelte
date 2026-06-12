<!-- NotImplementedTab — the deliberate placeholder for a domain WITHOUT a
     dedicated panel (Rule 59).  The generic slot-editor DomainTab was
     retired 2026-06-13: it silently swallowed mis-routed domains (the
     landing-gear → "Gear units"/"Gear switch" incident) and shipped a
     confusing half-UI.  A domain the tab router can't place now renders
     THIS — loudly unfinished, never half-working — so a missing hookup is
     caught on the first open instead of masquerading as a feature. -->
<script lang="ts">
    import type { Domain } from '../devicemodel'

    export let domain: Domain | undefined = undefined
</script>

<div class="card ni-card">
    <div class="card-header">
        <h3>{domain?.label ?? 'Unknown domain'}</h3>
    </div>
    <div class="ni-body">
        <p class="ni-title">Not implemented</p>
        <p class="help-text">
            This domain is advertised by the connected board but has no dedicated Studio panel yet
            (domain id: <code>{domain?.id ?? '?'}</code>).
        </p>
        <p class="help-text">
            If a panel for it exists, its tab routing is missing — hook the domain id into
            <code>studioTabs</code> (devicemodel.ts) and the tab switch in
            <code>MainLayout.svelte</code> (Rule 59).  Until then, configure this feature via its
            YAML config file or the scalefx-cli.
        </p>
    </div>
</div>

<style>
    .ni-card { margin: 12px; }
    .ni-body { padding: 18px 8px; }
    .ni-title { font-size: 15px; font-weight: 700; color: var(--warning); text-transform: uppercase; letter-spacing: 1px; margin: 0 0 10px; }
    .ni-body code { font-family: var(--font-mono); font-size: 11px; background: var(--bg-input); padding: 1px 5px; border-radius: 3px; }
</style>
