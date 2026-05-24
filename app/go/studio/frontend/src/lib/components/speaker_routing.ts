/**
 * speaker_routing.ts — single source of truth for the L / R / Stereo
 *   routing button used in every sound row across Studio.
 *
 *   Wire-format bitmask matches the firmware `AudioChannel` enum
 *   (CH1 = 0x01, CH2 = 0x02, CH1|CH2 = 0x03).  Stored as a number on
 *   every persisted-options field that selects routing (GunFx
 *   `RofItem.outputMask`, EngineFx future per-sound mask, …) so
 *   Studio + firmware never disagree on the encoding.
 *
 *   UX rules (Rule 34 design system):
 *     - Click cycles Stereo → Left → Right → Stereo
 *     - Button stays interactive even after the operator clears the
 *       sound path — routing is a property of the SLOT, not the file,
 *       so the operator can pre-pick where the next browse will land
 *     - The label always carries the routing name in WORDS (left /
 *       right / stereo) so the state is readable without hovering;
 *       the icon is a quick visual cue, the text removes ambiguity
 *     - Colour cue: stereo = neutral, left = warm, right = cool —
 *       matches the bar-legend colour-coding rule (Rule 36 §4)
 */

export const MASK_LEFT   = 0x01
export const MASK_RIGHT  = 0x02
export const MASK_STEREO = 0x03

/** Cycle: stereo → left → right → stereo.  Stereo is the rest state
 *  (most common case for a centred sound), tap once for left-only
 *  panning, tap twice for right-only. */
export function cycleOutputMask(curMask: number): number {
    if (curMask === MASK_STEREO) return MASK_LEFT
    if (curMask === MASK_LEFT)   return MASK_RIGHT
    return MASK_STEREO
}

/** Long-form label used in tooltips + `aria-label`. */
export function speakerLabel(mask: number): string {
    if (mask === MASK_LEFT)  return 'left'
    if (mask === MASK_RIGHT) return 'right'
    return 'stereo'
}

/** Short label rendered ON the button — fits inside the standard
 *  `.btn-slot` width so the speaker button column-aligns with browse
 *  / Clear in a sound row.  Operator decodes via shape + tooltip. */
export function routeShortLabel(mask: number): string {
    if (mask === MASK_LEFT)  return 'L'
    if (mask === MASK_RIGHT) return 'R'
    return 'L+R'
}

/** CSS class that the button carries so the design-system colour
 *  variables can paint it (`var(--warning)` for left, `var(--success)`
 *  for right, `var(--accent)` for stereo).  Single source of truth so
 *  both the inline button and any future status pill agree. */
export function speakerStateClass(mask: number): string {
    if (mask === MASK_LEFT)  return 'route-left'
    if (mask === MASK_RIGHT) return 'route-right'
    return 'route-stereo'
}

/** Inline SVG glyph (14×14) — speaker body always points left so the
 *  shape stays stable across states; wave arcs to the right turn off
 *  when the right channel is muted; a small L/R/LR tag confirms the
 *  state at a glance. */
export function speakerIcon(mask: number): string {
    const body = '<path d="M2 5h2.2L7 2.8v8.4L4.2 9H2z" fill="currentColor"/>'
    const wavesOn  =
        '<path d="M9 4.6c.9.6 1.4 1.4 1.4 2.4S9.9 8.8 9 9.4" stroke="currentColor" stroke-width="1.1" stroke-linecap="round" fill="none"/>' +
        '<path d="M10.7 3.4c1.4.9 2.2 2.2 2.2 3.6s-.8 2.7-2.2 3.6" stroke="currentColor" stroke-width="1.1" stroke-linecap="round" fill="none"/>'
    const wavesOff =
        '<path d="M9 4.6c.9.6 1.4 1.4 1.4 2.4S9.9 8.8 9 9.4" stroke="currentColor" stroke-width="1.1" stroke-linecap="round" fill="none" opacity="0.25"/>' +
        '<path d="M10.7 3.4c1.4.9 2.2 2.2 2.2 3.6s-.8 2.7-2.2 3.6" stroke="currentColor" stroke-width="1.1" stroke-linecap="round" fill="none" opacity="0.25"/>'
    if (mask === MASK_LEFT) {
        // Left only — kill right waves to show the channel is muted.
        return `<svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">${body}${wavesOff}</svg>`
    }
    if (mask === MASK_RIGHT) {
        // Right only — waves on, but the body alone (no extra mark) would
        // look identical to stereo at first glance, so we tint the body
        // via the parent button's colour class and keep the icon shape
        // consistent.  Text label on the button removes any ambiguity.
        return `<svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">${body}${wavesOn}</svg>`
    }
    return `<svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">${body}${wavesOn}</svg>`
}
