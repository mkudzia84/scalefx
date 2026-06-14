<!-- WizardStepChannels — Step 3: map each channel to a function (Rule 43
     named inputs) + a live fitness check against the features you enabled. -->
<script lang="ts">
    import { deviceModel, setChannelFunction, liveChannels, liveChannelKey, usToPct } from '../devicemodel'
    import { analyzeWizard } from '../wizard-advice'

    $: inputs = $deviceModel.inputs
    $: advice = analyzeWizard($deviceModel)

    const selValue = (e: Event) => (e.target as HTMLSelectElement).value

    // Group the channel-function catalog for the dropdown optgroups.
    $: groups = (() => {
        const m = new Map<string, { id: string; label: string }[]>()
        for (const f of $deviceModel.channelFunctions) {
            if (f.id === 'unassigned') continue
            if (!m.has(f.group)) m.set(f.group, [])
            m.get(f.group)!.push({ id: f.id, label: f.label })
        }
        return [...m.entries()]
    })()

    function fnOf(inp: any, ch: number): string {
        return inp.channels.find((c: any) => c.channel === ch)?.function ?? 'unassigned'
    }
    function liveUs(ref: { guid: string; index: number }, ch: number): { us: number; valid: boolean } | null {
        return $liveChannels[liveChannelKey({ guid: ref.guid, kind: 4, index: ref.index }, ch)] ?? null
    }
    function portLabel(ref: { guid: string; index: number }): string {
        const p = $deviceModel.ports.find(pp => pp.ref.guid === ref.guid && pp.ref.index === ref.index && pp.direction === 'input')
        return p ? `${p.boardName || 'Hub'} · ${p.name || `IN_${ref.index + 1}`}` : `Input ${ref.index + 1}`
    }
</script>

{#each inputs as inp (inp.port.guid + '/' + inp.port.index)}
    <div class="ch-card">
        <div class="ch-head">{portLabel(inp.port)} — {inp.channelCount} channels</div>
        {#each Array(inp.channelCount) as _, ch}
            {@const lv = liveUs(inp.port, ch)}
            <div class="ch-row">
                <span class="ch-n">CH{ch + 1}</span>
                <div class="ch-bar"><div class="ch-fill" class:nosig={!lv || !lv.valid} style="width:{lv && lv.valid ? usToPct(lv.us) : 0}%"></div></div>
                <select class="field-input" value={fnOf(inp, ch)}
                        on:change={(e) => setChannelFunction(inp.port, ch, selValue(e))}>
                    <option value="unassigned">— unassigned —</option>
                    {#each groups as [g, fns]}
                        <optgroup label={g}>
                            {#each fns as fn}<option value={fn.id}>{fn.label}</option>{/each}
                        </optgroup>
                    {/each}
                </select>
            </div>
        {/each}
    </div>
{/each}

{#if advice.length}
    <div class="advice">
        <div class="advice-head">Fitness check</div>
        {#each advice as a}
            <div class="advice-row {a.level}"><span class="dot">{a.level === 'error' ? '✖' : a.level === 'warn' ? '⚠' : 'ℹ'}</span>{a.message}</div>
        {/each}
    </div>
{:else}
    <div class="advice ok">✓ Every enabled effect has the channel it needs.</div>
{/if}

<style>
    .ch-card { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-input); padding: 10px 12px; margin-bottom: 10px; }
    .ch-head { font-size: 12px; font-weight: 700; color: var(--text-bright); margin-bottom: 6px; }
    .ch-row { display: flex; align-items: center; gap: 10px; margin: 4px 0; }
    .ch-n { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); min-width: 36px; }
    .ch-bar { flex: 1; height: 14px; background: var(--bg-surface); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; min-width: 80px; max-width: 220px; }
    .ch-fill { height: 100%; background: var(--success); }
    .ch-fill.nosig { background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 5px, transparent 5px, transparent 10px); }
    .ch-row .field-input { flex: 1; max-width: 280px; }

    .advice { margin-top: 16px; border: 1px solid var(--border); border-radius: 6px; padding: 10px 12px; background: var(--bg-raised); }
    .advice.ok { color: var(--success); font-size: 12px; }
    .advice-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 6px; }
    .advice-row { font-size: 12px; display: flex; gap: 8px; padding: 3px 0; }
    .advice-row .dot { flex-shrink: 0; }
    .advice-row.warn { color: var(--warning); }
    .advice-row.error { color: var(--error); }
    .advice-row.info { color: var(--text-dim); }
</style>
