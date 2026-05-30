<!-- One port's role + name editing, shared by the Ports list and the PCB
     overlay popover.  Servo ports (one possible role) show a fixed "Servo"
     tag + name; every other port shows a role picker limited to the kinds
     it can host.  Uses shared design-system classes (Rule 34). -->
<script lang="ts">
    import { attachRole, detachRole, setPortName, RoleKind,
             syncJetiExpanderRemap, type Port } from '../devicemodel'

    export let port: Port
    let busy = false

    $: isServo = port.kindName === 'servo'

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }

    async function onRole(roleKind: number) {
        busy = true
        try {
            if (roleKind === RoleKind.None) await detachRole(port.ref)
            else await attachRole(port.ref, roleKind)
            await syncJetiExpanderRemap(port.ref, roleKind)   // IN_1 JetiEX → IN_2 telemetry
        } finally { busy = false }
    }
    async function onName(name: string) {
        busy = true
        try { await setPortName(port.ref, name.trim()) } finally { busy = false }
    }
</script>

<div class="pc">
    <label class="fld">
        <span class="fld-label">Role / Function</span>
        {#if isServo}
            <span class="role-fixed" title="Servo ports can only host a servo actuator">Servo</span>
        {:else}
            <select class="field-input" value={port.roleKind} disabled={busy}
                    on:change={(e) => onRole(Number(selValue(e)))} title="Role this port performs">
                <option value={RoleKind.None}>— none —</option>
                {#each port.allowedRoles as r}
                    <option value={r.kind}>{r.label}</option>
                {/each}
            </select>
        {/if}
    </label>
    <label class="fld">
        <span class="fld-label">Name</span>
        <input class="field-input" type="text" placeholder="optional label…"
               value={port.name} disabled={busy}
               on:change={(e) => onName(inputValue(e))} />
    </label>
</div>

<style>
    .pc { display: flex; flex-direction: column; gap: 7px; }
    .fld { display: flex; flex-direction: column; gap: 3px; }
    .fld-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); }
    .role-fixed { font-size: 12px; color: var(--text-dim); padding: 4px 8px; border: 1px dashed var(--border); border-radius: 3px; }
</style>
