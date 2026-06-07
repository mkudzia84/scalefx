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

    // Right column, top→bottom (12 rows).  The headers sit on a denser
    // grid than the silkscreen labels the detector caught — ~3% apart, not
    // ~6% — so the column is anchored at the top row and compacted.
    const rightY = [9.2, 12.25, 15.3, 18.35, 21.4, 24.45, 27.5, 30.55, 33.6, 36.65, 39.7, 42.75]
    for (let i = 0; i < 10; i++) {
        const servoIdx = 9 - i                // top row = SRV10 (IN_12) … = servo[9]
        hubMarkers.push({ kind: PortKind.Servo, index: servoIdx, label: `SRV${servoIdx + 1}`, x: 94, y: rightY[i] })
    }
    // IN_2 (UART2 Jeti EX Bus telemetry monitor) then IN_1 (main channel).
    hubMarkers.push({ kind: PortKind.Input, index: 1, label: 'IN2', x: 94, y: rightY[10] })
    hubMarkers.push({ kind: PortKind.Input, index: 0, label: 'IN1', x: 94, y: rightY[11] })
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
    const hbX = [16.2, 30.3, 44.3]
    hbX.forEach((x, i) => gearMarkers.push({ kind: PortKind.HBridge, index: i, label: `HB${i + 1}`, x, y: 69.5 }))
    const srvX = [53.8, 58.1, 62.4, 66.7, 70.9, 75.1, 79.4]
    srvX.forEach((x, i) => gearMarkers.push({ kind: PortKind.Servo, index: i, label: `SRV${i + 1}`, x, y: 72.5 }))
}
const gearInfo: InfoMarker[] = [
    { label: 'IN', x: 40.5, y: 11.5, title: 'RC input header (board-local — not driven by the hub)' },
]

export const boardPcb: Record<string, BoardPcb> = {
    hubfx: { image: hubfxTop, markers: hubMarkers, info: hubInfo },
    gearcontrol: { image: gearcontrolTop, markers: gearMarkers, info: gearInfo },
}

export function pcbFor(boardKind: string): BoardPcb | undefined {
    return boardPcb[boardKind]
}
