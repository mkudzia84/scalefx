<!-- WizardStepInput — Step 2: the radio link.  Per input port: pick the
     protocol (PPM / SBUS / Jeti) and how many channels.  Writes /hubfx.yaml
     inputs[] via the same mutators the IO tab uses. -->
<script lang="ts">
    import { deviceModel, setInputProtocol, setInputChannelCount, liveChannels, liveChannelKey } from '../devicemodel'

    $: inputs = $deviceModel.inputs
    $: protocols = $deviceModel.inputProtocols.filter(p => p.implemented)

    const selValue = (e: Event) => (e.target as HTMLSelectElement).value
    const numValue = (e: Event) => Number((e.target as HTMLInputElement).value)

    function portLabel(ref: { guid: string; index: number }): string {
        const p = $deviceModel.ports.find(pp => pp.ref.guid === ref.guid && pp.ref.index === ref.index && pp.direction === 'input')
        if (p) return `${p.boardName || 'Hub'} · ${p.name || `IN_${ref.index + 1}`}`
        return `Input ${ref.index + 1}`
    }
    function maxFor(protocol: string): number {
        return $deviceModel.inputProtocols.find(p => p.id === protocol)?.maxChannels ?? 16
    }
    /** How many channels are currently showing a live signal on this port. */
    function liveCount(ref: { guid: string; index: number }, count: number): number {
        let n = 0
        for (let c = 0; c < count + 4; c++) {
            const v = $liveChannels[liveChannelKey({ guid: ref.guid, kind: 4, index: ref.index }, c)]
            if (v && v.valid && c + 1 > n) n = c + 1
        }
        return n
    }
</script>

{#if inputs.length === 0}
    <p class="muted">No input ports on this board.</p>
{:else}
    {#each inputs as inp (inp.port.guid + '/' + inp.port.index)}
        {@const live = liveCount(inp.port, inp.channelCount)}
        <div class="in-card">
            <div class="in-head">{portLabel(inp.port)}</div>
            <div class="in-row">
                <span class="lbl">Protocol</span>
                <select class="field-input" value={inp.protocol}
                        on:change={(e) => setInputProtocol(inp.port, selValue(e))}>
                    {#each protocols as p}
                        <option value={p.id}>{p.label}</option>
                    {/each}
                </select>
            </div>
            <div class="in-row">
                <span class="lbl">Channels</span>
                <input class="field-input narrow" type="number" min="1" max={maxFor(inp.protocol)}
                       value={inp.channelCount}
                       on:change={(e) => setInputChannelCount(inp.port, Math.max(1, Math.min(maxFor(inp.protocol), Math.round(numValue(e)))))} />
                <span class="unit">of {maxFor(inp.protocol)} max</span>
                {#if live > 0}
                    <button class="small" on:click={() => setInputChannelCount(inp.port, Math.min(maxFor(inp.protocol), live))}
                            title="Set the channel count to the {live} channels currently detected on the wire">
                        ⤢ Autodetect ({live})
                    </button>
                {:else}
                    <span class="muted small">no live signal</span>
                {/if}
            </div>
        </div>
    {/each}
{/if}
<p class="muted hint">Next you'll name what each channel does. The radio frame usually pads to a fixed width — name only the channels you actually use.</p>

<style>
    .in-card { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-raised); padding: 12px 14px; margin-bottom: 10px; }
    .in-head { font-size: 12px; font-weight: 700; color: var(--text-bright); margin-bottom: 8px; }
    .in-row { display: flex; align-items: center; gap: 10px; margin: 6px 0; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 70px; }
    .field-input.narrow { width: 80px; }
    .unit { font-size: 11px; color: var(--text-dim); }
    .muted { color: var(--text-dim); font-size: 12px; }
    .muted.small { font-size: 11px; }
    .hint { margin-top: 14px; }
</style>
