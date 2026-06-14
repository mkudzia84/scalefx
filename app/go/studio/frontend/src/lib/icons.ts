// icons — shared monochrome SVG icons (Feather/Lucide style, currentColor) so
// they tint with the surrounding text like the rest of the UI chrome.  Render
// via {@html svgIcon(PATHS, size, inlineStyle)}.

export function svgIcon(paths: string, size = 14, style = ''): string {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" ` +
        `stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"` +
        `${style ? ` style="${style}"` : ''}>${paths}</svg>`
}

// Setup Wizard — magic wand + sparkles (Lucide wand-2).
export const WAND_PATHS =
    '<path d="m21.64 3.64-1.28-1.28a1.21 1.21 0 0 0-1.72 0L2.36 18.64a1.21 1.21 0 0 0 0 1.72l1.28 1.28a1.2 1.2 0 0 0 1.72 0L21.64 5.36a1.2 1.2 0 0 0 0-1.72Z"/>' +
    '<path d="m14 7 3 3"/><path d="M5 6v4"/><path d="M19 14v4"/><path d="M10 2v2"/>' +
    '<path d="M7 8H3"/><path d="M21 16h-4"/><path d="M11 3H9"/>'
