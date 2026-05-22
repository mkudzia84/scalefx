<!-- ScaleFX Studio — GunFX panel (placeholder).
     Enable toggle in the header; the full config form (per-unit muzzle
     flash / recoil servo / smoke / trigger) lands in a follow-up turn. -->
<script lang="ts">
    import { gunFxDraft } from '../effects'

    // Bind to the store so the enable toggle survives a tab switch.
    $: enabled = $gunFxDraft.enabled

    function onToggle(e: Event) {
        gunFxDraft.set({ enabled: (e.target as HTMLInputElement).checked })
    }
</script>

<div class="card" class:disabled={!enabled}>
    <div class="card-header">
        <h3>GunFX</h3>
        <label class="enable-toggle">
            <input type="checkbox" checked={enabled} on:change={onToggle} />
            <span>{enabled ? 'Enabled' : 'Disabled'}</span>
        </label>
    </div>
    {#if enabled}
        <p class="placeholder">
            Per-unit gun configuration (muzzle flash · recoil servo · smoke
            heater · trigger channel) — coming next.
        </p>
    {/if}
</div>

<style>
    .card.disabled { opacity: 0.78; }
    .enable-toggle { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--text-dim); cursor: pointer; }
    .enable-toggle input { accent-color: var(--accent); }
    .placeholder { color: var(--text-dim); font-size: 12px; padding: 12px 4px; font-style: italic; }
</style>
