<!-- WizardPortPicker — a role-aware port dropdown that AUTO-ATTACHES the role
     on pick (so the operator never visits the IO tab).  Reused by every
     effect step for motors / servos / LEDs / heaters / fans. -->
<script lang="ts">
    import { deviceModel } from '../devicemodel'
    import { refOptLabel } from '../components/port_keys'
    import { effectClaims } from '../effect-claims'
    import { assignablePool, ensureRole, portToRef, emptyRef, refKey, portKey, type PortRefT } from '../wizard-ports'

    export let value: PortRefT | null = null
    export let kind: 'servo' | 'pwm' | 'hbridge'
    export let role: number
    export let label = ''
    export let onChange: (ref: PortRefT) => void
    export let busy = false

    $: pool = assignablePool($deviceModel.ports, $effectClaims, kind, role, value)
    $: empty = pool.length === 0 && !(value && value.kind)

    async function pick(e: Event) {
        const key = (e.target as HTMLSelectElement).value
        if (!key) { onChange(emptyRef(kind)); return }
        const p = $deviceModel.ports.find(pp => portKey(pp) === key)
        if (!p) { onChange(emptyRef(kind)); return }
        await ensureRole(p, role)
        onChange(portToRef(p))
    }
</script>

<div class="wpp-row">
    {#if label}<span class="wpp-label">{label}</span>{/if}
    <select class="field-input wpp-select" value={refKey(value)} on:change={pick} disabled={busy || empty}>
        <option value="">{empty ? `— no free ${kind} port —` : '— pick a port —'}</option>
        {#each pool as p (portKey(p))}
            <option value={portKey(p)}>{refOptLabel(p)}</option>
        {/each}
    </select>
</div>
{#if empty}<div class="wpp-warn">No free {kind} port available — connect/configure more hardware.</div>{/if}

<style>
    .wpp-row { display: flex; align-items: center; gap: 10px; margin: 5px 0; }
    .wpp-label { font-size: 11px; color: var(--text-dim); min-width: 110px; }
    .wpp-select { flex: 1; min-width: 240px; }
    .wpp-warn { font-size: 11px; color: var(--warning); margin: 2px 0 4px 120px; }
</style>
