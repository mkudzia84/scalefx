# Port silkscreen detector for the LightFX 2D render.
#
# Adapted from analyze_gearcontrol_image.py (see that file for the full pipeline
# explanation and adaptation guide). Same colour-mask + flood-fill logic, with
# region filters tuned to LightFX's layout:
#
#   • 8 LED JSTs in two rows of 4 (CH4-CH1 along the top, CH5-CH8 along the
#     bottom). Detected via the white silkscreen housing — they read as the
#     8 largest white components on the board edges.
#   • 3 servo headers stacked vertically on the left, each marked with a single
#     magenta "SIG" dot in the silkscreen.
#   • Two round battery solder pads (− and +) on the bottom-left, detected as
#     grey/silver annular pads.
#   • USB-C on the right edge (largest white component overall).
#
# Output coordinates are in the PNG's native pixel space (706 × 445) ready to
# paste into LightFxBoardDialog.svelte's hotspots[] array. The dialog renders
# each box as four white corner brackets (mimicking the PCB silkscreen frames),
# so the bbox should sit slightly OUTSIDE the connector body — that's what the
# detector returns by default (the silkscreen frame's outer edge).
#
# Usage:
#   python tools/analyze_lightfx_image.py

from PIL import Image
from collections import deque

PNG = r"c:\data\code\scalefx\media\lightfx_2d.png"

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
    """Bright magenta/pink silkscreen dots."""
    w, h = img.size
    px = img.load()
    mask = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if r >= 180 and b >= 140 and g <= 130 and (r + b) - 2 * g > 160:
                mask[y * w + x] = 1
    return mask


def grey_mask(img, lo=90, hi=215, max_chroma=28):
    """Neutral grey/silver pixels — battery solder pads."""
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
    if not boxes:
        return None
    x0 = min(b[0] for b in boxes)
    y0 = min(b[1] for b in boxes)
    x1 = max(b[0] + b[2] for b in boxes)
    y1 = max(b[1] + b[3] for b in boxes)
    return (x0, y0, x1 - x0, y1 - y0, sum(b[4] for b in boxes))


def components(mask, w, h, min_area=80):
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
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
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
    cx, cy = x + w / 2, y + h / 2
    return x0 <= cx <= x1 and y0 <= cy <= y1


def main():
    img, (W, H) = load(PNG)
    print(f"# image: {W}x{H}\n")

    print(f"# white pass (R,G,B all >= {WHITE_MIN})...")
    wmask = white_mask(img)
    wcomps = components(wmask, W, H, min_area=200)
    print(f"# {len(wcomps)} white components\n")

    # ── LED JSTs: 8 large white housings (~60x66) at top/bottom edges. ──
    led_housings = [b for b in wcomps if b[2] >= 50 and b[2] <= 80
                    and b[3] >= 50 and b[3] <= 80
                    and (b[1] <= 10 or b[1] + b[3] >= H - 5)]

    top_row = [b for b in led_housings if b[1] <= 10]
    bot_row = [b for b in led_housings if b[1] + b[3] >= H - 5]
    top_row.sort(key=lambda b: b[0])  # left -> right
    bot_row.sort(key=lambda b: b[0])

    # Visually labelled left->right: top row = CH4 CH3 CH2 CH1,
    # bottom row = CH5 CH6 CH7 CH8.
    top_labels = ["CH4", "CH3", "CH2", "CH1"]
    bot_labels = ["CH5", "CH6", "CH7", "CH8"]

    print("## LED JSTs — top row (CH4..CH1)")
    for lbl, b in zip(top_labels, top_row):
        x, y, w, h, _ = b
        print(f"   {lbl:>4}: x={x:>4} y={y:>4} w={w:>4} h={h:>4}")

    print("\n## LED JSTs — bottom row (CH5..CH8)")
    for lbl, b in zip(bot_labels, bot_row):
        x, y, w, h, _ = b
        print(f"   {lbl:>4}: x={x:>4} y={y:>4} w={w:>4} h={h:>4}")

    # ── USB-C: largest white component on the right edge. ──
    usb = [b for b in wcomps if in_region(b, 580, 130, 706, 280)
           and b[2] >= 60 and b[3] >= 70]
    if usb:
        usb.sort(key=lambda c: -c[4])
        x, y, w, h, _ = usb[0]
        print(f"\n## USB-C  (J1)")
        print(f"   USB: x={x:>4} y={y:>4} w={w:>4} h={h:>4}")

    # ── Servo headers: 3 magenta "SIG" dots stacked on the left edge. ──
    print("\n# magenta pass...")
    mmask = magenta_mask(img)
    mcomps = components(mmask, W, H, min_area=8)
    print(f"# {len(mcomps)} magenta components")

    left_dots = [b for b in mcomps if in_region(b, 0, 60, 75, 230)]
    print(f"# left-band magenta dots ({len(left_dots)}):")
    for b in sorted(left_dots, key=lambda c: c[1]):
        x, y, w, h, a = b
        print(f"   x={x:>4} y={y:>4} w={w:>4} h={h:>4} area={a}")
    sig_dots = [b for b in left_dots if 4 <= b[2] <= 30 and 4 <= b[3] <= 30]
    sig_dots.sort(key=lambda b: b[1])  # top -> bottom
    print("\n## Servo headers — Servo1..Servo3 (3 magenta SIG dots)")
    # Each 3-pin servo header is ~52 px wide (3 pins × ~17 px) and ~28 px tall.
    # The SIG dot sits in the silkscreen LEFT of the leftmost pin, so we
    # extend the box rightwards from the dot.
    for i, b in enumerate(sig_dots[:3]):
        x, y, w, h, _ = b
        bx = max(0, x - 4)
        by = max(0, y - 12)
        bw = 60
        bh = 32
        print(f"   Servo{i + 1}: x={bx:>4} y={by:>4} w={bw:>4} h={bh:>4}")

    # ── Battery pads: 2 round silver/grey pads on the lower-left. ──
    print("\n# grey pass (battery solder pads)...")
    gmask = grey_mask(img)
    gcomps = components(gmask, W, H, min_area=200)
    region_grey = [b for b in gcomps if in_region(b, 0, 230, 110, 420)]
    print(f"# grey components in lower-left ({len(region_grey)}):")
    for b in sorted(region_grey, key=lambda c: c[1]):
        x, y, w, h, a = b
        print(f"   x={x:>4} y={y:>4} w={w:>4} h={h:>4} area={a}")
    bpads = [b for b in gcomps
             if in_region(b, 0, 230, 110, 420)
             and 25 <= b[2] <= 80 and 25 <= b[3] <= 80
             and 0.5 <= b[2] / b[3] <= 2.0]
    bpads.sort(key=lambda b: b[1])
    print(f"# battery pad candidates: {len(bpads)}")
    for i, b in enumerate(bpads[:4]):
        x, y, w, h, a = b
        print(f"   pad{i + 1}: x={x:>4} y={y:>4} w={w:>4} h={h:>4} area={a}")

    if len(bpads) >= 2:
        bbox = union_bbox(bpads[:2])
        x, y, w, h, _ = bbox
        pad = 6
        print(f"\n## Battery pads — combined")
        print(f"   BAT: x={x - pad:>4} y={y - pad:>4} "
              f"w={w + 2 * pad:>4} h={h + 2 * pad:>4}")
    elif len(bpads) == 1:
        # Only one pad detected — extrapolate the second below it (annular pads
        # on this PCB are spaced ~50 px on Y).
        x, y, w, h, _ = bpads[0]
        print(f"\n## Battery pads — fallback (single pad, extrapolated)")
        print(f"   BAT: x={x - 6:>4} y={y - 6:>4} w={w + 12:>4} h={h * 2 + 30:>4}")

    # ── Status LED indicators (LED1, LED2): right-edge magenta-marker pairs. ──
    led_pairs = [b for b in mcomps if in_region(b, 640, 285, 706, 380)
                 and b[2] >= 4 and b[3] >= 2]
    led_pairs.sort(key=lambda b: b[1])
    if len(led_pairs) >= 4:
        # Top pair = LED1 (POWER), bottom pair = LED2 (status).
        led1 = union_bbox(led_pairs[:2])
        led2 = union_bbox(led_pairs[2:4])
        for name, b in (("LED1", led1), ("LED2", led2)):
            x, y, w, h, _ = b
            pad_x = 8
            pad_y = 6
            print(f"\n## {name}")
            print(f"   {name}: x={x - pad_x:>4} y={y - pad_y:>4} "
                  f"w={w + 2 * pad_x:>4} h={h + 2 * pad_y:>4}")


if __name__ == "__main__":
    main()
