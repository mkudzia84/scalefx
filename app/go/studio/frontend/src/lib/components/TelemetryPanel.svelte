<!-- TelemetryPanel — live view of the master's telemetry collection (item 4):
     the hub-local sensors + every actively-polled input device (e.g. an ESC on
     IN_2), plus the current publish rate.  Full-width live widget (Rule 60.2),
     polled ~1 Hz while the IO tab is mounted.  Empty until a Jeti EX input is
     attached / an ESC appears on IN_2 — it's a placeholder that fills out as
     the hubfx board gains sensors. -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { telemetry, fmtSensorValue, startTelemetryPolling, stopTelemetryPolling,
             linkStates, installConnectionListener, LINK_STATE_NAMES, escTelemetryActive } from '../telemetry'
    import { deviceModel, setInputEscProtocol, setInputEscRpmScaling,
             type InputPortConfig, type PortRef } from '../devicemodel'

    onMount(() => {
        installConnectionListener()
        startTelemetryPolling()
    })
    onDestroy(() => stopTelemetryPolling())

    $: snap = $telemetry
    $: stale = !snap || (Date.now() - snap.ts) > 3000

    // Worst current link state across all tracked input sources (item 5):
    // 0 up · 1 signal lost · 2 DOWN.  Surfaced as a chip so the operator sees
    // a Jeti/SBUS/PPM link drop (and the gear emergency-deploy trigger) live.
    $: links = Object.values($linkStates)
    $: worstLink = links.reduce((w, l) => Math.max(w, l.state), 0)
    $: anyBrownouts = links.reduce((n, l) => n + (l.brownouts ?? 0), 0)

    // ── ESC telemetry sources (config moved here from the Input panel) ──
    let busy = false
    let srcError = ''
    $: escPorts = $deviceModel.inputs.filter(c => c.protocol === 'esc-telemetry')
    $: escProtocols = $deviceModel.escProtocols ?? []
    function hw(p: PortRef): string {
        const m = $deviceModel.ports.find(x =>
            x.ref.guid === p.guid && x.ref.kind === p.kind && x.ref.index === p.index)
        return m?.hardwareName || `IN${p.index + 1}`
    }
    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    async function onEscProto(p: PortRef, escProto: string) {
        busy = true; srcError = ''
        try { await setInputEscProtocol(p, escProto) }
        catch (e) { srcError = String(e) } finally { busy = false }
    }
    function numOf(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    async function onPoles(cfg: InputPortConfig, e: Event) {
        const v = Math.round(numOf(e))
        if (!Number.isFinite(v) || v < 2 || v > 100 || v % 2 !== 0) { srcError = 'Motor poles must be even, 2–100'; return }
        busy = true; srcError = ''
        try { await setInputEscRpmScaling(cfg.port, v, cfg.escGearRatio || 1) }
        catch (err) { srcError = String(err) } finally { busy = false }
    }
    async function onGear(cfg: InputPortConfig, e: Event) {
        const v = numOf(e)
        if (!Number.isFinite(v) || v <= 0 || v >= 100) { srcError = 'Gear ratio must be 0–100'; return }
        busy = true; srcError = ''
        try { await setInputEscRpmScaling(cfg.port, cfg.escMotorPoles || 2, v) }
        catch (err) { srcError = String(err) } finally { busy = false }
    }
    // Effective publish divider = pole pairs × gear (what the firmware applies).
    function rpmDivider(cfg: InputPortConfig): number {
        const pairs = (cfg.escMotorPoles && cfg.escMotorPoles >= 2) ? cfg.escMotorPoles / 2 : 1
        const gear  = (cfg.escGearRatio && cfg.escGearRatio > 0) ? cfg.escGearRatio : 1
        return pairs * gear
    }
</script>

{#if escPorts.length > 0}
    {#if srcError}<div class="banner err">{srcError}</div>{/if}
    {#each escPorts as cfg (cfg.port.guid + cfg.port.index)}
        <div class="card esc-src">
            <div class="card-header">
                <h3>ESC telemetry — {hw(cfg.port)}</h3>
                <span class="esc-chip" class:active={$escTelemetryActive}
                      title={$escTelemetryActive
                          ? 'The ESC is streaming on this telemetry link.'
                          : 'No stream — check the ESC telemetry mode / wiring.'}>
                    {$escTelemetryActive ? '● streaming' : '○ no signal'}
                </span>
            </div>
            <div class="form-row">
                <span class="field-label">ESC</span>
                <select class="field-input" style="flex:0 0 260px" value={cfg.escProtocol || 'kontronik'}
                        disabled={busy}
                        title="Which ESC's native telemetry stream this port listens to — sensors flow to the Jeti radio + this panel"
                        on:change={(e) => onEscProto(cfg.port, selValue(e))}>
                    {#each escProtocols as ep}
                        <option value={ep.id}>{ep.label}</option>
                    {/each}
                </select>
            </div>
            <div class="form-row">
                <span class="field-label">Motor poles</span>
                <input class="field-input narrow" type="number" min="2" max="100" step="2"
                       value={cfg.escMotorPoles || 2} disabled={busy}
                       title="Magnet/pole count of the motor — Kontronik transmits ELECTRICAL rpm = shaft rpm × poles/2. An outrunner's pole count is its magnet count (e.g. 10)."
                       on:change={(e) => onPoles(cfg, e)} />
                <span class="field-label">Gear ratio</span>
                <input class="field-input narrow" type="number" min="0.01" max="99" step="0.01"
                       value={cfg.escGearRatio || 1} disabled={busy}
                       title="Gearbox ratio motor:output (1 = direct drive). The published RPM is the OUTPUT shaft (head/prop) speed."
                       on:change={(e) => onGear(cfg, e)} />
                <span class="unit">: 1</span>
            </div>
            <p class="hint">
                Published RPM = transmitted ÷ <strong>{rpmDivider(cfg).toFixed(2)}</strong>
                (pole pairs × gear ratio) — Kontronik ESCs transmit electrical RPM,
                so both corrections apply before the value reaches the radio and this panel.
            </p>
        </div>
    {/each}
{/if}

<div class="card telem-card">
    <div class="card-header">
        <h3>Telemetry collection</h3>
        <div class="header-actions">
            {#if links.length > 0}
                <span class="link-chip" class:lost={worstLink === 1} class:down={worstLink === 2}
                      title="Input link health (item 5) — {links.length} source(s), {anyBrownouts} brownout(s). DOWN triggers any subscribed emergency reaction (e.g. gear deploy).">
                    link: {LINK_STATE_NAMES[worstLink]}{anyBrownouts ? ` · ${anyBrownouts} brownout${anyBrownouts === 1 ? '' : 's'}` : ''}
                </span>
            {/if}
            {#if snap}
                <span class="rate" class:stale title="Per-value target ~10 Hz; emit rate scales with sensor count.">
                    publish {(snap.respHzX10 / 10).toFixed(1)} Hz · {snap.pubIntervalMs} ms · {snap.activeSensors} active
                </span>
            {:else}
                <span class="rate stale">no data</span>
            {/if}
        </div>
    </div>

    {#if !snap || snap.devices.length === 0}
        <p class="empty">
            No telemetry yet — attach a <strong>Jeti EX input</strong>, or wait for an
            ESC to appear on IN_2.  Hub-local sensors + any polled input device will
            list here as the board gains them.
        </p>
    {:else}
        <!-- One sub-frame per device (Rule 60.8 grammar — same frame as the
             gear strut cards / other panels). -->
        <div class="dev-grid">
            {#each snap.devices as d (d.usn + ':' + d.lsn)}
                <div class="sub-frame dev" class:inactive={!d.active}>
                    <div class="frame-head">
                        {d.name || '(unnamed)'}
                        <span class="dev-tag" class:local={d.local}>{d.local ? 'hub-local' : 'downstream'}</span>
                        {#if !d.active}<span class="dev-tag stale">stale</span>{/if}
                    </div>
                    {#if d.sensors.length === 0}
                        <div class="sensor dim">— no sensors —</div>
                    {:else}
                        {#each d.sensors as s (s.id)}
                            <div class="sensor" class:dim={!s.active}>
                                <span class="s-label">{s.label || `id ${s.id}`}</span>
                                <span class="s-val">{fmtSensorValue(s)}</span>
                                <span class="s-unit">{s.unit}</span>
                            </div>
                        {/each}
                    {/if}
                </div>
            {/each}
        </div>
    {/if}
</div>

<style>
    .telem-card { margin: 0; }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .rate { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); }
    .rate.stale { opacity: 0.55; }

    .link-chip { font-family: var(--font-mono); font-size: 10px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--success); border: 1px solid var(--success); border-radius: 3px; padding: 1px 6px; }
    .link-chip.lost { color: var(--warning); border-color: var(--warning); }
    .link-chip.down { color: var(--error); border-color: var(--error); background: rgba(255,80,80,0.12); }

    .empty { color: var(--text-dim); font-size: 12px; padding: 8px 2px; }

    /* Devices laid out in the standard responsive group grid; each is a global
       .sub-frame (Rule 60.8) so it matches the frame view used elsewhere. */
    .dev-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; }
    .dev.inactive { opacity: 0.55; }
    .dev .frame-head { display: flex; align-items: baseline; gap: 8px; }
    .dev-tag { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); border: 1px solid var(--border); border-radius: 3px; padding: 0 5px; }
    .dev-tag.local { color: var(--accent); border-color: var(--accent); }
    .dev-tag.stale { color: var(--warning); border-color: var(--warning); }

    .sensor { display: grid; grid-template-columns: 1fr auto auto; align-items: baseline; gap: 8px; font-size: 12px; padding: 2px 0; }
    .sensor.dim { color: var(--text-dim); }
    .s-label { color: var(--text); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .s-val { font-family: var(--font-mono); font-weight: 600; color: var(--text-bright); }
    .sensor.dim .s-val { color: var(--text-dim); }
    .s-unit { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); min-width: 28px; }
    .esc-src { margin-bottom: 12px; }
    .esc-chip {
        font-size: 11px; padding: 2px 8px; border-radius: 10px;
        background: var(--bg-raised); color: var(--text-dim);
        border: 1px solid var(--border);
    }
    .esc-chip.active { color: var(--ok, #3fb950); border-color: var(--ok, #3fb950); }
</style>
