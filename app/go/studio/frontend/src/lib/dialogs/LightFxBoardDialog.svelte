<!-- ScaleFX Studio — LightFX board layout viewer. -->
<!-- Uses the native PNG resolution (706 × 445) as the SVG viewBox so every -->
<!-- hotspot rectangle is positioned in raw image-pixel coordinates. The SVG -->
<!-- scales proportionally to fit the dialog without distorting the hits.    -->
<!--
    Pixel-coord analysis (image = 706 × 445).
    Bounding boxes are measured directly from silkscreen pixels via
    tools/analyze_lightfx_image.py — NOT eyeballed. Re-run the tool
    if the render is regenerated.

    Detection strategy (mirrors analyze_gearcontrol_image.py):
      • LED JSTs (CH1-CH8): white-mask housings, 8 largest white components
        sit on the top + bottom edges (~60 × 66 px each). Top row reads
        CH4 CH3 CH2 CH1 left-to-right; bottom row reads CH5 CH6 CH7 CH8.
      • USB-C: largest white component on the right edge.
      • Servo headers (Servo1-3): magenta "SIG" silkscreen dot per row, then
        extended right to cover the 3-pin header housing.
      • Battery pads (BAT − / +): grey/silver annular pads in the lower-left,
        merged into one bbox covering both pads.
      • LED1 (POWER) / LED2 (status): magenta-marker pairs on the right edge.

      CH1   (LED JST, top row, rightmost)        x= 376 y=   0  w= 60 h= 66
      CH2                                         x= 285 y=   0  w= 59 h= 66
      CH3                                         x= 193 y=   0  w= 59 h= 66
      CH4   (LED JST, top row, leftmost)         x= 101 y=   0  w= 59 h= 66
      CH5   (LED JST, bottom row, leftmost)      x= 104 y= 379  w= 59 h= 66
      CH6                                         x= 196 y= 379  w= 59 h= 66
      CH7                                         x= 285 y= 379  w= 59 h= 66
      CH8   (LED JST, bottom row, rightmost)     x= 376 y= 379  w= 60 h= 66
      Servo1                                      x=  10 y=  99  w= 60 h= 32
      Servo2                                      x=  10 y= 141  w= 60 h= 32
      Servo3                                      x=  10 y= 183  w= 60 h= 32
      BAT    (− / + solder pads, combined)       x=   6 y= 217  w= 57 h=116
      USB    (USB-C, J1)                          x= 628 y= 179  w= 78 h= 87
      LED1   (POWER indicator)                    x= 667 y= 288  w= 39 h= 18
      LED2   (status indicator)                   x= 655 y= 321  w= 52 h= 24

    Pin map (mirrors lightfx_pico.ino):
      CH1=GP0  CH2=GP1  CH3=GP2  CH4=GP3
      CH5=GP4  CH6=GP5  CH7=GP6  CH8=GP7
      Servo1=GP8   Servo2=GP9
      Servo3=GP10 (also RC PWM "light input" on direct LightFX)
      LED1=power rail   LED2=GP24/25 (CONN/ERR via firmware)
      BAT=VSYS / GP29 sense
-->
<script lang="ts">
    import boardImage from '../../assets/images/lightfx_2d.png'

    // Live per-channel state (mirrors StatusBroadcast).
    export let ledLive: { bri: number; seq: boolean; enabled: boolean }[] = []

    export let open: boolean = false
    export let onClose: (() => void) | null = null

    // Servo 3 runtime pulse width (µs). In direct mode this doubles as the RC
    // PWM "light input" pulse the firmware reads to pick the active program;
    // SRV1/SRV2 stay regular servo outputs in either mode.
    export let slaveMode: boolean = false
    export let liveInput_us: number = 0

    // Battery telemetry — rendered on the BAT hotspot.
    export let batteryChemistry: string = 'lipo'
    export let batteryCellCount: number = 0   // 0 = auto-infer from voltage
    export let batteryVoltage_mV: number = 0

    const CHEMISTRY_LABEL: Record<string, string> = { lipo: 'LiPo', liion: 'Li-Ion', nimh: 'NiMH' }

    // ─── Image native dimensions (must match the PNG). ───
    const IMG_W = 706
    const IMG_H = 445

    interface Hotspot {
        id: string
        label: string
        sub: string                // pin sublabel (GPx) or footprint id
        x: number; y: number; w: number; h: number
    }

    // Hotspots in image-pixel coordinates (see header comment above).
    // Order is purely cosmetic — render order = SVG draw order (later = on top).
    const hotspots: Hotspot[] = [
        // Top row (CH4 CH3 CH2 CH1, left → right)
        { id: 'led4', label: 'CH4', sub: 'GP3', x: 101, y:   0, w:  59, h:  66 },
        { id: 'led3', label: 'CH3', sub: 'GP2', x: 193, y:   0, w:  59, h:  66 },
        { id: 'led2', label: 'CH2', sub: 'GP1', x: 285, y:   0, w:  59, h:  66 },
        { id: 'led1', label: 'CH1', sub: 'GP0', x: 376, y:   0, w:  60, h:  66 },
        // Bottom row (CH5 CH6 CH7 CH8, left → right)
        { id: 'led5', label: 'CH5', sub: 'GP4', x: 104, y: 379, w:  59, h:  66 },
        { id: 'led6', label: 'CH6', sub: 'GP5', x: 196, y: 379, w:  59, h:  66 },
        { id: 'led7', label: 'CH7', sub: 'GP6', x: 285, y: 379, w:  59, h:  66 },
        { id: 'led8', label: 'CH8', sub: 'GP7', x: 376, y: 379, w:  60, h:  66 },
        // Left side: 3 servo headers
        { id: 'srv1', label: 'SRV1', sub: 'GP8',  x:  10, y:  99, w:  60, h:  32 },
        { id: 'srv2', label: 'SRV2', sub: 'GP9',  x:  10, y: 141, w:  60, h:  32 },
        { id: 'srv3', label: 'SRV3', sub: 'GP10', x:  10, y: 183, w:  60, h:  32 },
        // Battery pads (− / +, combined)
        { id: 'batt', label: 'BAT',  sub: '+/−',  x:   6, y: 217, w:  57, h: 116 },
        // Right edge
        { id: 'usb',  label: 'USB',  sub: 'J1',   x: 628, y: 179, w:  78, h:  87 },
        { id: 'led_pwr',  label: 'LED1', sub: 'PWR', x: 660, y: 280, w:  46, h:  26 },
        { id: 'led_stat', label: 'LED2', sub: 'GP24/25', x: 648, y: 314, w:  58, h:  32 },
    ]

    // ─── Battery inference (mirror of LightFxTab logic). ───
    const CELL_NOMINAL_mV: Record<string, number> = { lipo: 3700, liion: 3700, nimh: 1200 }
    $: inferredCells = (() => {
        if (batteryVoltage_mV < 3000) return 0
        const nom = CELL_NOMINAL_mV[batteryChemistry] ?? 3700
        return Math.min(6, Math.max(1, Math.round(batteryVoltage_mV / nom)))
    })()
    $: effectiveCells = batteryCellCount > 0 ? batteryCellCount : inferredCells
    $: batteryLines = (() => {
        const chem = CHEMISTRY_LABEL[batteryChemistry] ?? batteryChemistry
        const cells = effectiveCells > 0 ? `${effectiveCells}S` : '—S'
        const v = batteryVoltage_mV > 0 ? `${(batteryVoltage_mV / 1000).toFixed(2)}V` : 'no volt'
        return [`${chem} · ${cells}`, v]
    })()

    function ledIdxFromId(id: string): number {
        const m = id.match(/^led(\d+)$/)
        return m ? parseInt(m[1]) - 1 : -1
    }

    function ledLines(idx: number): string[] {
        const st = ledLive[idx]
        if (!st) return ['—']
        if (!st.enabled)      return ['Disabled']
        if (st.seq)           return ['▶ Seq']
        if (st.bri > 0)       return [`${st.bri}%`]
        return ['0%']
    }

    function srvLines(id: string): string[] {
        if (id === 'srv1') return ['Servo']
        if (id === 'srv2') return ['Servo']
        if (id === 'srv3') {
            if (slaveMode)             return ['Servo']
            if (liveInput_us > 0)      return ['Input', `${liveInput_us} µs`]
            return ['Input', '— µs']
        }
        return []
    }

    function fixedLines(id: string): string[] {
        const lIdx = ledIdxFromId(id)
        if (lIdx >= 0) return ledLines(lIdx)
        if (id === 'batt')      return batteryLines
        if (id === 'usb')       return ['USB-C']
        if (id === 'led_pwr')   return ['Power']
        if (id === 'led_stat')  return ['Status']
        if (id.startsWith('srv')) return srvLines(id)
        return []
    }

    function isDisabled(id: string): boolean {
        const lIdx = ledIdxFromId(id)
        if (lIdx >= 0) return !(ledLive[lIdx]?.enabled ?? true)
        return false
    }

    function isActive(id: string): boolean {
        const lIdx = ledIdxFromId(id)
        if (lIdx >= 0) {
            const st = ledLive[lIdx]
            return !!st && st.enabled && (st.seq || st.bri > 0)
        }
        return false
    }

    function hotspotDescription(h: Hotspot): string {
        const lIdx = ledIdxFromId(h.id)
        if (lIdx >= 0) {
            const st = ledLive[lIdx]
            if (!st)           return `${h.label} · ${h.sub} — no data`
            if (!st.enabled)   return `${h.label} · ${h.sub} — disabled`
            if (st.seq)        return `${h.label} · ${h.sub} — sequence running`
            return `${h.label} · ${h.sub} — ${st.bri}% brightness`
        }
        if (h.id === 'batt') {
            const chem = CHEMISTRY_LABEL[batteryChemistry] ?? batteryChemistry
            const cellsSrc = batteryCellCount > 0 ? 'configured' : 'auto-detected'
            const cells = effectiveCells > 0 ? `${effectiveCells}S (${cellsSrc})` : '— cells'
            const v = batteryVoltage_mV > 0 ? `${(batteryVoltage_mV / 1000).toFixed(2)} V` : 'no reading'
            return `Battery feed (− / +) · GP29 / VSYS sense · ${chem} · ${cells} · ${v}`
        }
        if (h.id === 'srv1')      return `Servo 1 (GP8) — available for landing-light bindings`
        if (h.id === 'srv2')      return `Servo 2 (GP9) — available for landing-light bindings`
        if (h.id === 'srv3') {
            if (slaveMode)        return `Servo 3 (GP10) — available for landing-light bindings`
            const us = liveInput_us > 0 ? `${liveInput_us} µs` : '— µs'
            return `Servo 3 (GP10) — RC PWM "light input" in standalone mode · ${us}`
        }
        if (h.id === 'usb')       return `USB-C (J1) — data + power`
        if (h.id === 'led_pwr')   return `LED1 — power-rail indicator`
        if (h.id === 'led_stat')  return `LED2 — firmware status (CONN/ERR via GP24/GP25)`
        return h.label
    }

    let hoverId: string | null = null

    function close() {
        open = false
        onClose?.()
    }

    function handleKeydown(e: KeyboardEvent) {
        if (!open) return
        if (e.key === 'Escape') close()
    }

    // Label pill size in image-pixel units. The PNG is much smaller than
    // gearcontrol's, so labels are downsized to fit inside the hotspots
    // without overlapping silkscreen.
    const LABEL_H = 16
    const LABEL_CHAR_W = 7
    function labelWidth(s: string): number {
        return Math.max(s.length * LABEL_CHAR_W + 8, 28)
    }
    const FUNC_LINE_H = 14

    // ─── Silkscreen-style corner brackets ───
    // Each port is framed by 4 small "L"-shaped brackets at the corners,
    // mirroring the white silkscreen frames already drawn on the PCB. The
    // bracket arm length scales with the smaller side of the bbox so small
    // pads (LED indicators) don't get over-bracketed.
    function bracketPath(x: number, y: number, w: number, h: number): string {
        const arm = Math.max(6, Math.min(14, Math.min(w, h) * 0.35))
        const x1 = x + w
        const y1 = y + h
        return [
            // top-left
            `M ${x} ${y + arm} L ${x} ${y} L ${x + arm} ${y}`,
            // top-right
            `M ${x1 - arm} ${y} L ${x1} ${y} L ${x1} ${y + arm}`,
            // bottom-left
            `M ${x} ${y1 - arm} L ${x} ${y1} L ${x + arm} ${y1}`,
            // bottom-right
            `M ${x1 - arm} ${y1} L ${x1} ${y1} L ${x1} ${y1 - arm}`,
        ].join(' ')
    }
</script>

<svelte:window on:keydown={handleKeydown} />

{#if open}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="bv-backdrop" on:click|self={close}>
        <div class="bv-dialog">

            <header class="bv-header">
                <h2>LightFX — Board Layout {slaveMode ? '(Slave mode)' : '(Direct mode)'}</h2>
                <button class="bv-close" on:click={close} title="Close">✕</button>
            </header>

            <div class="bv-body">

                <div class="bv-image-wrap">
                    <svg class="bv-svg"
                         xmlns="http://www.w3.org/2000/svg"
                         viewBox="0 0 {IMG_W} {IMG_H}"
                         preserveAspectRatio="xMidYMid meet">
                        <image href={boardImage} x="0" y="0" width={IMG_W} height={IMG_H} />
                        {#each hotspots as h}
                            {@const lIdx = ledIdxFromId(h.id)}
                            {@const rawLines = fixedLines(h.id)}
                            {@const disabled = isDisabled(h.id)}
                            {@const active = isActive(h.id)}
                            <!-- svelte-ignore a11y-mouse-events-have-key-events -->
                            <g class="bv-hot"
                               class:bv-hot-active={hoverId === h.id}
                               class:bv-hot-disabled={disabled}
                               class:bv-hot-led-on={active && lIdx >= 0}
                               on:mouseenter={() => hoverId = h.id}
                               on:mouseleave={() => hoverId = null}
                               role="button" tabindex="0">
                                <title>{hotspotDescription(h)}</title>
                                <!-- Invisible hit-area so the whole bbox is clickable/hoverable. -->
                                <rect class="bv-hot-hit"
                                      x={h.x} y={h.y}
                                      width={h.w} height={h.h} />
                                <!-- Silkscreen-style corner brackets framing the port. -->
                                <path class="bv-hot-frame"
                                      d={bracketPath(h.x, h.y, h.w, h.h)} />
                                <rect class="bv-hot-pill"
                                      x={h.x + 2}
                                      y={h.y + 2}
                                      width={labelWidth(h.label)}
                                      height={LABEL_H}
                                      rx="2" ry="2" />
                                <text class="bv-hot-txt"
                                      x={h.x + 2 + labelWidth(h.label) / 2}
                                      y={h.y + 2 + LABEL_H / 2 + 4}
                                      text-anchor="middle">{h.label}</text>
                                <text class="bv-hot-sub"
                                      x={h.x + h.w - 4}
                                      y={h.y + 13}
                                      text-anchor="end">{h.sub}</text>
                                {#if rawLines.length}
                                    <text class="bv-hot-func"
                                          x={h.x + h.w / 2}
                                          y={h.y + 2 + LABEL_H + 14}
                                          text-anchor="middle">
                                        {#each rawLines as ln, i}
                                            <tspan x={h.x + h.w / 2} dy={i === 0 ? 0 : FUNC_LINE_H}>{ln}</tspan>
                                        {/each}
                                    </text>
                                {/if}
                            </g>
                        {/each}
                    </svg>
                </div>

            </div>

        </div>
    </div>
{/if}

<style>
    .bv-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.6);
        z-index: 160;
        display: flex;
        align-items: center;
        justify-content: center;
    }
    .bv-dialog {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 8px;
        box-shadow: 0 12px 48px var(--shadow);
        width: 1100px;
        max-width: 96vw;
        height: 82vh;
        max-height: 82vh;
        display: flex;
        flex-direction: column;
        color: var(--text);
    }
    .bv-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 12px 18px;
        border-bottom: 1px solid var(--border);
    }
    .bv-header h2 {
        margin: 0;
        font-size: 14px;
        font-weight: 600;
        color: var(--text-bright);
    }
    .bv-close {
        background: none;
        border: none;
        color: var(--text-dim);
        font-size: 16px;
        cursor: pointer;
        padding: 4px 10px;
        border-radius: 4px;
    }
    .bv-close:hover { color: var(--text); background: var(--bg-raised); }

    .bv-body {
        flex: 1 1 auto;
        min-height: 0;
        display: flex;
        padding: 12px 18px;
        overflow: hidden;
    }

    .bv-image-wrap {
        flex: 1;
        position: relative;
        display: flex;
        align-items: center;
        justify-content: center;
        background: var(--bg-base);
        border: 1px solid var(--border);
        border-radius: 4px;
        overflow: hidden;
        min-height: 0;
    }
    .bv-svg {
        width: 100%;
        height: 100%;
        display: block;
    }

    .bv-hot { cursor: pointer; }

    /* Transparent click-area covering the full bbox. The visible frame is the
       <path class="bv-hot-frame"> below — these brackets mimic the white
       silkscreen frames already drawn around each port on the PCB. */
    .bv-hot-hit {
        fill: transparent;
        stroke: none;
        pointer-events: all;
    }
    .bv-hot-frame {
        fill: none;
        stroke: #ffffff;
        stroke-width: 2;
        stroke-linecap: round;
        stroke-linejoin: round;
        paint-order: stroke;
        filter: drop-shadow(0 0 1.5px rgba(0, 0, 0, 0.85));
        pointer-events: none;
        transition: stroke 80ms ease, stroke-width 80ms ease;
    }
    .bv-hot-pill {
        fill: rgba(0, 0, 0, 0.78);
        stroke: #ffffff;
        stroke-width: 1;
        transition: fill 80ms ease, stroke 80ms ease;
    }
    .bv-hot-txt {
        fill: #ffffff;
        font-family: system-ui, -apple-system, sans-serif;
        font-size: 11px;
        font-weight: 700;
        letter-spacing: 0.3px;
        pointer-events: none;
    }
    .bv-hot-sub {
        fill: rgba(255, 255, 255, 0.92);
        font-family: system-ui, -apple-system, sans-serif;
        font-size: 9px;
        font-weight: 600;
        paint-order: stroke fill;
        stroke: rgba(0, 0, 0, 0.85);
        stroke-width: 2;
        stroke-linejoin: round;
        pointer-events: none;
    }
    .bv-hot-func {
        fill: #ffffff;
        font-family: system-ui, -apple-system, sans-serif;
        font-size: 11px;
        font-weight: 700;
        paint-order: stroke fill;
        stroke: rgba(0, 0, 0, 0.92);
        stroke-width: 3;
        stroke-linejoin: round;
        pointer-events: none;
    }

    .bv-hot:hover .bv-hot-frame,
    .bv-hot-active .bv-hot-frame {
        stroke: #ffeb3b;
        stroke-width: 3;
    }
    .bv-hot:hover .bv-hot-pill,
    .bv-hot-active .bv-hot-pill { fill: #ffeb3b; stroke: #ffeb3b; }
    .bv-hot:hover .bv-hot-txt,
    .bv-hot-active .bv-hot-txt { fill: #1a1a1a; }
    .bv-hot:hover .bv-hot-func,
    .bv-hot-active .bv-hot-func { fill: #ffeb3b; }

    /* LED channel that is lit/sequencing — brackets glow amber. */
    .bv-hot-led-on .bv-hot-frame {
        stroke: #ffca28;
        stroke-width: 2.5;
    }
    .bv-hot-led-on .bv-hot-pill { fill: #ffca28; stroke: #ffca28; }
    .bv-hot-led-on .bv-hot-txt  { fill: #1a1a1a; }

    /* Disabled channel — muted grey + dashed brackets. */
    .bv-hot-disabled .bv-hot-frame {
        stroke: #808080;
        stroke-dasharray: 4 3;
    }
    .bv-hot-disabled .bv-hot-pill { fill: rgba(0, 0, 0, 0.55); stroke: #808080; }
    .bv-hot-disabled .bv-hot-func { fill: #cccccc; }
    .bv-hot-disabled:hover .bv-hot-frame,
    .bv-hot-disabled.bv-hot-active .bv-hot-frame {
        stroke: #ffeb3b;
        stroke-dasharray: none;
    }
    .bv-hot-disabled:hover .bv-hot-pill,
    .bv-hot-disabled.bv-hot-active .bv-hot-pill { fill: #ffeb3b; stroke: #ffeb3b; }
</style>
