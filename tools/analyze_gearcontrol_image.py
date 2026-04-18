# Port silkscreen detector for the GearControl 2D render.
#
# Finds the white silkscreen / connector housings AND magenta silkscreen
# outlines on the PCB and emits their bounding boxes in raw image pixel
# coordinates, so the studio board-overlay dialog can be calibrated from
# measurement rather than eyeballing.
#
# Usage:
#   python tools/analyze_gearcontrol_image.py
#
# Requirements:
#   python -m pip install --user Pillow
#
# Pipeline:
#   1. Build a binary mask per colour class:
#        white:   R,G,B all >= WHITE_MIN (covers white silkscreen + JST housings)
#        magenta: bright pink/purple silkscreen used around the gear JSTs
#   2. Flood-fill connected components via BFS (4-neighbour), filter by area.
#   3. Region-filter each component's centroid into the zones where a port
#      actually lives (IN header, gear JST strip, servo row, USB-C).
#   4. Print bboxes (x, y, w, h, area) ready to paste into the .svelte
#      hotspots[] array.
#
# How to adapt for a new board:
#   • Copy this file → analyze_<board>_image.py.
#   • Update PNG to point at media/<board>_2d.png.
#   • Replace the in_region() filter rectangles in main() with the rough
#     pixel zones where each port group lives on the new render.
#   • If the silkscreen markings use different colours, add a new mask
#     function mirroring magenta_mask() (R/G/B thresholds tuned visually).

from PIL import Image
import sys
from collections import deque

PNG = r"c:\data\code\scalefx\media\gearcontrol_2d.png"

# Treat "white" as bright pixels — covers both silkscreen and white JST housings.
WHITE_MIN = 215

def load(path):
    img = Image.open(path).convert("RGB")
    return img, img.size

def white_mask(img):
    w, h = img.size
    px = img.load()
    mask = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if r >= WHITE_MIN and g >= WHITE_MIN and b >= WHITE_MIN:
                mask[y * w + x] = 1
    return mask

def magenta_mask(img):
    """Bright magenta/pink silkscreen markers (R high, G low, B high)."""
    w, h = img.size
    px = img.load()
    mask = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if r >= 180 and b >= 140 and g <= 130 and (r + b) - 2 * g > 160:
                mask[y * w + x] = 1
    return mask

def grey_mask(img, lo=110, hi=210, max_chroma=22):
    """Neutral grey pixels — captures bare-copper / silver solder pads.
    All channels in [lo,hi] AND |R-G|, |G-B|, |R-B| all <= max_chroma."""
    w, h = img.size
    px = img.load()
    mask = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if (lo <= r <= hi and lo <= g <= hi and lo <= b <= hi
                    and abs(r - g) <= max_chroma
                    and abs(g - b) <= max_chroma
                    and abs(r - b) <= max_chroma):
                mask[y * w + x] = 1
    return mask

def union_bbox(boxes):
    """Tightest rectangle that contains every (x, y, w, h, area) in boxes."""
    if not boxes:
        return None
    x0 = min(b[0] for b in boxes)
    y0 = min(b[1] for b in boxes)
    x1 = max(b[0] + b[2] for b in boxes)
    y1 = max(b[1] + b[3] for b in boxes)
    return (x0, y0, x1 - x0, y1 - y0, sum(b[4] for b in boxes))

def components(mask, w, h, min_area=200):
    """Flood-fill white connected components and return their bboxes."""
    visited = bytearray(w * h)
    comps = []
    for sy in range(h):
        for sx in range(w):
            idx = sy * w + sx
            if not mask[idx] or visited[idx]:
                continue
            q = deque()
            q.append((sx, sy))
            visited[idx] = 1
            minx, miny, maxx, maxy, area = sx, sy, sx, sy, 0
            while q:
                x, y = q.popleft()
                area += 1
                if x < minx: minx = x
                if y < miny: miny = y
                if x > maxx: maxx = x
                if y > maxy: maxy = y
                for nx, ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
                    if 0 <= nx < w and 0 <= ny < h:
                        nidx = ny * w + nx
                        if mask[nidx] and not visited[nidx]:
                            visited[nidx] = 1
                            q.append((nx, ny))
            if area >= min_area:
                comps.append((minx, miny, maxx - minx + 1, maxy - miny + 1, area))
    return comps

def in_region(b, x0, y0, x1, y1):
    x, y, w, h, _ = b
    cx, cy = x + w/2, y + h/2
    return x0 <= cx <= x1 and y0 <= cy <= y1

def main():
    img, (W, H) = load(PNG)
    print(f"# image: {W}x{H}")
    print(f"# detecting white components (R,G,B all >= {WHITE_MIN}, area >= 200)...")
    mask = white_mask(img)
    comps = components(mask, W, H, min_area=200)
    print(f"# {len(comps)} components")

    # Sort by area descending for quick scan of the biggest silkscreen shapes.
    comps.sort(key=lambda b: -b[4])

    # Heuristic region filters — rough zones where each port group lives.
    # Anything outside these is noise (text, traces, vias).
    j_jst   = [b for b in comps if in_region(b,   60, 380,  720, 580) and b[2] > 120 and b[3] > 90]
    srv_row = [b for b in comps if in_region(b,  700, 420, 1280, 620) and b[2] > 40 and b[3] > 40]
    in_hdr  = [b for b in comps if in_region(b,  440,  20,  680, 140) and b[2] > 60 and b[3] > 30]
    usb_c   = [b for b in comps if in_region(b, 1300, 180, 1444, 420) and b[2] > 60 and b[3] > 60]
    batt    = [b for b in comps if in_region(b,   80,   0,  500, 150) and b[2] > 60 and b[3] > 60]

    def dump(name, group):
        print(f"\n## {name}  ({len(group)} candidates)")
        for b in sorted(group, key=lambda c: (c[1], c[0])):
            x, y, w, h, a = b
            print(f"   x={x:>4} y={y:>4} w={w:>4} h={h:>4}  area={a:>7}")

    dump("IN header region",    in_hdr)
    dump("J6 / J10 / J11 JSTs (white)", j_jst)
    dump("SRV1..SRV7 row",      srv_row)
    dump("J1 USB-C region",     usb_c)
    dump("Battery solder pads (top-left)", batt)

    # Top-20 everything for reference
    print("\n## top 20 components by area")
    for b in comps[:20]:
        x, y, w, h, a = b
        print(f"   x={x:>4} y={y:>4} w={w:>4} h={h:>4}  area={a:>7}")

    # ── Grey pads: detect the two round battery solder pads in the top-left ──
    print("\n# grey pass (silver/copper battery solder pads)...")
    gmask = grey_mask(img)
    gcomps = components(gmask, W, H, min_area=600)
    print(f"# {len(gcomps)} grey components")
    # Top-left quadrant only, and roughly circular (w/h within 1.6x of each other).
    bpads = [b for b in gcomps
             if in_region(b, 80, 0, 500, 160)
             and b[2] > 50 and b[3] > 50
             and 0.6 <= b[2] / b[3] <= 1.6]
    bpads.sort(key=lambda c: c[0])  # left-to-right
    dump("Battery pads (grey, individual)", bpads)
    bbox = union_bbox(bpads[:2]) if len(bpads) >= 2 else None
    if bbox:
        print(f"\n## Battery pads — single combined bbox")
        print(f"   x={bbox[0]:>4} y={bbox[1]:>4} w={bbox[2]:>4} h={bbox[3]:>4}  area={bbox[4]:>7}")
    else:
        print("\n## Battery pads — could not detect 2 candidates; tune grey_mask thresholds")

    # ── Magenta markers: re-detect the gear JST outlines using the pink silkscreen ──
    print("\n# magenta pass (pink silkscreen around gear JSTs)...")
    mmask = magenta_mask(img)
    mcomps = components(mmask, W, H, min_area=60)
    print(f"# {len(mcomps)} magenta components")
    jm = [b for b in mcomps if in_region(b, 60, 380, 720, 600) and b[2] > 80 and b[3] > 80]
    dump("J6/J10/J11 JSTs (magenta outline)", jm)
    print("\n## top 10 magenta components by area")
    mcomps.sort(key=lambda b: -b[4])
    for b in mcomps[:10]:
        x, y, w, h, a = b
        print(f"   x={x:>4} y={y:>4} w={w:>4} h={h:>4}  area={a:>7}")

if __name__ == "__main__":
    main()
