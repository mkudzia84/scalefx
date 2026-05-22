<!-- ScaleFX Studio — Input panel (left column of the Input & Ports tab).
     Per input port: RC protocol, channel count, and a per-channel
     function assignment with a live value bar fed by the hub's RC
     broadcast stream. -->
<script lang="ts">
    import {
        deviceModel, liveChannels, setInputProtocol, setInputChannelCount,
        setChannelFunction, liveChannelKey, usToPct, boardDisplayNames,
        type InputPortConfig, type ChannelFunctionDef, type PortRef,
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

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }

    async function onProtocol(p: PortRef, proto: string) {
        busy = true; error = ''
        try { await setInputProtocol(p, proto) } catch (e) { error = String(e) } finally { busy = false }
    }
    async function onCount(p: PortRef, n: number) {
        busy = true; error = ''
        try { await setInputChannelCount(p, n) } catch (e) { error = String(e) } finally { busy = false }
    }
    async function onFunction(p: PortRef, ch: number, fn: string) {
        busy = true; error = ''
        try { await setChannelFunction(p, ch, fn) } catch (e) { error = String(e) } finally { busy = false }
    }

    function live(cfg: InputPortConfig, ch: number) {
        return $liveChannels[liveChannelKey(cfg.port, ch)]
    }

    // Silkscreen label for the input port (from the model; fallback IN<n>).
    function hw(p: PortRef): string {
        const m = $deviceModel.ports.find(x =>
            x.ref.guid === p.guid && x.ref.kind === p.kind && x.ref.index === p.index)
        return m?.hardwareName || `IN${p.index + 1}`
    }
</script>

<div class="tab-content">
    <div class="card-header">
        <h3>Input</h3>
    </div>
    <p class="field-hint" style="margin-bottom:12px">RC receiver protocol, channels, and what each channel does. Bars show live values from the hub.</p>

    {#if error}<div class="banner err">{error}</div>{/if}
    {#if inputs.length === 0}<div class="empty-state">No input ports on this system.</div>{/if}

    {#each inputs as cfg (cfg.port.guid + cfg.port.index)}
        <div class="card input-card">
            <div class="board-head">
                <span class="board-name">{names[cfg.port.guid] ?? hw(cfg.port)} · {hw(cfg.port)}</span>
            </div>

            <div class="form-row">
                <span class="field-label">Protocol</span>
                <select class="field-input" style="flex:0 0 150px" value={cfg.protocol} disabled={busy}
                        on:change={(e) => onProtocol(cfg.port, selValue(e))}>
                    {#each protocols as proto}
                        <option value={proto.id} disabled={!proto.implemented}>
                            {proto.label}{proto.implemented ? '' : ' (soon)'}
                        </option>
                    {/each}
                </select>
                <span class="field-label">Channels</span>
                <input class="field-input narrow" type="number" min="1" max="18" value={cfg.channelCount} disabled={busy}
                       on:change={(e) => onCount(cfg.port, numValue(e))} />
            </div>

            <div class="channels">
                {#each cfg.channels as ch (ch.channel)}
                    {@const lv = live(cfg, ch.channel)}
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
    {/each}
</div>

<style>
    .banner { padding: 7px 10px; border-radius: 4px; margin-bottom: 10px; font-size: 12px; }
    .banner.err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); }
    .input-card { margin-bottom: 12px; }
    .board-head { margin-bottom: 10px; }
    .board-name { font-size: 13px; font-weight: 600; color: var(--text-bright); }
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
