---
name: scalefx-flash
description: Build, flash, version-bump, and crash-debug ScaleFX firmware (HubFX ESP32-S3 / Pico controllers) with the scalefx-flash tool. Use when the user asks to build, flash, upload, reflash, bump the firmware version, decode a coredump/crash, or verify the on-device firmware.
---

# ScaleFX firmware build & flash

The tool is `app/go/scalefx-flash.exe` (build it first if missing: `cd app/go && go build -o scalefx-flash.exe ./flash/`). Run from the repo root.

## Controllers
Active targets: `hubfx` (ESP32-S3, the master — 99% of work), `lightfx` + `gearcontrol` (Pico generic expanders). The standalone `gunfx` Pico controller was removed (2026-06-06) — those effects now live only on the HubFX. `controllers` lists current targets.

## Core commands
```bash
app/go/scalefx-flash.exe build  hubfx --no-clean          # compile only (use --no-clean for fast incremental)
app/go/scalefx-flash.exe flash  hubfx --no-clean --port COM15   # build + flash + verify
app/go/scalefx-flash.exe upload hubfx --port COM15         # flash WITHOUT rebuilding (reuse last .bin)
app/go/scalefx-flash.exe verify --port COM15              # read back firmware version/build
app/go/scalefx-flash.exe ports                            # list detected ScaleFX serial ports
```
- `--port` auto-detects when omitted (HubFX = CH343 VID:PID `1A86:55D3`). The dev board is usually **COM15**.
- Prefer **`build`** when iterating on code you'll verify another way (e.g. Studio); **`flash`** when the user needs it on hardware. Only flash when the user asks for it on the board.
- Builds run in the background and take ~20–90 s — launch with `run_in_background: true` and wait for the completion notification rather than polling.

## Version & build number (Rules 9 / 10)
`FIRMWARE_VERSION` + `BUILD_NUMBER` live near the top of `controllers/hubfx/esp32s3/src/hubfx_esp32s3.cpp`.
- `BUILD_NUMBER` **auto-increments on every build** (a build hook bumps it) — don't hand-edit it; it'll show as a dirty one-line diff after a build, commit it as `chore(hubfx): bump build number`.
- Bump `FIRMWARE_VERSION` **proactively** by the change class: **MAJOR** = wire-breaking, **MINOR** = additive (new packet/field, append-only per Rule 11), **PATCH** = logic/bugfix only. Trust the `INIT_READY` buildNum over the source `#define` after a flash.

## After a protocol change
The Go side is the sync check. After editing any `controllers/lib/sfx_serial/serial/<mod>/<mod>.h`, run `cd app/go && go build ./cli/` (or `./...`) — it must compile, proving the `protocol/<mod>/<mod>.go` mirror matches (Rules 1, 2).

## Crash debugging — decode, don't guess
A firmware panic writes an ELF coredump to flash. Pull + decode in one shot **before reflashing** (espcoredump refuses on an ELF SHA mismatch):
```bash
app/go/scalefx-flash.exe coredump hubfx
```
Live panic text is on the **native USB-Serial-JTAG** port, not the CH343/UART0. Full guide: `instructions/24-COREDUMP-DEBUGGING.md`.

## esptool
ESP32-S3 needs esptool v5.2.0 (~12 MB, gitignored). Fetch via `app/go/scalefx-flash.exe tools download`; it's searched in `tools/esptool/`, next to the binary, then PATH.

## lightfx programs (HubFX effect data)
A freshly-flashed HubFX has no on-device LED programs. Seed them without reflashing:
```bash
app/go/scalefx-flash.exe programs --port COM15
```
This uploads `media/presets/lightfx/programs/*.yaml` → `/lightfx/programs/`.

## Gotchas
- Studio holds the serial port while connected — close/disconnect it before `flash`/`upload`/`verify`/integration tests, or the port is busy.
- The CLI is single-threaded and never trips wire-collision/keepalive hazards — **always validate wire changes against Studio too**, not just the CLI (Rules 53–56).
