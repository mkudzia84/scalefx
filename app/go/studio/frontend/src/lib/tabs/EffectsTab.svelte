<!-- ScaleFX Studio — Effects tab.
     Same shell as IoTab: sticky top bar + single scroll container
     wrapping both columns.  Each effect panel manages its own Apply /
     enable inside its `.status-row`, so the top bar carries only the
     tab title (and any future tab-level actions). -->
<script lang="ts">
    import EnginePanel from './EnginePanel.svelte'
    import GunFxPanel from './GunFxPanel.svelte'
</script>

<div class="effects">
    <div class="effects-header">
        <h3>Effects</h3>
    </div>
    <div class="effects-scroll">
        <div class="effects-cols">
            <div class="col left"><EnginePanel /></div>
            <div class="col right"><GunFxPanel /></div>
        </div>
    </div>
</div>

<style>
    .effects { display: flex; flex-direction: column; height: 100%; min-height: 0; }

    .effects-header {
        display: flex; align-items: center; gap: 12px;
        padding: 6px 12px 8px;
        border-bottom: 1px solid var(--border);
        background: var(--bg);
        flex-shrink: 0;
    }
    .effects-header h3 { margin: 0; font-size: 14px; font-weight: 600; color: var(--text-bright); }

    /* Single scroll wrapper: both columns scroll together under the
       pinned top bar — same pattern as IoTab. */
    .effects-scroll { flex: 1; min-height: 0; overflow: auto; }
    .effects-cols   { display: flex; min-height: 100%; padding: 12px; gap: 14px; }
    .col            { flex: 1; min-width: 0; }
</style>
