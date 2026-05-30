<!-- ScaleFX Studio — Input panel (left column of the Input & Ports tab).
     Per input port: RC protocol, channel count, and a per-channel
     function assignment with a live value bar fed by the hub's RC
     broadcast stream. -->
<script lang="ts">
    import {
        deviceModel, liveChannels, setInputProtocol, setInputChannelCount,
        setChannelFunction, liveChannelKey, usToPct, boardDisplayNames,
        formatPortRail, RoleKind,
        type InputPortConfig, type ChannelFunctionDef, type PortRef,
        type InputProtocolDef,
    } from '../devicemodel'

    let busy = false
    let error = ''

    $: inputs = $deviceModel.inputs
    $: protocols = $deviceModel.inputProtocols
    $: names = boardDisplayNames($deviceModel.ports)
    $: functionGroups = groupFns($deviceModel.channelFunctions)

    function groupFns(fns: ChannelFunctionDef[]): { group: string; items: ChannelFunctionDef[] }[] {
        const order: string[] = []
        const by = new Map<string, ChannelFunctionDef[]>()
        for (const f of fns) {
            if (!by.has(f.group)) { by.set(f.group, []); order.push(f.group) }
            by.get(f.group)!.push(f)
        }
        return order.map(g => ({ group: g, items: by.get(g)! }))
    }

    // Function ids from the catalog (built-ins).  Anything else came from
    // /hubfx.yaml's inputs[] (operator-authored binding name) and gets a
    // "Custom" optgroup so the value is visible in the dropdown.
    $: knownFns = new Set($deviceModel.channelFunctions.map(f => f.id))
    function customsForInputs(cfgs: InputPortConfig[], known: Set<string>): string[] {
        const seen = new Set<string>()
        for (const cfg of cfgs) {
            for (const ch of cfg.channels) {
                if (ch.function && ch.function !== 'unassigned' && !known.has(ch.function)) {
                    seen.add(ch.function)
                }
            }
        }
        return [...seen].sort()
    }
    $: customFns = customsForInputs(inputs, knownFns)

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }

    async function onProtocol(p: PortRef, proto: string) {
        busy = true; error = ''
        try {
            await setInputProtocol(p, proto)
            // Clamp the channel count down if the new protocol carries
            // fewer channels than were declared (e.g. PPM 8 → a future
            // 1-channel PWM mode).
            const cfg = inputs.find(c => c.port.guid === p.guid && c.port.index === p.index)
            const max = maxCh(proto)
            if (cfg && cfg.channelCount > max) await setInputChannelCount(p, max)
            // IN_2's telemetry role is inferred by the firmware (the expander
            // stamps it) and surfaced via the topology re-read in AttachRole —
            // no UI-side remap needed; the returned snapshot already has it.
        } catch (e) { error = String(e) } finally { busy = false }
    }
    async function onCount(p: PortRef, n: number) {
        busy = true; error = ''
        const clamped = Math.max(1, Math.min(n, maxCh(protoOf(p))))
        try { await setInputChannelCount(p, clamped) } catch (e) { error = String(e) } finally { busy = false }
    }
    // Current protocol id for a port (for clamping in onCount).
    function protoOf(p: PortRef): string {
        return inputs.find(c => c.port.guid === p.guid && c.port.index === p.index)?.protocol ?? ''
    }
    async function onFunction(p: PortRef, ch: number, fn: string) {
        busy = true; error = ''
        try { await setChannelFunction(p, ch, fn) } catch (e) { error = String(e) } finally { busy = false }
    }

    // Highest channel index seen live on this port (+1), i.e. how many
    // channels the signal actually carries.  Takes the live store as a param
    // so the {@const} that calls it re-evaluates when channels update.
    function detectedCount(cfg: InputPortConfig, lc: Record<string, { us: number; valid: boolean }>): number {
        const prefix = `${cfg.port.guid}|${cfg.port.index}|`
        let max = 0
        for (const k of Object.keys(lc)) {
            if (!k.startsWith(prefix)) continue
            const ch = parseInt(k.slice(prefix.length), 10)
            if (!Number.isNaN(ch) && ch + 1 > max) max = ch + 1
        }
        return max
    }

    // Silkscreen label for the input port (from the model; fallback IN<n>).
    function hw(p: PortRef): string {
        const m = $deviceModel.ports.find(x =>
            x.ref.guid === p.guid && x.ref.kind === p.kind && x.ref.index === p.index)
        return m?.hardwareName || `IN${p.index + 1}`
    }
    // The Port model row for this input ref (carries allowedRoles + voltage).
    function portOf(p: PortRef) {
        return $deviceModel.ports.find(x =>
            x.ref.guid === p.guid && x.ref.kind === p.kind && x.ref.index === p.index)
    }
    // A Jeti EX Telemetry port is the downstream telemetry PASS-THRU (no RC
    // channels) — auto-assigned to IN_2 while IN_1 runs Jeti EX.  It carries a
    // downstream slave's (e.g. ESC) telemetry toward the Rx, so it renders as a
    // compact pass-thru row, NOT a channel-input group with function mappings.
    function isTelemetryPassthru(cfg: InputPortConfig): boolean {
        return portOf(cfg.port)?.roleKind === RoleKind.JetiExTelemetry
    }
    // Rule 34: the protocol picker offers ONLY protocols whose backing
    // role is in the port's allowedRoles — a PWM-pulse-only input shows
    // just PPM, never SBUS/Jeti.  The currently-selected protocol is kept
    // visible even if (somehow) outside the set, so editing never blanks
    // the field.  On a board whose input allows all RC roles every
    // protocol shows; the filter matters for narrower ports.
    function protosFor(p: PortRef, current: string): InputProtocolDef[] {
        const allowed = new Set((portOf(p)?.allowedRoles ?? []).map(r => r.kind))
        return protocols.filter(pr => {
            if (pr.id === current) return true                  // always keep the current value
            // Jeti EX Telemetry is the AUTO downstream role — assigned to IN_2
            // when IN_1 is set to Jeti EX, never user-picked here.
            if (pr.roleKind === RoleKind.JetiExTelemetry) return false
            return pr.roleKind === undefined || allowed.size === 0 || allowed.has(pr.roleKind)
        })
    }
    // Selected protocol's channel ceiling (PPM 8, SBUS/Jeti 16). The
    // channel-count input binds its `max` to this so the operator can't
    // declare more channels than the protocol carries.
    function maxCh(protocolId: string): number {
        return protocols.find(pr => pr.id === protocolId)?.maxChannels ?? 18
    }
    // Rail voltage for the port (from the model). "" when unknown.
    function rail(p: PortRef): string {
        const m = $deviceModel.ports.find(x =>
            x.ref.guid === p.guid && x.ref.kind === p.kind && x.ref.index === p.index)
        return formatPortRail(m?.voltageMv ?? 0)
    }
</script>

<div class="tab-content">
    <!-- Hint paragraph removed (it offset InputPanel content vs
         PortRoleTab content on the other column, breaking row
         alignment).  Each input row's controls + tooltips describe
         the function inline. -->

    {#if error}<div class="banner err">{error}</div>{/if}
    {#if inputs.length === 0}<div class="empty-state">No input ports on this system.</div>{/if}

    {#each inputs as cfg (cfg.port.guid + cfg.port.index)}
        {#if isTelemetryPassthru(cfg)}
            <!-- Telemetry pass-thru (IN_2): a downstream EX-Bus telemetry input,
                 NOT an RC channel source and NOT an output — no channel group. -->
            <div class="card input-card passthru">
                <div class="board-head">
                    <span class="board-name">{names[cfg.port.guid] ?? hw(cfg.port)} · {hw(cfg.port)}</span>
                    {#if rail(cfg.port)}
                        <span class="rail-chip" title="Rail voltage declared by the board's port descriptor">{rail(cfg.port)}</span>
                    {/if}
                    <span class="passthru-tag">Jeti EX Telemetry · pass-thru</span>
                </div>
            </div>
        {:else}
        {@const det = detectedCount(cfg, $liveChannels)}
        <div class="card input-card">
            <div class="board-head">
                <span class="board-name">{names[cfg.port.guid] ?? hw(cfg.port)} · {hw(cfg.port)}</span>
                {#if rail(cfg.port)}
                    <span class="rail-chip" title="Rail voltage declared by the board's port descriptor">{rail(cfg.port)}</span>
                {/if}
            </div>

            <div class="form-row">
                <span class="field-label">Protocol</span>
                <select class="field-input" style="flex:0 0 150px" value={cfg.protocol} disabled={busy}
                        title="Input decoding mode — limited to the roles this port can host"
                        on:change={(e) => onProtocol(cfg.port, selValue(e))}>
                    {#each protosFor(cfg.port, cfg.protocol) as proto}
                        <option value={proto.id} disabled={!proto.implemented}>
                            {proto.label}{proto.implemented ? '' : ' (soon)'}
                        </option>
                    {/each}
                </select>
                <span class="field-label">Channels</span>
                <input class="field-input narrow" type="number" min="1" max={maxCh(cfg.protocol)}
                       value={cfg.channelCount} disabled={busy}
                       title="Channels decoded from this input (max {maxCh(cfg.protocol)} for {cfg.protocol})"
                       on:change={(e) => onCount(cfg.port, numValue(e))} />
                <button class="small" disabled={busy || det === 0}
                        title={det === 0
                            ? 'No live signal — enable the input / check wiring'
                            : `Set channel count to ${det} (detected on the live ${cfg.protocol} signal)`}
                        on:click={() => onCount(cfg.port, det)}>
                    Autodetect{det > 0 ? ` (${det})` : ''}
                </button>
            </div>

            <div class="channels">
                {#each cfg.channels as ch (ch.channel)}
                    <!-- Read $liveChannels INLINE so this @const re-evaluates
                         on every frame — a helper that reads the store
                         internally is invisible to Svelte's reactivity and
                         freezes the bar (same trap as the gun status pills). -->
                    {@const lv = $liveChannels[liveChannelKey(cfg.port, ch.channel)]}
                    {@const signal = !!lv && lv.valid}
                    <div class="ch-block">
                        <div class="ch-top">
                            <span class="ch-idx">CH{ch.channel + 1}</span>
                            <select class="field-input ch-fn" value={ch.function} disabled={busy}
                                    on:change={(e) => onFunction(cfg.port, ch.channel, selValue(e))}>
                                {#each functionGroups as g}
                                    <optgroup label={g.group}>
                                        {#each g.items as f}
                                            <option value={f.id}>{f.label}</option>
                                        {/each}
                                    </optgroup>
                                {/each}
                                {#if customFns.length > 0}
                                    <optgroup label="Custom (from /hubfx.yaml)">
                                        {#each customFns as fn}
                                            <option value={fn}>{fn}</option>
                                        {/each}
                                    </optgroup>
                                {/if}
                            </select>
                        </div>
                        <div class="bar" class:nosignal={!signal}>
                            {#if signal}
                                <div class="bar-fill" style="width: {usToPct(lv.us)}%"></div>
                                <span class="bar-val">{lv.us}µs</span>
                            {:else}
                                <span class="bar-nosignal">NO SIGNAL</span>
                            {/if}
                        </div>
                    </div>
                {/each}
            </div>
        </div>
        {/if}
    {/each}
</div>

<style>
    .banner { padding: 7px 10px; border-radius: 4px; margin-bottom: 10px; font-size: 12px; }
    .banner.err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); }
    .input-card { margin-bottom: 12px; }
    /* Telemetry pass-thru: dimmer, no channel group — it's a downstream link. */
    .input-card.passthru { border-left: 2px solid var(--accent); }
    .input-card.passthru .board-head { margin-bottom: 0; }
    .passthru-tag { margin-left: auto; font-family: var(--font-mono); font-size: 10px; color: var(--accent); padding: 1px 6px; border: 1px solid var(--accent); border-radius: 3px; }
    .board-head { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }
    .board-name { font-size: 13px; font-weight: 600; color: var(--text-bright); }
    .rail-chip { font-family: var(--font-mono); font-size: 10px; color: var(--text); padding: 1px 6px; border: 1px solid var(--border); border-radius: 3px; }
    .channels { display: flex; flex-direction: column; gap: 10px; }
    .ch-block { display: flex; flex-direction: column; gap: 4px; }
    .ch-top { display: flex; align-items: center; gap: 8px; }
    .ch-idx { font-family: var(--font-mono); font-size: 11px; font-weight: 600; color: var(--text); width: 38px; flex-shrink: 0; }
    .ch-fn { flex: 1; font-family: var(--font-ui); }
    .bar { position: relative; height: 16px; background: var(--bg-raised); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; }
    .bar-fill { height: 100%; background: linear-gradient(90deg, var(--accent), var(--success)); transition: width 0.08s linear; }
    .bar-val { position: absolute; right: 6px; top: 0; line-height: 16px; font-family: var(--font-mono); font-size: 10px; color: var(--text-bright); text-shadow: 0 0 3px rgba(0,0,0,0.7); }
    /* Explicit no-signal: striped grey track + centered label. */
    .bar.nosignal {
        background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 6px, transparent 6px, transparent 12px);
    }
    .bar-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-family: var(--font-mono); font-size: 9px; letter-spacing: 0.8px; color: var(--text-dim); }
</style>
