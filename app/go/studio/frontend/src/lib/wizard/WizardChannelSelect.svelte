<!-- WizardChannelSelect — bind an effect field to a NAMED RC channel (Rule 43).
     Options are the channels named on the Channel-map step. -->
<script lang="ts">
    import { deviceModel } from '../devicemodel'
    import { collectChannelOptions } from '../channels'

    export let value: string
    export let label: string
    export let onChange: (fn: string) => void
    export let emptyLabel = '— manual (no radio) —'

    const selValue = (e: Event) => (e.target as HTMLSelectElement).value

    $: opts = (() => {
        const seen = new Set<string>()
        return collectChannelOptions($deviceModel).filter(o => (seen.has(o.fnId) ? false : (seen.add(o.fnId), true)))
    })()
</script>

<div class="wcs-row">
    <span class="wcs-label">{label}</span>
    <select class="field-input wcs-select" value={value} on:change={(e) => onChange(selValue(e))}>
        <option value="">{emptyLabel}</option>
        {#each opts as o}<option value={o.fnId}>{o.label}</option>{/each}
    </select>
</div>

<style>
    .wcs-row { display: flex; align-items: center; gap: 10px; margin: 5px 0; }
    .wcs-label { font-size: 11px; color: var(--text-dim); min-width: 110px; }
    .wcs-select { flex: 1; min-width: 240px; }
</style>
