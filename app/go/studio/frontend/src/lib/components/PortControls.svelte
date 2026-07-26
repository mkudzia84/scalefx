<!-- One port's role + name editing, shared by the Ports list and the PCB
     overlay popover.  Servo ports (one possible role) show a fixed "Servo"
     tag + name; every other port shows a role picker limited to the kinds
     it can host.  For INPUT ports the picker is harmonised with the Input &
     Ports tab (Rule 34 shared design language): the RC-vs-telemetry split
     (the telemetry input hosts only ESC telemetry, the primary input hosts
     only the RC-channel roles) plus an inline ESC telemetry-type selector. -->
<script lang="ts">
    import {
        attachRole, detachRole, setPortName, setInputEscProtocol,
        deviceModel, RoleKind, type Port,
    } from '../devicemodel'

    export let port: Port
    let busy = false

    $: isServo = port.kindName === 'servo'
    $: isInput = port.kindName === 'input'

    // The input config carries the selected ESC telemetry stream (escProtocol).
    $: inputCfg = $deviceModel.inputs.find(
        c => c.port.guid === port.ref.guid && c.port.index === port.ref.index)
    $: escProtocols = $deviceModel.escProtocols ?? []

    // RC-vs-telemetry split (mirrors InputPanel.protosFor): on a multi-input
    // board the HIGHEST-index input is the telemetry monitor (ESC telemetry
    // only); the others are RC-channel inputs (no ESC telemetry).
    $: telemetryInput = (() => {
        if (!isInput) return false
        const board = $deviceModel.inputs.filter(c => c.port.guid === port.ref.guid)
        if (board.length < 2) return false
        const maxIdx = Math.max(...board.map(c => c.port.index))
        return port.ref.index === maxIdx
    })()

    // Role options: for inputs, apply the RC/telemetry split (keeping the
    // current role visible so editing never blanks the field).
    $: roleOptions = isInput
        ? port.allowedRoles.filter(r =>
            r.kind === port.roleKind ||
            (telemetryInput ? r.kind === RoleKind.EscTelemetry : r.kind !== RoleKind.EscTelemetry))
        : port.allowedRoles

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }

    async function onRole(roleKind: number) {
        busy = true
        try {
            // IN_2's Jeti telemetry role is inferred firmware-side and surfaced
            // by AttachRole's topology re-read — no UI-side remap here.
            if (roleKind === RoleKind.None) await detachRole(port.ref)
            else await attachRole(port.ref, roleKind)
        } finally { busy = false }
    }
    async function onEscProto(escProto: string) {
        busy = true
        try { await setInputEscProtocol(port.ref, escProto) } finally { busy = false }
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
                {#each roleOptions as r}
                    <option value={r.kind}>{r.label}</option>
                {/each}
            </select>
        {/if}
    </label>
    {#if isInput && port.roleKind === RoleKind.EscTelemetry}
        <label class="fld">
            <span class="fld-label">Telemetry type</span>
            <select class="field-input" value={inputCfg?.escProtocol || 'kontronik'} disabled={busy}
                    on:change={(e) => onEscProto(selValue(e))}
                    title="Which ESC's native telemetry stream this port decodes">
                {#each escProtocols as ep}
                    <option value={ep.id}>{ep.label}</option>
                {/each}
            </select>
        </label>
    {/if}
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
