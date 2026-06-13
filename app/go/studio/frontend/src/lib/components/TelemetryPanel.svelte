<!-- TelemetryPanel — live view of the master's telemetry collection (item 4):
     the hub-local sensors + every actively-polled input device (e.g. an ESC on
     IN_2), plus the current publish rate.  Full-width live widget (Rule 60.2),
     polled ~1 Hz while the IO tab is mounted.  Empty until a Jeti EX input is
     attached / an ESC appears on IN_2 — it's a placeholder that fills out as
     the hubfx board gains sensors. -->
<script lang="ts">
    import { onMount, onDestroy } from 'svelte'
    import { telemetry, pollTelemetry, fmtSensorValue,
             linkStates, installConnectionListener, LINK_STATE_NAMES } from '../telemetry'

    let timer: ReturnType<typeof setInterval> | undefined
    onMount(() => {
        installConnectionListener()
        pollTelemetry()
        timer = setInterval(() => pollTelemetry(), 1000)
    })
    onDestroy(() => { if (timer) clearInterval(timer) })

    $: snap = $telemetry
    $: stale = !snap || (Date.now() - snap.ts) > 3000

    // Worst current link state across all tracked input sources (item 5):
    // 0 up · 1 signal lost · 2 DOWN.  Surfaced as a chip so the operator sees
    // a Jeti/SBUS/PPM link drop (and the gear emergency-deploy trigger) live.
    $: links = Object.values($linkStates)
    $: worstLink = links.reduce((w, l) => Math.max(w, l.state), 0)
    $: anyBrownouts = links.reduce((n, l) => n + (l.brownouts ?? 0), 0)
</script>

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
        <div class="dev-grid">
            {#each snap.devices as d (d.usn + ':' + d.lsn)}
                <div class="dev" class:inactive={!d.active}>
                    <div class="dev-head">
                        <span class="dev-name">{d.name || '(unnamed)'}</span>
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
    .telem-card { margin: 10px 0 4px; }
    .header-actions { display: flex; align-items: center; gap: 8px; }
    .rate { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); }
    .rate.stale { opacity: 0.55; }

    .link-chip { font-family: var(--font-mono); font-size: 10px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--success); border: 1px solid var(--success); border-radius: 3px; padding: 1px 6px; }
    .link-chip.lost { color: var(--warning); border-color: var(--warning); }
    .link-chip.down { color: var(--error); border-color: var(--error); background: rgba(255,80,80,0.12); }

    .empty { color: var(--text-dim); font-size: 12px; padding: 8px 2px; }

    .dev-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 10px; }
    .dev { border: 1px solid var(--border); border-radius: 5px; padding: 8px 10px; background: var(--bg-input); }
    .dev.inactive { opacity: 0.55; }
    .dev-head { display: flex; align-items: baseline; gap: 8px; margin-bottom: 6px; padding-bottom: 4px; border-bottom: 1px dashed var(--border); }
    .dev-name { font-weight: 600; color: var(--text-bright); font-size: 13px; }
    .dev-tag { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); border: 1px solid var(--border); border-radius: 3px; padding: 0 5px; }
    .dev-tag.local { color: var(--accent); border-color: var(--accent); }
    .dev-tag.stale { color: var(--warning); border-color: var(--warning); }

    .sensor { display: grid; grid-template-columns: 1fr auto auto; align-items: baseline; gap: 8px; font-size: 12px; padding: 2px 0; }
    .sensor.dim { color: var(--text-dim); }
    .s-label { color: var(--text); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .s-val { font-family: var(--font-mono); font-weight: 600; color: var(--text-bright); }
    .sensor.dim .s-val { color: var(--text-dim); }
    .s-unit { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); min-width: 28px; }
</style>
