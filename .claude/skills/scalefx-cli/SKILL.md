---
name: scalefx-cli
description: Drive a connected ScaleFX board from the scalefx-cli tool — run board commands, subscribe to live telemetry/diagnostics, inspect topology/ports/config, attach + drive roles. Use when the user asks to run a CLI command, query the board, subscribe to events, bench-test an expander's roles, or reproduce a wire issue on the CLI.
---

# ScaleFX CLI

`app/go/scalefx-cli.exe` (build: `cd app/go && go build -o scalefx-cli.exe ./cli/`). Single-threaded — the safe place to reproduce wire behaviour without Studio's concurrency.

## Run commands

```bash
app/go/scalefx-cli.exe -p COM15 -c "<command>"     # one-shot: run one command, exit
app/go/scalefx-cli.exe -p COM15                     # interactive REPL
```

Flags: `-p <port>` (open at start), `-b <baud>` (0 = default 6 Mbps), `-v` (verbose wire logging), `-c "<cmd>"` (one-shot), `-no-color`.

**Run a script (many commands, one connection):** `-c` is single-command, but the REPL reads stdin line-by-line — pipe a script and it runs each line on one connection (closes on EOF):

```bash
{ echo init; for i in 0 1 2 3 4 5 6 7; do echo "role-attach-local pwm $i led-animator"; echo "led-on $i 100"; done; echo role-list-local; } \
  | app/go/scalefx-cli.exe -p COM20
```

## Command names

Commands are **flat, hyphenated names** grouped by category (`gear-reset`, `light-select`, `led-on`) — there is no colon-prefix dispatch in this CLI. `help` lists everything; `help <cmd>` describes one.

**GUID-transparent role commands** (`servo-set`, `led-*`, `bimotor-*`, `servo-profile-*`, `motor-element-*`, `heater-element-*`, `role-attach`/`role-detach`, `topo-*`): append `guid=XXXX` to target an expander **through the hub**; omit it (or `hub`) for the hub itself. The GUID is a keyword, never positional (a GUID like `3225` is indistinguishable from a numeric arg).

**`-local` role commands** (`role-attach-local`, `role-detach-local`, `role-list-local`, `led-*`) drive the **directly-connected** board with **no hub** — the bench-test path for an expander on its own USB port (e.g. a LightFX/GearControl Pico).

## Command reference

### Session
- `connect <port>` — open a serial port
- `disconnect` — close the current port
- `ports` — list attached serial ports
- `verbose <on|off>` — toggle wire-level packet logging
- `help [cmd]` — list commands or describe one
- `quit` — exit the CLI

### Core / lifecycle
- `status` — print STATUS payload
- `init [mode] [flags]` — send INIT (default: SLAVE, flags=0)
- `identify` — print hub/board identity
- `capabilities` — list capability bits
- `reboot` — reboot the device
- `bootsel` — enter bootloader (Pico only)
- `keepalive` — send one KEEPALIVE packet
- `i2c-scan` — scan the I²C bus
- `diag [max]` — dump the device's diagnostic ring buffer (max entries)
- `diag-clear` — drop the local console replay (does NOT clear the device buffer)
- `rc-routing [on|off]` — global RC→effect routing gate (no arg = show)
- `subscribe` — stream async events + firmware `[LOG I]` diagnostics until disconnect (see below)

### Topology, ports & roles
Inspect:
- `system-info` — hub + all expanders in one round-trip
- `expanders` — list connected expanders
- `topo-ports [guid]` — list ports for a board (default: all)
- `topo-roles [guid]` — list roles for a board (default: all)
- `topo-query <guid|hub> <reqTypeHex> [payloadHex]` — generic GUID-routed role QUERY (request→typed RESP), e.g. `topo-query 3225 49 00` = SERVO_GET_STATUS_REQ on servo 0

Role lifecycle:
- `role-attach <guid> <portKind> <portIdx> <roleKind> [hex-cfg]` — bind a role on a board (via the hub)
- `role-detach <guid> <portKind> <portIdx>` — detach a role (via the hub)
- `role-attach-local <portKind> <portIdx> <roleKind> [hexcfg]` — attach DIRECTLY on the connected board, no hub. portKind=`servo|pwm|hbridge|input`, roleKind=`servo|bi-dc-motor|dc-motor|heater|led-animator`
- `role-detach-local <portKind> <portIdx>` — detach on the connected board
- `role-list-local` — list roles attached on the connected board

Drive — servo:
- `servo-set <portIdx> <targetUs> [guid=XXXX]` — drive a servo to targetUs (intent; the role's motion profile shapes the slew)
- `servo-profiles` — dump the LIVE motion profile of every attached hub servo (spot a stale/clamped calibration)
- `servo-profile-get <portIdx> [guid=XXXX]` — read a servo's motion profile
- `servo-profile-set <portIdx> <key=val> …` — push min/max/center/max_speed/max_accel/max_jerk/reversed

Drive — LED animator:
- `led-on <portIdx> [pct] [guid=XXXX]` — light a channel SOLID at pct% (default 100; queues constant EV_ON + starts)
- `led-off <portIdx> [guid=XXXX]` — stop a channel (output low)
- `led-brightness <portIdx> <pct> [guid=XXXX]` — set master brightness (0..100) without changing the program

Drive — DC motor / heater (element scaling, Rule 42):
- `motor-element-get <portIdx>` / `motor-element-set <portIdx> <key=val> …` — read/push element_mv + scaling (passthrough|linear|quadratic)
- `heater-element-get <portIdx>` / `heater-element-set <portIdx> <key=val> …` — read/push element_mv / scaling / drive_pct / hyst_cx10

Drive — bidirectional DC motor (gear/door BiDcMotor):
- `bimotor-move-end <portIdx> <a|b> [duty] [timeoutMs] [guid=XXXX]` — drive to logical endstop A (+duty) or B (−duty); outcome async (`subscribe`)
- `bimotor-seek <portIdx> <signedDuty> [timeoutMs] [guid=XXXX]` — position-agnostic seek until stall/endstop/timeout
- `bimotor-status <portIdx> [guid=XXXX]` — duty, voltage, current, stalled, position (A/B), guard mode
- `bimotor-guard <portIdx> <live|fixed> [ratioX|thresholdMa] [windowMs] [ceilingMa]` — retune stall guard (live trailing-min ratio e.g. `live 2.5`, or fixed mA)

### Storage (file ops)
- `target [sd|flash]` — switch active backend (no arg = print current)
- `pwd` — print active target + working directory
- `cd [path]` — change working directory (no arg = /)
- `ls [-f] [path]` — list a directory (`-f` forces flash)
- `tree [-f] [path]` — recursive listing
- `stat [-f] <path>` — metadata for a path
- `cat [-f] <path>` — print a small text file
- `df` — disk usage for flash + SD
- `mkdir [-p] [-f] <path>` — create a directory (`-p` makes parents)
- `rm [-r] [-f] <path>` — remove a file (`-r` recursive)
- `download [-f] <remote> [local]` — download a remote file
- `upload [-m sync|stream] [-f] <local> [remote]` — upload a local file (default mode = stream on ESP32)
- `upload-diag` — post-mortem of the last upload (SD write latencies, abort reason)
- `sd-init [speed_mhz]` — (re-)initialise the SD card driver

### Config
- `config-status` — aggregate config-store state (loaded, file size, validate ok)
- `config-reload [path]` — re-read /hubfx.yaml (and friends); fan apply to every service
- `config-save <path>` — save in-memory config to file (atomic temp+rename)

### Audio
- `play <ch> <path> [vol] [output(1|2|all)] [loops(0=inf|N)]` — start playback on a mixer channel
- `queue <ch> <path> [now|finish]` — queue a follow-on sound
- `queue-clear <ch|all>` — clear a channel's queue
- `audio-stop [ch|all]` — stop one channel or all
- `fade <ch>` — fade-stop a channel
- `volume <ch|master> <0-100>` — set channel or master volume
- `audio-status` — raw AUDIO_STATUS_RESP payload
- `audio-preloads` — PSRAM asset cache: per-path residency + owners
- `codec-status` — raw CODEC_STATUS_RESP payload

### Effects (hub-side services)
EngineFX:
- `engine-start` / `engine-stop` — kick off the startup / shutdown sequence
- `engine-status` — current engine state + RC toggle

GearControl:
- `gear-list` — list configured gear units
- `gear-deploy <id>` / `gear-retract <id>` — lower / raise a gear unit
- `gear-stop <id>` — halt motion (motor brake)
- `gear-reset <id>` — clear error state (ERROR → retracted)
- `gear-all <stop|deploy|retract>` — apply to every configured gear
- `gear-status` — per-unit gear lifecycle phases

GunFX:
- `gun-start <id> [rpm]` / `gun-stop <id>` — start/stop auto-fire (rpm 0 = default)
- `gun-fire <id>` — fire exactly one shot
- `gun-smoke <id> <on|off>` — arm/disarm the smoke heater
- `gun-manual <id> <key=val> …` — puppet-mode override (yaw_us, pitch_us, rof, fire, smoke, fan_burst)
- `gun-manual-release <id>` — exit manual override; RC resumes next tick
- `gun-verbose <id> <on|off>` — subscribe ~10 Hz GUN_VERBOSE_STATUS broadcasts
- `gun-status` — per-gun firing + smoke state

LandingLight:
- `landing-list` — list configured landing lights (owner + phase)
- `landing-on <id>` / `landing-off <id>` — deploy + power on / power off + retract
- `landing-status` — per-light lifecycle phases

LightFX (hub program engine — drives an expander's LED channels via roles):
- `light-programs` — list LightFX programs registered on the hub
- `light-select <idx|name>` — switch to a program
- `light-brightness <0-100>` — set LightFX master brightness
- `light-reset` — drop the active program; LEDs off
- `light-status` — active program + master brightness

Alerts:
- `alert <info|warning|error|critical> [outputMask]` — play the preset alert sound
- `alert-stop` — silence the alert channel
- `alert-status` — alert channel state + last severity

## Subscribe to live telemetry / diagnostics
```bash
app/go/scalefx-cli.exe -p COM15 -c "subscribe"
```
Streams async events + firmware `[LOG I]` diagnostics until disconnect — the way to read instrumentation (e.g. `[servo] …`, `[bimotor] …`, `[ll] …`, `[lightfx-selector] …`) while exercising the board from Studio or the radio. Live-view broadcasts (RC channels, servo status) are lossy by design (Rule 53); registered flow-control ACKs are not. Async role outcomes (`bimotor-move-end`/`-seek`) arrive here.

## Output formatting (Rule 19 extension + colour)
CLI command handlers + renderers live in `app/go/console/` (`cmd_<area>.go`) on top of the typed client in `app/go/client/` + the wire mirrors in `app/go/protocol/<mod>/`. Colourized output uses the shared ANSI helpers in `app/go/console/term.go` (`cGreen`/`cYellow`/`cRed`/`cCyan`/`cDim`, `Ok`/`Note`/`Hdr`) — green=active/ok, yellow=warn/transition, red=error/fault, dim=idle. Mirror that palette when adding a new command.

## Reproducing wire issues
The CLI never trips keepalive/upload-exclusivity/thread-safety hazards (single-threaded, no live-view, no pollers). A bug that only shows in Studio is almost always one of Rules 53–56 (lossy-vs-reliable async, stream-upload exclusivity, native-hardware, thread-safe `Connection`). **Always validate wire changes against Studio**, not just the CLI.
