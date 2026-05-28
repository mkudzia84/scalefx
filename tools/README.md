# tools/

Offline analysis utilities that don't belong inside a binary. Run from the
repo root with `python tools/<script>.py`.

## HubFX ESP32-S3 IDF-component dependency bootstrap

`setup-hubfx-idf-deps.ps1` populates `controllers/hubfx/esp32s3/managed_components/`
(~430 MB of ESP-IDF component sources — gitignored) on a fresh clone.
PowerShell-only (IDF tools refuse MSYS/MinGW shells).

```powershell
# Idempotent — ensures everything is downloaded + builds firmware.
.\tools\setup-hubfx-idf-deps.ps1

# Wipe + redownload (validate fresh-clone scenarios, recover from
# corrupted component cache).
.\tools\setup-hubfx-idf-deps.ps1 -Clean

# Download deps only, skip the link step.
.\tools\setup-hubfx-idf-deps.ps1 -SkipBuild
```

First invocation: 15-30 min (one-time toolchain + component download).
Subsequent runs from cache: 30-90 s.  Rationale + flag list in the
script header comment.  Background: `controllers/hubfx/esp32s3/`
switched to Arduino-as-IDF-component mode in `feature/idf-component-build`
to unlock `custom_sdkconfig` flags and esp-dsp.

## Board overlay calibration

Studio's "View Board" dialogs (e.g. [GearControlBoardDialog.svelte](../app/go/studio/frontend/src/lib/dialogs/GearControlBoardDialog.svelte))
draw interactive hotspots on top of a rendered PCB image. Hotspot rectangles
are in *native image pixel coordinates* so they stay pixel-perfect under
any scale. The coordinates come from measurement, not eyeballing.

### Workflow

1. Drop the 2D render into [media/](../media/) (e.g. `<board>_2d.png`) and
   copy it into `app/go/studio/frontend/src/assets/images/` so Vite bundles
   it into the binary.
2. Run the board-specific analyzer:
   ```bash
   python tools/analyze_gearcontrol_image.py
   ```
   It prints bounding boxes grouped by port zone, e.g.:
   ```
   ## J6/J10/J11 JSTs (magenta outline)
      x= 106 y= 448 w= 160 h= 123  area=   2174
      x= 318 y= 448 w= 160 h= 123  area=   2288
      x= 531 y= 448 w= 159 h= 123  area=   2056
   ```
3. Paste the values into the dialog's `hotspots[]` array and the header
   comment block. Rebuild the frontend:
   ```bash
   cd app/go/studio/frontend && npm run build
   cd .. && wails build
   ```

### Adding a new board

Copy `analyze_gearcontrol_image.py` → `analyze_<board>_image.py` and edit:

- `PNG` — path to the new render.
- `in_region(...)` filter rectangles in `main()` — rough pixel zones where
  each port group lives.
- If the PCB uses a silkscreen colour other than white/magenta, add a new
  `<colour>_mask()` function mirroring `magenta_mask()` and tune the RGB
  thresholds visually.

The detector itself (`white_mask`, `magenta_mask`, flood-fill `components`)
is reusable — only the zone rectangles and output groups are board-specific.

### Why two colour passes?

On the GearControl render the white flood-fill catches the JST connector
housings, but the actual port silhouette is drawn in magenta silkscreen
around the housing. The magenta pass is tighter and aligns the overlay
to the pad edges rather than the plastic body.

## Dependencies

```bash
python -m pip install --user Pillow
```

No other deps. The scripts are stdlib + Pillow.
