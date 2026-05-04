# Virtual Board

A host-side Go binary that pretends to be a ScaleFX controller. **One
artifact, three modes:**

1. **Long-running emulator** — listens on TCP and lets `scalefx-cli`
   and ScaleFX Studio connect to it as if it were a real device. Use
   it to develop UI features, reproduce wire-format bugs, or smoke-test
   commands without flashing firmware.
2. **Auto-discoverable port** — each running instance writes a
   discovery file so virtual boards appear in the CLI's `ports` list
   and in Studio's Connect dialog automatically.
3. **In-process test fixture** — `go test ./boards/<kind>/` drives the
   same Board type via the same Sender interface used by the TCP
   server, with an injected clock for deterministic timing assertions.
   This is where the LED-event timing + landing-gear contract tests
   live now (formerly `tests/lightfx_sim/`).

One binary emulates any board type (LightFX, GearControl, GunFX,
HubFX). The board is selected at startup; per-board behaviour lives in
`boards/<kind>/`.

## Layout

```
tests/virtual_board/
├── main.go                     ← flag parsing + board factory
├── server/                     ← generic TCP listener, COBS framing, status loop
│   ├── server.go
│   └── board.go                ← Board interface
├── shared/
│   └── events.go               ← LED event evaluator (LightFX + HubFX share it)
└── boards/
    ├── lightfx/                ← 8 LED channels, 3 servos, 3 landing lights, battery
    │   ├── board.go
    │   ├── state.go
    │   └── lightfx_test.go     ← 12 unit tests covering events + program runtime
    ├── gearcontrol/            ← 3 gears (× 2 door servos), yaw, gear-input PWM
    ├── gunfx/                  ← 3 gun servos, smoke gen, trigger
    └── hubfx/                  ← slave registry, audio, file storage, INA226 rails

app/go/virtualdiscovery/        ← discovery helper (used by both protocol + harness)
```

## Build

```bash
# emulator binary
cd tests/virtual_board
go build -o virtual_board.exe .

# unit tests (covers the LED event state machine + program runtime)
go test ./...
```

No g++ / CMake / Ninja needed — host-side firmware compilation has
been retired. The Go tests in `boards/lightfx/lightfx_test.go` cover
the same scenarios that lived in `tests/lightfx_sim/` (beacon rhythm,
flash duty, fade ramps, sinusoidal fading, multi-channel lockstep,
landing-group deploy/retract on program switch, stale-sequence
cleanup).

## Three workflows

### 1. Driving Studio's UI by hand

```bash
# Terminal 1
./virtual_board.exe -board lightfx -port :9000

# Terminal 2 — launch Studio (or use the VS Code task)
cd app/go/studio && wails dev
```

The Connect dialog now lists `tcp://localhost:9000` with description
`Virtual lightfx: LightFX-Virtual` (auto-enumerated through the
discovery file). Click → connect. Studio identifies the board, hits
`init direct verbose`, and the LightFX panels start updating from the
1 Hz STATUS broadcast.

Drag the master-brightness slider, click LED channels on/off, deploy a
landing-light slot — every action round-trips through the wire layer
just like a real Pico would, only the device side is a Go process you
can attach a debugger to.

You can run **multiple boards concurrently** for hub-style scenarios:

```bash
./virtual_board.exe -board hubfx       -port :9000   # listed first
./virtual_board.exe -board lightfx     -port :9001
./virtual_board.exe -board gearcontrol -port :9002
./virtual_board.exe -board gunfx       -port :9003
```

All four show up in the Connect dialog and in `scalefx-cli ports`.

### 2. Driving the board through the CLI

```bash
cd app/go
go build -o scalefx-cli.exe ./cli/

./scalefx-cli.exe ports                       # virtual boards appear here
./scalefx-cli.exe --port tcp://localhost:9000 # connect to LightFX

# inside the CLI:
init direct verbose                # turn on STATUS broadcast (1 Hz)
status                             # confirm round-trip
light:led 1 80                     # LED ch1 → 80%
light:landing.deploy 1             # deploy slot 1 (1.5 s phase + async status)
gear:deploy 1                      # GearControl-only command
gun:trigger.on                     # GunFX-only command
hub:slaves                         # HubFX-only command
```

For scripted tests:

```bash
echo "init direct verbose; light:led 1 80; status; quit" \
  | ./scalefx-cli.exe --port tcp://localhost:9000
```

### 3. Automated unit tests (in-process, no TCP)

```bash
cd tests/virtual_board
go test ./boards/lightfx/ -v
```

Tests drive the in-process `Board` via the same `Sender` interface the
TCP server uses, so packet parsing + handler logic + state-machine all
get exercised. Time is injected (`Board.SetClock`) so the LED-event
rhythm is deterministic.

```go
d := newDriver()
ledSeqAddBeacon(d, /*ch*/ 1, /*cycle*/ 1000, /*duration*/ 0,
                          /*flash%*/ 20, /*max*/ 100, /*min*/ 0)
ledSeqStart(d, 1)
d.advance(100*time.Millisecond, time.Millisecond)
expectNear(t, "beacon peak", int(brightness(d, 1)), 100, 5)
```

The merged tests cover:

- LED on/off rhythm
- Flash 50/50 duty across multiple cycles
- FadeIn linear ramp
- Beacon timing — peak inside flash window, dwell at min during off
- Fading sinusoidal cosine pulse
- Multi-channel `seqStart(0)` phase lock
- Light-program runtime: select → deploy/retract group, NAV ↔ LAND
  toggle, reset, stale-sequence cleanup

For end-to-end (over-the-wire) tests, drive the binary via TCP from a
separate `_test.go` that spawns it in-process — the harness for that
isn't written yet but is a small lift.

## Combined GUI + CLI debugging session

A typical bug-hunt session uses both at once:

1. Launch the virtual board with `-verbose` so every packet is logged
2. Launch Studio in dev mode; connect to it
3. Reproduce the GUI issue (drag a slider, click deploy, etc.)
4. Cross-reference: the virtual board log shows the exact bytes sent
   by Studio, in the order received; the same bytes can be replayed
   through `scalefx-cli` to isolate whether the bug is in the panel or
   the wire layer
5. If it's a state-machine bug (e.g. "the program switch leaves a
   stale sequence playing"), capture the scenario as a Go test in
   `boards/<kind>/<kind>_test.go` so the regression can't come back

## Adding a board

1. Create `boards/<kind>/board.go` implementing `server.Board`:
   `Name`, `Version`, `Platform`, `BoardKind`, `Capabilities`,
   `Tick(now)`, `BuildStatusModuleData()`,
   `HandlePacket(s, ptype, tag, payload)`.
2. Wire it into `main.go:newBoard` so `-board <kind>` resolves.
3. Pick a device name that starts with one of `LightFX` / `GearControl`
   / `GunFX` / `HubFX` so `core.DetectControllerType` picks the right
   command group on the client side.
4. Add a `<kind>_test.go` with a few timing / state assertions to lock
   in the behaviour.

## Adding a packet to an existing board

1. New switch arm in `boards/<kind>/board.go:HandlePacket`.
2. Update the state struct in `state.go` if the command persists data.
3. If the new state is observable, add the bytes to
   `BuildStatusModuleData` so Studio's panels see it through the
   1 Hz STATUS broadcast.
4. Add a Go test if the behaviour is timing-sensitive.

## Tradeoff vs the previous C++ harness

`tests/lightfx_sim/` used to compile the actual firmware C++ code
(`led_event_seq.cpp`, `light_program_manager.cpp`) host-side using
g++/CMake/Ninja, so it caught regressions in the firmware itself.
Merging into `virtual_board` means tests now run against a Go
re-implementation of the same algorithms (`shared/events.go` mirrors
the LED event formulas; `boards/lightfx/board.go` mirrors
`LightProgramManager::selectProgram`).

What we lose: a Go-only tree no longer detects C++-specific regressions
(integer truncation, float→int conversions, ABI quirks). What we gain:
one unified harness, no host-toolchain dependency, faster iteration
loops, and integration tests that run against the same code Studio
talks to. If the Go re-implementation drifts from firmware behaviour,
the wire-layer tests would still flag the symptoms — the diagnosis
just won't pinpoint "this is a C++ bug" anymore.
