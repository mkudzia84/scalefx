<!-- ScaleFX Studio — GearControl board layout viewer. -->
<!-- Uses the native PNG resolution (1444 × 679) as the SVG viewBox so -->
<!-- every hotspot rectangle is positioned in raw image pixel coordinates. -->
<!-- The SVG scales proportionally to fit the dialog without distorting hits. -->
<!--
    Pixel-coord analysis (image = 1444 × 679).
    Bounding boxes are measured directly from silkscreen pixels via
    tools/analyze_gearcontrol_image.py — NOT eyeballed. Re-run the tool
    if the render is regenerated.

    J6/J10/J11 use the MAGENTA silkscreen outline (not the white JST
    housing), since the pink marker is the actual port edge.

    BAT (battery feed) is a single box covering BOTH grey solder pads
    in the top-left, measured from the GREY pixel pass (bare-copper
    pad detection — see grey_mask() in the analyzer).

      BAT    (− and + solder pads, combined)     x= 191  y=  21  w=255  h=106
      IN     (gear input 3-pin header, top)      x= 492  y=  39  w=174  h= 65
      J6     (gear JST, left) — magenta          x= 106  y= 448  w=160  h=123
      J10    (gear JST, center-left) — magenta  x= 318  y= 448  w=160  h=123
      J11    (gear JST, center) — magenta        x= 531  y= 448  w=159  h=123
      SRV1   (servo header #1)                   x= 719  y= 454  w= 64  h=175
      SRV2   (servo header #2)                   x= 788  y= 454  w= 59  h=175
      SRV3   (servo header #3)                   x= 852  y= 454  w= 58  h=175
      SRV4   (servo header #4)                   x= 916  y= 454  w= 58  h=175
      SRV5   (servo header #5)                   x= 979  y= 454  w= 59  h=175
      SRV6   (servo header #6)                  x=1043  y= 454  w= 58  h=175
      SRV7   (servo header #7)                  x=1107  y= 454  w= 58  h=175
      J1     (USB-C)                            x=1289  y= 268  w=137  h=144
-->
<script lang="ts">
    import boardImage from '../../assets/images/gearcontrol_2d.png'

    type PinRole = 'door' | 'yaw_input' | 'yaw_output' | 'unused'
    interface PinCfg {
        role: PinRole
        channel: number
        gear_id: number
    }

    export let open: boolean = false
    export let onClose: (() => void) | null = null
    /** Seven SRV pin configs in order SRV1..SRV7. */
    export let pinConfigs: PinCfg[] = []
    export let pinSlots: string[] = []
    export let gearNames: string[] = ['Nose', 'Left Main', 'Right Main']
    /** Per-gear enable flag, indexed 0=nose, 1=left main, 2=right main. */
    export let gearEnabled: boolean[] = [true, true, true]
    export let gearInputEnabled: boolean = false
    export let isHubFX: boolean = false

    // ─── Battery status (shown on the BATT+ pad). ───
    export let batteryChemistry: string = 'lipo'
    export let batteryCellCount: number = 0   // 0 when auto-inferred from voltage
    export let batteryVoltage_mV: number = 0
    const CHEMISTRY_LABEL: Record<string, string> = { lipo: 'LiPo', liion: 'Li-Ion', nimh: 'NiMH' }

    // ─── Image native dimensions (must match the PNG). ───
    const IMG_W = 1444
    const IMG_H = 679

    interface Hotspot {
        id: string
        label: string
        /** Pixel coordinates in the native 1444 × 679 image. */
        x: number; y: number; w: number; h: number
    }

    const hotspots: Hotspot[] = [
        { id: 'batt', label: 'BAT',   x:  191, y:  21, w: 255, h: 106 },
        { id: 'in',   label: 'IN',    x:  492, y:  39, w: 174, h:  65 },
        { id: 'j6',   label: 'J6',    x:  106, y: 448, w: 160, h: 123 },
        { id: 'j10',  label: 'J10',   x:  318, y: 448, w: 160, h: 123 },
        { id: 'j11',  label: 'J11',   x:  531, y: 448, w: 159, h: 123 },
        { id: 'srv1', label: 'SRV1',  x:  719, y: 454, w:  64, h: 175 },
        { id: 'srv2', label: 'SRV2',  x:  788, y: 454, w:  59, h: 175 },
        { id: 'srv3', label: 'SRV3',  x:  852, y: 454, w:  58, h: 175 },
        { id: 'srv4', label: 'SRV4',  x:  916, y: 454, w:  58, h: 175 },
        { id: 'srv5', label: 'SRV5',  x:  979, y: 454, w:  59, h: 175 },
        { id: 'srv6', label: 'SRV6',  x: 1043, y: 454, w:  58, h: 175 },
        { id: 'srv7', label: 'SRV7',  x: 1107, y: 454, w:  58, h: 175 },
        { id: 'usb',  label: 'J1',    x: 1289, y: 268, w: 137, h: 144 },
    ]

    // ─── Battery inference (mirror of GearControlTab) ───
    // Nominal per-cell mV for cell-count inference when batteryCellCount=0.
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

    function roleLabel(role: PinRole, channel: number, gearId: number): string {
        if (role === 'door')       return `Door · ${gearNames[channel] ?? `Gear ${channel}`}`
        if (role === 'yaw_input')  return 'Yaw Input (PWM)'
        if (role === 'yaw_output') return `Yaw Output · ${gearNames[gearId] ?? `Gear ${gearId}`}`
        return 'Unused'
    }

    // Compact gear name for narrow servo columns.
    function shortGear(idx: number): string {
        const n = gearNames[idx] ?? `G${idx}`
        return n.replace(/^Left /, 'L ').replace(/^Right /, 'R ')
    }

    // Multi-line function label for a servo column.
    function servoLines(idx: number): string[] {
        const p = pinConfigs[idx]
        if (!p) return ['Unused']
        if (p.role === 'door')       return ['Door', shortGear(p.channel)]
        if (p.role === 'yaw_input')  return ['Yaw', 'Input']
        if (p.role === 'yaw_output') return ['Yaw Out', shortGear(p.gear_id)]
        return ['Unused']
    }

    function servoIdxFromId(id: string): number {
        const m = id.match(/^srv(\d)$/)
        return m ? parseInt(m[1]) - 1 : -1
    }

    $: servoFunction = (id: string): string => {
        const idx = servoIdxFromId(id)
        if (idx < 0 || !pinConfigs[idx]) return ''
        const p = pinConfigs[idx]
        return roleLabel(p.role, p.channel, p.gear_id)
    }

    // Multi-line function label for non-servo hotspots.
    function fixedLines(id: string): string[] {
        if (id === 'batt') return batteryLines
        if (id === 'in')   return [gearInputEnabled ? 'PWM in' : 'Disabled']
        if (id === 'j6')   return ['Nose']
        if (id === 'j10')  return ['L Main']
        if (id === 'j11')  return ['R Main']
        if (id === 'usb')  return ['USB-C']
        return []
    }

    // true if the hotspot is bound to a disabled gear / role (rendered grey).
    function isUnused(id: string): boolean {
        const sIdx = servoIdxFromId(id)
        if (sIdx < 0) return false
        const p = pinConfigs[sIdx]
        return !p || p.role === 'unused'
    }

    // true if this hotspot maps to a currently-disabled gear channel / input.
    // Servo slots bound via role=door/yaw_output take the gear enable; unused
    // / yaw_input slots are treated as neutral (not disabled). Power (J1) is
    // always enabled.
    function isDisabled(id: string): boolean {
        if (id === 'in')  return !gearInputEnabled
        if (id === 'j6')  return !gearEnabled[0]
        if (id === 'j10') return !gearEnabled[1]
        if (id === 'j11') return !gearEnabled[2]
        if (id === 'usb') return false
        const sIdx = servoIdxFromId(id)
        if (sIdx < 0) return false
        const p = pinConfigs[sIdx]
        if (!p) return false
        if (p.role === 'door')       return !gearEnabled[p.channel]
        if (p.role === 'yaw_output') return !gearEnabled[p.gear_id]
        return false
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

    function hotspotDescription(h: Hotspot): string {
        if (h.id.startsWith('srv')) {
            const idx = servoIdxFromId(h.id)
            const slot = pinSlots[idx] ?? h.label
            return `${slot} — ${servoFunction(h.id) || 'Unused'}`
        }
        if (h.id === 'batt') {
            const chem = CHEMISTRY_LABEL[batteryChemistry] ?? batteryChemistry
            const cellsSrc = batteryCellCount > 0 ? 'configured' : 'auto-detected'
            const cells = effectiveCells > 0 ? `${effectiveCells}S (${cellsSrc})` : '— cells'
            const v = batteryVoltage_mV > 0 ? `${(batteryVoltage_mV / 1000).toFixed(2)} V` : 'no reading'
            return `Battery feed (− / +) · ${chem} · ${cells} · ${v}`
        }
        if (h.id === 'in')  return `Gear Input (GP0) — receiver PWM · ${gearInputEnabled ? 'enabled' : 'disabled'}`
        if (h.id === 'j6')  return 'J6 — current/battery sense (nose)'
        if (h.id === 'j10') return 'J10 — current/battery sense (left main)'
        if (h.id === 'j11') return 'J11 — current/battery sense (right main)'
        if (h.id === 'usb') return 'J1 — USB-C · data + power'
        return h.label
    }

    // Label pill size (in image-pixel units).
    const LABEL_H = 28
    const LABEL_CHAR_W = 11  // average glyph width for font-size:18 bold
    function labelWidth(s: string): number {
        return Math.max(s.length * LABEL_CHAR_W + 12, 40)
    }
    const FUNC_LINE_H = 22
</script>

<svelte:window on:keydown={handleKeydown} />

{#if open}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="bv-backdrop" on:click|self={close}>
        <div class="bv-dialog">

            <header class="bv-header">
                <h2>GearControl — Board Layout {isHubFX ? '(Slave mode)' : '(Direct mode)'}</h2>
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
                            {@const sIdx = servoIdxFromId(h.id)}
                            {@const slotLabel = sIdx >= 0 ? (pinSlots[sIdx] ?? h.label) : h.label}
                            {@const rawLines = sIdx >= 0 ? servoLines(sIdx) : fixedLines(h.id)}
                            {@const disabled = isDisabled(h.id)}
                            {@const unused = isUnused(h.id)}
                            {@const lines = disabled ? [...rawLines, '(off)'] : rawLines}
                            <!-- svelte-ignore a11y-mouse-events-have-key-events -->
                            <g class="bv-hot"
                               class:bv-hot-active={hoverId === h.id}
                               class:bv-hot-disabled={disabled}
                               class:bv-hot-unused={unused}
                               on:mouseenter={() => hoverId = h.id}
                               on:mouseleave={() => hoverId = null}
                               role="button" tabindex="0">
                                <title>{hotspotDescription(h)}</title>
                                <rect class="bv-hot-rect"
                                      x={h.x} y={h.y}
                                      width={h.w} height={h.h}
                                      rx="3" ry="3" />
                                <rect class="bv-hot-pill"
                                      x={h.x + 3}
                                      y={h.y + 3}
                                      width={labelWidth(slotLabel)}
                                      height={LABEL_H}
                                      rx="3" ry="3" />
                                <text class="bv-hot-txt"
                                      x={h.x + 3 + labelWidth(slotLabel) / 2}
                                      y={h.y + 3 + LABEL_H / 2 + 6}
                                      text-anchor="middle">{slotLabel}</text>
                                {#if lines.length}
                                    <text class="bv-hot-func"
                                          x={h.x + h.w / 2}
                                          y={h.y + 3 + LABEL_H + 20}
                                          text-anchor="middle">
                                        {#each lines as ln, i}
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

    .bv-hot {
        cursor: pointer;
    }
    .bv-hot-rect {
        fill: rgba(255, 59, 59, 0.18);
        stroke: #ff3b3b;
        stroke-width: 3;
        transition: fill 80ms ease, stroke 80ms ease;
    }
    .bv-hot-pill {
        fill: #ff3b3b;
        transition: fill 80ms ease;
    }
    .bv-hot-txt {
        fill: #ffffff;
        font-family: system-ui, -apple-system, sans-serif;
        font-size: 18px;
        font-weight: 700;
        letter-spacing: 0.4px;
        pointer-events: none;
    }
    .bv-hot:hover .bv-hot-rect,
    .bv-hot-active .bv-hot-rect {
        fill: rgba(255, 59, 59, 0.38);
        stroke: #ffeb3b;
        stroke-width: 4;
    }
    .bv-hot:hover .bv-hot-pill,
    .bv-hot-active .bv-hot-pill {
        fill: #ffeb3b;
    }
    .bv-hot:hover .bv-hot-txt,
    .bv-hot-active .bv-hot-txt {
        fill: #1a1a1a;
    }

    .bv-hot-func {
        fill: #ffffff;
        font-family: system-ui, -apple-system, sans-serif;
        font-size: 18px;
        font-weight: 700;
        paint-order: stroke fill;
        stroke: rgba(0, 0, 0, 0.92);
        stroke-width: 5;
        stroke-linejoin: round;
        pointer-events: none;
    }
    .bv-hot:hover .bv-hot-func,
    .bv-hot-active .bv-hot-func {
        fill: #ffeb3b;
    }

    /* Disabled-channel styling: swap red → muted grey so the overlay clearly
       reflects gears that are turned off at the firmware level. */
    .bv-hot-disabled .bv-hot-rect {
        fill: rgba(120, 120, 120, 0.18);
        stroke: #808080;
        stroke-dasharray: 6 4;
    }
    .bv-hot-disabled .bv-hot-pill {
        fill: #808080;
    }

    /* Unused slot styling: very muted, thin stroke — visible but unobtrusive. */
    .bv-hot-unused .bv-hot-rect {
        fill: rgba(120, 120, 120, 0.10);
        stroke: #9a9a9a;
        stroke-width: 2;
        stroke-dasharray: 3 4;
    }
    .bv-hot-unused .bv-hot-pill {
        fill: #5a5a5a;
    }
    .bv-hot-unused .bv-hot-func {
        fill: #cccccc;
    }
    .bv-hot-disabled:hover .bv-hot-rect,
    .bv-hot-disabled.bv-hot-active .bv-hot-rect {
        fill: rgba(120, 120, 120, 0.32);
        stroke: #ffeb3b;
    }
    .bv-hot-disabled:hover .bv-hot-pill,
    .bv-hot-disabled.bv-hot-active .bv-hot-pill {
        fill: #ffeb3b;
    }
</style>
