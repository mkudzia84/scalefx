// ScaleFX Studio — PCB overlay layout.
//
// Maps a board's ports to (x%, y%) positions on its top-side PCB photo so
// the schematic dialog can drop interactive markers on the real board.
// Coordinates are percentages of the image and come from measurement, not
// eyeballing: tools/analyze_hubfx.go detects the connector silkscreen /
// housings (white + magenta colour masks → flood-fill bounding boxes) and
// prints centroids; re-run it if a render is regenerated.

import hubfxTop from '../assets/pcb/hubfx_top.png'
import gearcontrolTop from '../assets/pcb/gearcontrol_top.png'
import expanderTop from '../assets/pcb/expander_top.png'
import { PortKind } from './devicemodel'

export interface PortMarker {
    kind: number
    index: number
    label: string   // silkscreen label (CH1, SRV1, IN1)
    x: number       // % of image width
    y: number       // % of image height
}
// Informational (non-port) markers — labelled board features that have no
// assignable role, e.g. the speaker outputs.  Rendered read-only.
export interface InfoMarker {
    label: string
    x: number
    y: number
    title: string
}
export interface BoardPcb {
    image: string
    markers: PortMarker[]
    info: InfoMarker[]
}

// ─── HubFX (769×969 top view) — measured centroids ────────────────────
// Left edge   : CH1..CH8  → PCA9685 LED/PWM channels (pwm 0..7), x≈7.7%.
// Right edge  : IN_12 (top) … IN_1 (bottom) header column, x≈94%.
//               IN_3..IN_12 are servo ports 0..9 (SRV1..SRV10); IN_2 + IN_1
//               are the two input ports (IN2 = Jeti EX Bus telemetry monitor
//               on UART2, idx 1; IN1 = main RC/Jeti, idx 0).  So top→bottom =
//               SRV10..SRV1, IN2, IN1.
const hubMarkers: PortMarker[] = []
{
    const chY = [11.1, 17.2, 23.3, 29.4, 35.5, 41.7, 47.8, 53.9] // CH1..CH8
    chY.forEach((y, i) => hubMarkers.push({ kind: PortKind.Pwm, index: i, label: `CH${i + 1}`, x: 7.7, y }))

    // Right edge servo column (rev B): SRV1 at the BOTTOM → SRV10 at the top,
    // even ~2.7% pitch.  x≈93%.
    const srvTopY = 15.5, srvPitch = 2.75
    for (let servoIdx = 0; servoIdx < 10; servoIdx++) {
        const y = srvTopY + (9 - servoIdx) * srvPitch   // SRV10 top, SRV1 bottom
        hubMarkers.push({ kind: PortKind.Servo, index: servoIdx, label: `SRV${servoIdx + 1}`, x: 93, y })
    }
    // Input headers sit at the TOP of the board (rev B), NOT the right column:
    //   INP   = IN_1 (UART1) main RC / channel input   (board silkscreen "INP")
    //   TELEM = IN_2 (UART2) telemetry monitor (ESC/Jeti EX Bus telemetry)
    // Show the same short labels the board uses so the overlay matches the PCB.
    hubMarkers.push({ kind: PortKind.Input, index: 0, label: 'INP', x: 40.5, y: 9 })
    hubMarkers.push({ kind: PortKind.Input, index: 1, label: 'TEL', x: 32.5, y: 9 })
}

// Audio (speaker) outputs — top edge, L + R; informational only.
const hubInfo: InfoMarker[] = [
    { label: 'SPK L', x: 74.4, y: 3.4, title: 'Speaker output — left (TAS5825P)' },
    { label: 'SPK R', x: 84.7, y: 3.4, title: 'Speaker output — right (TAS5825P)' },
]

// ─── GearControl (699×344 top view) — measured centroids ──────────────
// tools/analyze_gearcontrol.go: magenta mask → the 3 H-bridge screw
// terminals (J6/J10/J11, deliberately pink-highlighted in the render);
// yellow-pin mask → the 7 servo headers (even ~4.3% pitch on the right).
//   HB1..HB3 : x = 16.2 / 30.3 / 44.3 %, y = 69.5 %
//   SRV1..7  : x = 53.8 … 79.4 % (4.3% pitch), y = 72.5 %
// The board's own IN header (top) is NOT a hub-addressable port — the
// expander exposes only 7 servo + 3 hbridge to the master — so it's an
// informational marker, not a port.
const gearMarkers: PortMarker[] = []
{
    // 3 H-bridge motor screw terminals (J6/J10/J11), bottom-left, y≈69%.
    const hbX = [16.2, 30.3, 44.3]
    hbX.forEach((x, i) => gearMarkers.push({ kind: PortKind.HBridge, index: i, label: `HB${i + 1}`, x, y: 69 }))
    // Servo headers bottom-right.  NOTE: the current PCB rev has EIGHT physical
    // headers (Servo1..Servo8) on a ~5.6% pitch from x≈53% to x≈92%, but the
    // GearControl firmware only exposes SEVEN servo ports — so we mark the
    // first seven headers (Servo1..Servo7); the 8th header is unmapped until
    // the firmware grows an 8th servo port.
    const srvX = [53.0, 58.6, 64.1, 69.7, 75.3, 80.9, 86.4]
    srvX.forEach((x, i) => gearMarkers.push({ kind: PortKind.Servo, index: i, label: `SRV${i + 1}`, x, y: 72 }))
}
const gearInfo: InfoMarker[] = [
    // Large white RC-input header, top-left of the board (board-local — the
    // hub drives gear state over the wire, not through this header).
    { label: 'IN', x: 22, y: 9, title: 'RC input header (board-local — not driven by the hub)' },
]

// ─── PortExpander (768×418 top view) — eyeballed positions ────────────
// Coordinates are estimated from the render, not measured (no
// tools/analyze_expander.go yet) — refine when the analyzer is run.
// Board surface: 8 servo headers (Servo1..8, bottom row), 5 H-bridge
// motor outputs (CN3/CN4/CN5 white connectors top-left + 2 more
// bottom-left).  HB index ↔ connector mapping is PROVISIONAL pending
// netlist verification against the schematic.
// CH1/CH2 (top middle) are reserved board-local inputs, not
// hub-addressable ports — informational markers only.
const expanderMarkers: PortMarker[] = []
{
    // HB1..HB3 = CN3/CN4/CN5 top-left; HB4/HB5 = the two bottom-left outputs.
    expanderMarkers.push({ kind: PortKind.HBridge, index: 0, label: 'HB1', x: 42.3, y: 7.5 })
    expanderMarkers.push({ kind: PortKind.HBridge, index: 1, label: 'HB2', x: 30.6, y: 7.5 })
    expanderMarkers.push({ kind: PortKind.HBridge, index: 2, label: 'HB3', x: 19.5, y: 7.5 })
    expanderMarkers.push({ kind: PortKind.HBridge, index: 3, label: 'HB4', x: 14.3, y: 88.5 })
    expanderMarkers.push({ kind: PortKind.HBridge, index: 4, label: 'HB5', x: 24.1, y: 88.5 })
    // Servo1..8 — bottom row, even ~4.3% pitch.
    const srvX = [31.6, 35.9, 40.2, 44.5, 48.8, 53.1, 57.4, 61.7]
    srvX.forEach((x, i) => expanderMarkers.push({ kind: PortKind.Servo, index: i, label: `SRV${i + 1}`, x, y: 84 }))
}
const expanderInfo: InfoMarker[] = [
    { label: 'CH1', x: 55.5, y: 7.5, title: 'Reserved input channel (board-local — not driven by the hub)' },
    { label: 'CH2', x: 63.8, y: 7.5, title: 'Reserved input channel (board-local — not driven by the hub)' },
    { label: 'USB', x: 95.7, y: 46.5, title: 'USB-C — CDC wire to the HubFX master' },
]

export const boardPcb: Record<string, BoardPcb> = {
    hubfx: { image: hubfxTop, markers: hubMarkers, info: hubInfo },
    gearcontrol: { image: gearcontrolTop, markers: gearMarkers, info: gearInfo },
    portexpander: { image: expanderTop, markers: expanderMarkers, info: expanderInfo },
}

export function pcbFor(boardKind: string): BoardPcb | undefined {
    return boardPcb[boardKind]
}
