# ScaleFX — Claude Code Guide

Authoritative rulebook: [.github/copilot-instructions.md](.github/copilot-instructions.md) (Rules 0–60; all GunFX Phase 4 rules landed; Rules 53–56 = wire-async / upload-exclusivity / native-hardware / thread-safe-wire; Rule 57 = upload self-heal; Rule 58 = transparent expander roles (opaque PortRef-addressed transport — drive/query/telemetry of any role on any board behaves hub-local); Rule 59 = every Studio tab routes to a DEDICATED panel (no generic fallback — un-hooked domains render NotImplementedTab; verify the id chain domainCatalog → studioTabs → MainLayout by clicking the tab); Rule 60 = panel layout grammar (two-column unit cards: ports LEFT / behaviour RIGHT; live widgets full-width; ≤4-option mode choices = `.seg-select` segmented toggles, radios retired; sibling-aware unclaimed pools; ops in header; param packs in grids). **HubFX is pure ESP-IDF now — `framework = espidf`, zero Arduino.**).
Detailed workflows: [instructions/](instructions/) — grouped index in [instructions/README.md](instructions/README.md); start at [32-ARCHITECTURE-DIAGRAMS.md](instructions/32-ARCHITECTURE-DIAGRAMS.md) for the visual system map.

Studio UI work: **start at [instructions/23-STUDIO-WIDGET-CATALOG.md](instructions/23-STUDIO-WIDGET-CATALOG.md)** — handbook of every reusable widget pattern with copy-pasteable snippets.

This file is a compact index for Claude. When a rule below conflicts with copilot-instructions.md, that file wins — update this one to match.

## System at a glance

Multi-platform embedded effects: one **ESP32-S3 HubFX** master + up to N **Pico (RP2040)** generic-expander boards (LightFX, GearControl) over USB CDC at 6 Mbps. Expanders expose ports + roles; the hub drives them transparently (Rule 58). (Terminology: live code still uses `slave`/`SlaveType`/`BoardState::SLAVE`; the rename to `expander` is deferred — grep the code, not the docs, for the current name.)

- **Protocol:** Binary COBS, `[type:u8][tag:u8][len:u16LE][payload:0–512][crc8:u8]`, CRC-8 poly `0x07`, little-endian throughout.
- **HubFX Pico (RP2350) is OBSOLETE** — [controllers/hubfx/pico/](controllers/hubfx/pico/) is frozen reference. All new HubFX work goes in [controllers/hubfx/esp32s3/](controllers/hubfx/esp32s3/). (Rule 17.)
- **Expander wire surface (legacy per-board ranges, mostly superseded by ports/roles):** GunFX `0x01–0x2F`, LightFX `0x40–0x5F`, GearControl `0x60–0x7F`. These describe a *standalone expander's* own packets — NOT what the hub dispatches.
- **HubFX master dispatch map (the authoritative, conflict-validated allocation — every packet a `BoardOf<>` policy `ownsType()` on the hub; dispatch STOPS at the first policy whose `ownsType` returns true, so NO two policies may claim the same byte).** Validate against this before adding a packet; append at the next free value, never into a neighbour's block:
  - Ports `0x10–0x3F` · Roles `0x40–0x7F`
  - Expander `0x80–0x87` · Topology `0x88–0x8F` **+ `0xA6–0xA7`** (ROLE_QUERY/RESPONSE — generic request-response forward, 2026-06-07; the 0x88–0x8F block was full)
  - Config `0x90–0x92` + `0xAC` · Storage `0x93–0xA5` + `0xA9` + `0xB0` (upload-diag `0xA4/0xA5`) · Codec `0xAA–0xAB`
  - **Effects:** LandingLight `0xB1–0xB6` · LightFX `0xB7–0xBD` · GearControl `0xBE–0xC6` **+ `0xD7`** (GEAR_RESET; `0xD8/0xD9` freed — GEAR_CALIBRATE/CALIB_CANCEL removed per instructions/29 §6a #3) · EngineFX `0xC7–0xCB` · GunFX `0xCC–0xD2` **+ `0xE2–0xE5`** (manual override + verbose status) · Alerts `0xD3–0xD6`
  - **Audio control `0xDA–0xE1`** · **Audio preload diag `0xE6–0xE7`** · **Input routing `0xE8–0xEA`** (global RC→effect gate, InputDispatcher) · BatteryConfig `0xEE` · Board/Core lifecycle `0xEF–0xFF`
  - Free: `0x00–0x0F`, `0xA8`, `0xAD–0xAF`, `0xD8–0xD9` (ex-GearCalibrate), `0xEB–0xED`.
  - ⚠️ This index DRIFTS — before adding a packet, grep the actual `ownsType()` predicates in `controllers/hubfx/esp32s3/src/`, not just this map. Past collisions all came from the map lagging the code (e.g. GunFX silently grew into `0xE2–0xE5`, so a later AUDIO_PRELOAD placed there was swallowed by gunfx's earlier `ownsType` → `MISSING_PARAM` NACKs). Append at the next truly-free value; never into a neighbour's block.
- **Error ranges (comprehensive allocation, 2026-05-23 sweep):**
  - `0x00–0x1F` Serial / generic (wire framing + param validation)
  - `0x20–0x2F` **PortError** (port-level infrastructure)
  - `0x30–0x3F` **GunError** (was 0xCB-0xCD)
  - `0x40–0x4F` **RoleError** (role-level infrastructure)
  - `0x50–0x5F` **LightFxError** (was 0xB6-0xB8)
  - `0x60–0x6F` **GearError** (was 0xC1-0xC6)
  - `0x70–0x7F` **EngineError** (was 0xC6-0xC7 — root cause of the EngineStart→GEAR_NO_STALL_DETECTED ghost on 2026-05-23)
  - `0x80–0x87` **ExpanderError**
  - `0x88–0x8F` **LandingLightError** (was 0xB1-0xB4)
  - `0x90–0x9F` **AlertError** (was 0xD1-0xD3)
  - `0xA0–0xAF` **StorageError** (was 0x86, 0x8A-0x8F — was squatting in storage packet bytes)
  - `0xB0–0xBF` **AudioError** (was 0x85, 0x89)
  - `0xC0–0xC7` **TopologyError** (infrastructure — was 0x90–0x96, collided with AlertError; moved 2026-06-06)
  - `0xC8–0xEF` Reserved (free for future effects)
  - `0xF0–0xFF` System (internal/comm errors)
  - Lesson: error codes collide silently in Go's `errorNames` map (last `init()` wins → wrong name shown), and squatting in packet-type bytes is wire-OK but human-confusing. Keep every error in its spec range above; one namespace per range. **Guarded:** [tests/host/go_unit/error_collisions_test](tests/host/go_unit/error_collisions_test) asserts no two modules register the same error code under different names — it's in the pre-merge gate.

## Layout

```
controllers/
  lightfx/pico/src/           LightFX generic-expander (Arduino-Pico, C++20) — 8 PWM-LED + 3 servo + battery
  gearcontrol/pico/src/       GearControl generic-expander (Arduino-Pico, C++20) — 7 servo + 3 H-bridge.
                              (Standalone gunfx Pico controller REMOVED 2026-06-06 — those effects now
                              live only on the HubFX master under hubfx/esp32s3/src/effects/.)
  hubfx/esp32s3/              active HubFX — PURE ESP-IDF (framework=espidf, app_main in hubfx_esp32s3.cpp)
  lib/                        shared drivers + protocol
    sfx_platform/             OS/SDK abstraction (SFX_DELAY_MS, SfxMutex, SFX_FREE_HEAP, SPSC ring)
    sfx_serial/serial/        wire (CRC-8/COBS), DiagLog, client (SerialBus/BusClient/ResultQueue)
    sfx_peripherals/          LED, servo, PWM, I2C, INA226 + IndicatorServicePolicy / BatteryServicePolicy
    sfx_board/                BoardServer<...UserPolicies> + BoardServicePolicy + Port/RoleServicePolicy
                              + roles/ + per-family role handlers + StreamWriter
    sfx_audio/ sfx_storage/ sfx_config/ sfx_usb/    subsystem drivers + their service policies
app/go/
  protocol/<mod>/             wire format mirrors of C++ headers (source of truth for the HubFX
                              master protocol; firmware-side header archived)
  client/<mod>.go             typed SDK (+ client/roletarget.go — transparent role I/O)
  console/cmd_<mod>.go        CLI command registry (self-registering in init())
  cli/                        scalefx-cli.exe entry
  flash/                      scalefx-flash.exe entry
  studio/                     Wails v2 GUI (Svelte frontend/)
  firmware/                   build + flash logic
media/                        asset library — WAVs (sounds/) + YAML preset templates (presets/).
                              Mirror of the firmware's expected on-device layout; see media/README.md
                              for the catalog and deployment notes.
tests/                        standalone test projects (firmware + Go tools)
```

## Build / flash / run

Prefer VS Code tasks (Rule 20) — they set cwd and handle controller prompts:

- `Build Firmware`, `Build and Flash Firmware`, `Flash Firmware (no build)`
- `Build Go CLI`, `Build Flash CLI`, `Build ScaleFX Studio (GUI)`
- `Interactive CLI (Go)`, `Run ScaleFX Studio (GUI)`

Raw commands (when a task doesn't fit):

```bash
app/go/scalefx-flash.exe build  <lightfx|gearcontrol|hubfx> --no-clean
app/go/scalefx-flash.exe flash  <controller> --no-clean
cd app/go     && go build -o scalefx-cli.exe   ./cli/
cd app/go     && go build -o scalefx-flash.exe ./flash/
cd app/go/studio && wails build
```

After any C++ protocol change: `cd app/go && go build ./cli/` — compiling the Go side is the primary sync check.

## Rules (condensed)

Authoritative rules with full rationale + examples: [.github/copilot-instructions.md](.github/copilot-instructions.md) (Rules 0–60). Below is the compact index — **same rule numbers**, one line each. When this conflicts with copilot-instructions.md, that file wins. Architecture overview: [32-ARCHITECTURE-DIAGRAMS.md](instructions/32-ARCHITECTURE-DIAGRAMS.md).

**Protocol & wire**
- **1** Protocol mirror — every packet/error/command in `serial/<mod>/<mod>.h` has a twin in `app/go/protocol/<mod>/<mod>.go`. Never change one side alone.
- **3** `*Client` methods return `CommandResult`, never `bool`. Query responses are implicit ACKs; skip `TAG_ASYNC`.
- **4** Little-endian everywhere (`payload[0] | (payload[1]<<8)` / `binary.LittleEndian.*`).
- **5** Physical units in names: `voltage_mV`, `timeout_ms`, `current_mA` — never bare.
- **8** Indicator LEDs via `board.indicators().setErrorCondition(...)`; every C++ error constant has a same-valued Go twin, kept in its spec error range (see the error-range map above).
- **11** Append-only payload extension: default value = no-op; server checks `len` for optional fields. (Wire compat across firmware versions — distinct from Rule 21 code compat.)
- **12** Channel enable/disable: guard `if (!_enabled) return ERROR_DISABLED;` at top; disabled = not persisted, distinct error, STATUS flag.
- **13** Error lifecycle: explicit reset command → UNKNOWN, clears reason, rejects actions until cleared. Not reboot-only.

**Framework & C++ structure**
- **6 / 18** Compose `BoardServer<...UserPolicies>` (usually via `BoardOf<TBoard, TStream, PortCapacity<…>, …ExtraPolicies>`). Every protocol-exposed subsystem is a `*ServicePolicy` satisfying the `SystemServicePolicy` concept (`kCapabilityBits / begin / ownsType / handle / update`). `BoardServicePolicy` + `IndicatorServicePolicy` + Port + Role are auto-prepended. C++20: gate every policy/schema template with a `concept` + `requires`. The old `SfxServer` / `addModuleHandler` / `BusServer` / `CommandRouter` / `ICommandHandler` are DELETED — any reference is stale.
- **7** Reuse before writing — check `controllers/lib/` first; enhance the library, don't inline hardware I/O. New I2C drivers extend `I2CDevice`.
- **14** Singletons only for board-unique resources (C++11 thread-safe static local, private ctor, deleted copy/move, idempotent `begin()`): `DiagLog`, `SdCardModule`, `FlashModule`, `AudioMixer`. Per-channel arrays are NOT singletons.
- **16 / 55** Shared `controllers/lib/` code uses `sfx_platform.h` macros + native ESP-IDF/Pico-SDK abstractions — never raw SDK in shared code, never the Arduino API (`pinMode`/`digitalWrite`/`Wire`/`Serial*`/`Servo`/`millis`/`delay`). HubFX is pure ESP-IDF (`ESP_PLATFORM`, native `EspServo`/LEDC/VFS); `<Arduino.h>` is Pico-only. New driver = a native `{esp,pico}_*` pair + selector.
- **33** Eliminate redundant template params — recover them from a carrier's nested typedefs (`AudioMixer::I2SOutput`) or a trait specialization, not a second template arg.
- **House rules:** no back-compat scaffolding in refactors (delete dead fields/flags outright — distinct from Rule 11 wire compat); no thin wrappers (inline a method that only decodes+fires-observer or only calls another).

**Dual-core & timing**
- **15** Cross-core variables are `std::atomic<T>` with explicit memory order (release on writes / acquire on reads / relaxed for same-core counters); `volatile` banned (MMIO only); annotate ownership; extract `atomic<T*>` to a local before `->`.
- **40** Effect-layer code (effects + roles) times off `sfx_core::EffectClock` (`nowMs`/`dtMs`), never raw `millis()`; `BoardServer::process()` latches it once per pass. Raw `millis()` stays for infrastructure + a role's purely-local timeout.

**Roles, ports & actuators**
- **31** Port direction is fixed at declaration (Servo/Pwm/HBridge output-only; Input is the one multi-modal input kind). `#InputPorts ≤ #UART peripherals` (ESP32-S3 ≤ 2, Pico ≤ 1).
- **37** Output port descriptors carry rail voltage via `.with_voltage_mV<N>()`; flows to Go/Studio + drives sub-rail element duty scaling. 0 = unknown.
- **42 / 44** Actuator mechanism (voltage scaling, motion profile, stall guard, calibration, REV) lives on the ROLE layer; effects stay at intent (`setTarget`/`setPct`). Per-port config in `/hubfx.yaml ports[]`. Servo profile edited in the feature panel (44); element scaling on the IO tab.
- **43** Effects reference RC channels by NAME from `/hubfx.yaml inputs:`; the apply translator resolves name → `PortRef` + channel (`findInputByName`). Output ports stay as `PortRef` in the effect YAML.
- **58** Transparent expander roles — opaque `PortRef{guid,kind,idx}`-addressed transport (FORWARD 0x8F / QUERY 0xA6→0xA7 / EVENT 0x8E / BULK_ATTACH 0x57). Expose a role via its codec + one line in `roleKindFor<>()` + a `RoleTarget` wrapper — NEVER a per-role switch in transport/forward/event/Go-dispatch. See [32 §3-4](instructions/32-ARCHITECTURE-DIAGRAMS.md).

**Wire reliability (Studio concurrency)**
- **53** Flow-control async ≠ lossy telemetry. The async dispatcher DROPS `TAG_ASYNC` on a full queue (live-view is lossy by design); a packet with a `RegisterAsyncFilter` (e.g. `FILE_UPLOAD_PROGRESS`) bypasses the lossy queue. Never route an ACK/flow-control packet through the broadcast path.
- **54** Raw stream upload holds the wire exclusively (COBS bypassed) — keepalive gated off, no concurrent commands; `Send()` logs any mid-stream COBS write as `COLLIDE`. Firmware UART RX ring ≥ segment (HubFX 32 KB).
- **56** The Go `protocol.Connection` is shared across goroutines — guard ALL mutable state (`nextTag`/`tagMu`, waiters/`waiterMu`, writes/`writeMu`, `streamActive`/atomics). An unlocked `NextTag()` = duplicate-tag timeout. Race-test against Studio, not the CLI.
- **57** Upload self-heal + wire-hold — detail in copilot-instructions.md.
- (Deep dive for 53/54: [27-WIRE-ASYNC-AND-UPLOAD.md](instructions/27-WIRE-ASYNC-AND-UPLOAD.md).)

**Go CLI / Studio plumbing**
- **2** Add a command: firmware `<mod>.h` + policy `handle()` case → controller README → `app/go/protocol/<mod>/<mod>.go` → `app/go/client/<mod>.go` → `app/go/console/cmd_<mod>.go`. (The old `api/` + `engine/handlers/` are archived; see [04-CHANGE-PROPAGATION.md](instructions/04-CHANGE-PROPAGATION.md).)
- **19** Decoded structs + pure `Decode*` in `app/go/protocol/<mod>`; typed accessors in `app/go/client/<mod>.go`; CLI renderers in `app/go/console/cmd_<mod>.go`. Never re-decode in `studio/app.go`.
- **25** Shared-module commands (storage, config) are universal — no board prefix. `Engine.SetControllerType(ct)` is the single setter that also propagates peer max-payload (Pico 512 / ESP32 2048 — use the named constants, never inline `508`/`2044`).
- **30** Board commands are grouped + disambiguated from universal commands (`connect`/`init`/`status`/file/config stay bare); role drive/query commands take a trailing `guid=XXXX`. Command surface: the **scalefx-cli skill**.

**Config & persistence**
- **26** Per-board config YAML auto-hydrated on connect: each controller owns its file (`/hubfx.yaml`, `/gearcontrol.yaml`, `/lightfx.yaml`, …); the firmware schema's `defaultPath()` is the single source of truth; tabs with a `BoardConfigDriver<T>` call `autoLoadOnConnect(...)`.
- **27** Canonical YAML: emit indented block sequences only; parsers ALSO accept compact + single-line flow collections (`{ kind: pwm, idx: 0 }`) but never emit them. Firmware `YamlParser::parseFlowNode` + Studio `parseFlowValue` stay in lock-step; Go uses `gopkg.in/yaml.v3`. Example: [sfx_config/README.md](controllers/lib/sfx_config/README.md).

**Studio UI** (full detail in copilot-instructions.md + [23-STUDIO-WIDGET-CATALOG.md](instructions/23-STUDIO-WIDGET-CATALOG.md))
- **23** Every board tab validates via a `<board>-verifier.ts` (`ConfigVerifier<T>`) + surfaces issues; `<SaveConfigDialog>` gates flash on zero errors.
- **24** Every interactive setting has a verb-led `title=` tooltip + validates on change + debounce-pushes (~350 ms) to the board; jog/position controls are immediate commands. Save still persists.
- **28** Per-board servo config uses the shared `ServoCalibrationDialog` (summary + `⚙ Calibrate Servo…`); never inline a servo slider panel in a row.
- **29** Battery card = canonical layout (`.batt-display` + cutoff/chem rows), LEFT column, live-push keys `battery` + `battery.cutoff`; reuse the GearControl `.batt-*` CSS verbatim.
- **34** Reuse the ONE design language in `style.css` (`button`/`.field-input`/`.card`/`.form-row`/CSS-vars); component `<style>` does layout only. Row-button order: picker `…` leftmost, `Clear` rightmost (`.btn-slot`/`.btn-spacer` align columns). `pickFile({ targets })` narrows the backend.
- **35** Validation gates Apply AND operational actions (`▶ Start`/`Test`/`Preview`) on `busy || dirty || hasErrors`; surface each error at the field AND the section header; validate continuously, not on click.
- **36** RC-channel + µs-threshold gating uses the shared `ChannelToggleCluster.svelte` (selector → trigger settings → live bar w/ NO-SIGNAL stripe → legend). Don't inline the rows.
- **38** Discrete N-band selectors (gun ROF) use the shared `ChannelBandCluster.svelte` (reverse-painted multi-band bar + overlap hatch); new items seed a non-overlapping band, never `[0,0]`.
- **39** Optional section with no port + empty candidate pool → YELLOW `section-warn` chip (non-blocking; distinct from Rule 35 red which gates Apply).
- **41** Optional puppet/manual-override subsection: left "Drive" inputs + right "Live mirror" (~10 Hz), per-call subsystem bitmask, 5 s auto-release; gates on `dirty || hasErrors`.
- **45** Effect enable affordance is a `<button class="small state-toggle">` (state in the LABEL), never a checkbox; lives in the status-row.
- **46** Studio Apply/dirty/validation is centralized in the global `ConfigToolbar`; each domain exports a `DirtySource` (`{id,label,isDirty,hasErrors,apply,refresh}`) registered in `App.svelte` (hubconfig first). Panels are pure views.
- **47** WAV-path + stereo-routing rows use the shared `SoundRow.svelte` (routing button rightmost, stays interactive when empty); masks in `speaker_routing.ts`.
- **48** Binary operational on/off is ONE action-toggle whose label is the ACTION (`▶ Engine Start` ⇄ `■ Engine Stop`), `.danger` when live; ON→OFF always allowed (emergency cutoff), OFF→ON keeps the Rule 35 gate.
- **49** Output-port pickers filter by `roleKind == required` AND unclaimed (keep current pick via `isExempt`); empty pool → Rule 39 yellow. Use the shared `freePortPool` helper. (Input ports use Rule 43.)
- **50** ONE typography ladder: shared `.hint`/`.hint.warn`/`.hint.err`/`.help-text`/`.unit` classes in `style.css`; never redefine locally, never inline a hex (CSS vars only).
- **59** Every Studio tab routes to a DEDICATED panel — the generic DomainTab is DELETED; un-hooked domains render `NotImplementedTab` (loud placeholder, never a half-UI). New/renamed tab = verify the STRING id chain (Go `domainCatalog` → `studioTabs` → `MainLayout` case → panel) by clicking the tab; ids cross the Go/TS boundary and never fail at compile time.
- **60** Panel layout grammar: per-unit cards = two-column grid (LEFT ports + calibration, RIGHT behaviour/sequencing; collapse <900px); live-telemetry widgets ALWAYS full-width; mode choices ≤4 options = `.seg-select` segmented toggle (radios retired, dropdowns only >4/dynamic); pools sibling-aware unclaimed-only (49+); ops in the card header, config in the body; ≥2 related numerics = `.form-grid`; in-column section heads use `.sub`. Reference: GearPanel strut cards.

**Process & tests**
- **0** Docs are code — update README / instructions / copilot-instructions.md in the same commit as the code.
- **9 / 10** Bump `BUILD_NUMBER` every flash (auto); bump `FIRMWARE_VERSION` proactively (MAJOR wire-breaking / MINOR additive / PATCH logic). Verify via `init`.
- **21** Tests live in [tests/](tests/) (`go_unit` / `go_integration` / `native` / `hw`), never under `controllers/*/test/` or `app/go/tests/`. Production code never imports from `tests/`.
- **22** Release notes mandatory — categorize New Features / Bug Fixes / Protocol Changes / Breaking ⚠️ / Internal; explain the version bump.
- **51** Tests must build cleanly; a refactor moves/deletes its tests in the SAME commit. Integration tests `t.Skip` on `testing.Short()` + missing `SCALEFX_HUBFX_PORT`.
- **52** Pre-merge gate: `tools/run-tests.ps1 -Premerge` MUST exit 0 before merging to `main` (unit + integration-vs-HubFX + `scalefx-flash build hubfx`).
## Key architectural touchstones

- **Handler macros** (`sfx_serial/serial/core/core.h`): `SFX_REQUIRE_LEN(n)`, `SFX_VALIDATE(cond, err)`, `SFX_DISPATCH(cb, ...)`, `SFX_HANDLE_CHANNEL_CMD(v, err, cb)`.
- **STATUS payload** = 22-byte core header (`counter:u32, uptime:u32, freeRam:u32, lastActivity_ms:u32, keepaliveCount:u32, boardState:u8, initFlags:u8`) + module callback data via `board.core().onStatusData(cb)`. Board states: `IDLE(0)`, `STANDALONE(1)`, `SLAVE(2)`, `DIRECT(3)`.
- **INIT_READY / IDENTIFY (0xFE)** — same length-prefixed payload (`nameLen:u8, name, verLen:u8, ver, platLen:u8, plat, cpuMHz:u32LE, freeRam:u32LE, buildNum:u32LE, capabilities:u32LE`). IDENTIFY does NOT activate hardware → used for safe type detection; INIT activates. HubFX auto-inits at boot (IDENTIFY suffices); slaves still need INIT. **Capabilities** is a Rule 11 append-only u32 bitmask, computed at compile time by `BoardServer<...>` as the OR of every policy's `kCapabilityBits` and seeded into `BoardServicePolicy` during `begin()`. Flags: `FLASH(1<<0)`, `SD(1<<1)`, `AUDIO(1<<2)`, `USB_HOST(1<<3)`, `ENGINE(1<<4)`, `CONFIG(1<<5)`, `SLAVE_BUS(1<<6)`, `BATTERY(1<<7)`. Mirrored in [protocol/core/core.go](app/go/protocol/core/core.go) as `CapFlash`/`CapSd`/… plus `HasCapability()` / `CapabilityNames()` helpers; surfaced through `Engine.Capabilities()` / `Engine.HasCapability(want)` and Studio's `DeviceCapabilities()` Wails binding. Studio's file manager (`FsStorageStatus`) gates flash/SD probes on these bits — never speculatively probe a backend the device does not advertise.
- **Response categories** (drives client design, see [instructions/01-ARCHITECTURE.md](instructions/01-ARCHITECTURE.md)):
  - **Instant** — `SFX_DISPATCH` → auto ACK/NACK; client gets `CommandResult` back from `sendCommand(...)`.
  - **Query** — server emits a typed response packet (no `SFX_DISPATCH`); client uses `sendQuery(reqType, payload, len, respType, out)` which captures the typed response in `out` and resolves the tag as `CommandResult::Ack()`.
  - **Long-Running** — immediate ACK on start; final state arrives via STATUS broadcast or async packets. Skip tag resolution when `tag == SfxWire::TAG_ASYNC`.
- **setup() sequence** (any controller): (1) `board.begin(prefix, version, buildNumber, ledPin, errPin)` — wires Serial, DiagLog, indicator pins, every policy, lifecycle callbacks, capability advertisement; (2) hardware init that needs to happen before INIT; (3) `board.policy<P>().bindXxx(...)` for policies that take external dependencies (battery sensor, USB registry, …); (4) optional `board.addExpectedI2CDevice(...)` + `board.enableI2CScan(Wire)`; (5) `board.onInit([](mode, flags){…})` / `board.onShutdown([](){…})` for controller-specific hooks. Then `loop()` is one line: `board.process()`.
- **Go stack** is shared between the CLI (`app/go/console/`) and Wails Studio. The typed client (`app/go/client/`) wraps the wire mirrors (`app/go/protocol/`); CLI commands self-register in `console/cmd_*.go` `init()`; role I/O is transparent via `client.RoleTarget`. Don't duplicate decode/dispatch logic in `studio/app.go`.
- **Config persistence** uses `ConfigServicePolicy` (multi-store path-routed) + `ConfigStore<TSchema>` — YAML-first per-device schema in LittleFS. Every controller that includes `ConfigServicePolicy` in its `BoardServer<...>` pack gets `config.reload/save/status` automatically.

## Board GUID

Every ScaleFX board exposes a stable hardware-derived **GUID** so masters can tell two boards of the same kind (e.g. two LightFX expanders) apart and persist per-board state across reconnects.

- **Source.** `sfxGetBoardId(out, maxLen)` in [sfx_platform.h](controllers/lib/sfx_platform/platform/sfx_platform.h) produces an **8-char uppercase hex string** (4 bytes). Pico: last 4 bytes of `pico_unique_board_id_t` (8-byte OTP flash unique-id). ESP32: last 4 bytes of the factory MAC (`esp_efuse_mac_get_default`). Both are immutable per silicon and survive reflashing.
- **Surface on the wire.** `BoardServerBase::buildDeviceName(prefix)` in [board_server.cpp](controllers/lib/sfx_board/server/board_server.cpp) emits `deviceName = "<Prefix>-<last 4 hex chars>"` — e.g. `"GunFx-3C4D"`. That suffix is the **canonical GUID** broadcast in `INIT_READY` / `IDENTIFY` payloads (16 bits / 65 536 values). Sufficient at ScaleFX scale (a hub hosts ≤ 2 expanders today) but not collision-proof at fleet scale — log a GUID collision if it ever fires.
- **Extracting the GUID.** Strip everything up to and including the last `-` in `deviceName`. Friendly per-board *aliases* live in `/hubfx.yaml`'s `expanders:` block (alias → GUID), not on the board — see the alias bullet below. (The old `/board.yaml` `BoardIdentifier` label was retired with the generic-expander refactor.)
- **Master-side use.** `ExpanderServicePolicyT<>` on HubFX issues `IDENTIFY` to every freshly-mounted CDC device, decodes the response, extracts the GUID suffix, and stores the spec in a GUID-keyed history slot. This is what lets the master re-apply cached per-board state when a board reconnects on a different USB port.
- **Battery telemetry.** A BATTERY-capable expander (e.g. LightFX/GunFX with `BatteryServicePolicy<AdcDividerBatteryT<…>>`) appends a battery section to its STATUS via `onStatusData`. The hub's `ExpanderServicePolicyT<>` polls each Ready such expander's `STATUS_REQ` on a slow cadence (`kBatteryPollIntervalMs`), decodes the tail (offset `STATUS_CORE_HEADER_SIZE`), caches it per slot, and appends it to `EXPANDER_SYSTEM_INFO_RESP` (Rule 11) — surfaced by the CLI `system-info`. Assumes the expander's `onStatusData` emits the battery section FIRST (true for the stock LightFX/GunFX boards).
- **Config aliases for expander ports.** `/hubfx.yaml`'s `expanders:` block declares each remote board once as `alias → guid` + a nested `ports:` (port→role) list (parsed in [hubfx_config.h](controllers/hubfx/esp32s3/src/config/hubfx_config.h), flattened into the master port table with each port stamped with the board GUID). Effect sub-files address ports by `board: <alias>` (resolved to the GUID by `portRefFromNode` against a table seeded when `/hubfx.yaml` loads first; `guid:` raw form still works). Hub-local roles attach at boot (`applyPortRoles` → `attachPortRolesForGuid(cfg, "")`). **Expander roles are pushed DECLARATIVELY at bringup (the standard expander pattern, 2026-06-07):** `ExpanderService::onReady(...)` builds the board's full role set with `buildRoleBlockForGuid` and sends it in ONE `ROLE_BULK_ATTACH` (0x57) packet via `TopologyService::applyRoleConfig` — NOT `onIdentified` (which fires before `Handshake::Ready` and gets dropped). This replaced the old per-port forward loop (`attachPortRolesForGuid` over each expander) which (a) raced/dropped on reconnect — sending N single attaches at a freshly-mounted board — and (b) used `sendRoleCommand`, which **never updated the hub's cached `slot->roles` roster**, so `topo-roles`/Studio kept showing the board's OWN boot defaults (e.g. a GearControl's 7 auto-attached servos, 0 hbridge) even when the roles HAD applied. `applyRoleConfig` updates the roster from the block on ACK. An empty block (count=0) is a valid no-op — an unconfigured/fresh expander comes up bare. `RoleServicePolicy::handleBulkAttach` (shared lib, so every expander gets it) loops the same `applyAttach` the single path uses and sends ONE ACK. Live Studio edits still use the per-port `TOPOLOGY_ROLE_ATTACH` → `handleRoleAttach` (which DOES update the roster). See [instructions/19-HUBFX-CONFIG-SCHEMA.md](instructions/19-HUBFX-CONFIG-SCHEMA.md). Aliases are config-only (never on the wire); the CLI `topo-ports` now annotates each port with its attached role.
- **Adding a stronger GUID later** (16-bit collision concern): append a full-width `[guidLen:u8][guid:N]` field to the `INIT_READY` / `IDENTIFY` payload — Rule 11 append-only, old masters keep working. Producer: `sfxGetBoardId` already returns 8 hex chars; just emit them all instead of slicing to 4.

## Specific gotchas

- **BOOTSEL is command `0xF9`** (binary packet), not just a button hold. Flash CLI sends it, then waits for the RPI-RP2 USB drive. `BUILD_NUMBER` auto-increments on every build even if flash verification times out — trust the `INIT_READY` buildNum, not the source define.
- **ESP32-S3 esptool is v5.2.0 standalone (~12 MB), gitignored.** Get it via `scalefx-flash tools download`; it's searched in workspace `tools/esptool/`, next to the binary, then `PATH`.
- **Upload protocol — WINDOWED (mode=2) is the default; BATCH (mode=3, formerly STREAM) is the fast path.** WINDOWED uses per-window ACK + flow control + per-chunk CRC-16, default window size 32 (~64 KB). BATCH sends raw 512 KB segments with per-segment ACK — fastest for large files. Both modes share a **single-core** pipeline: 32 KB SRAM fill buffer (64 KB PSRAM fallback), synchronous inline SD write on Core 0. Storage is decomposed into `StorageServicePolicy` (file-ops + dispatch) + `UploadEngine<TPolicy>` (the exclusive upload state machine) + `storage_path_util.h`. Uploads are **exclusive on HubFX** — the main loop skips audio/engine/USB/diagnostics while `isUploadActive()`, and `onUploadStart`/`onUploadEnd` fire in both modes. See [27-WIRE-ASYNC-AND-UPLOAD.md](instructions/27-WIRE-ASYNC-AND-UPLOAD.md) + [32-ARCHITECTURE-DIAGRAMS.md §1](instructions/32-ARCHITECTURE-DIAGRAMS.md).
- **Stream-upload stalls are usually the UART RX FIFO, not the SD card.** `NativeUartStream::available()` counts only the driver ring; the segment tail can sit in the 128-byte HW FIFO when the client pauses for the ACK. `beginConfig` sets `uart_set_rx_full_threshold(64)` + `uart_set_rx_timeout(10)` so the tail flushes ~17 µs after line idle — without them, large uploads abort intermittently "N bytes short of a segment". Diagnose with the `upload-diag` CLI / `FsUploadDiag()` post-mortem (SD `avg`/`MAX` write latency + abort reason; `FILE_UPLOAD_DIAG_REQ` 0xA4). Note: upload MD5 is hashed over the *wire stream*, not an SD readback — a card that ACKs-but-drops writes still shows "✓ match". See [instructions/27](instructions/27-WIRE-ASYNC-AND-UPLOAD.md) §6–7.
- **`LANDING_LIGHT_BIND` (0x52) byte 2 is `channelMask`, not a single channel ID** — breaking change v0.8→v0.9. Bit N → LED channel N+1, up to 8 channels per group, servo optional. See [instructions/11-LANDING-LIGHT-GROUPS.md](instructions/11-LANDING-LIGHT-GROUPS.md).
- **AudioTools `InputMixer<float>` is broken** (accumulator bug); `SineWaveGenerator` has an amplitude bug. Workaround: keep the full pipeline in `int16_t`. See [instructions/08-AUDIOTOOLS.md](instructions/08-AUDIOTOOLS.md).
- **Console output size formatting** (CLI + parsers, see [instructions/09-CONSOLE-OUTPUT.md](instructions/09-CONSOLE-OUTPUT.md)): `<1 KB → B`, `<1 MB → KB`, `<1 GB → MB`, else `GB`. Match this exactly when writing new parsers.
- **`INA226::begin()` is strict on identity (not lenient).** Reads MFG/DIE IDs before any write and returns `false` on non-canonical chips (`!= 0x5449 / 0x2260`) without touching the chip's registers. Counterfeits at INA addresses have been observed to corrupt OTHER chips on the shared I²C bus when written to — HubFX rev hit this with a clone @ 0x40 wedging the PCA9685 @ 0x70. Boot diag still surfaces `bootMfgId()` / `bootDieId()` for the clone via `isCanonical()`. Full investigation + bisection trail in [instructions/18-HUBFX-INA-CLONE-WEDGE.md](instructions/18-HUBFX-INA-CLONE-WEDGE.md).
- **ESP32 storage is on ESP-IDF native** (since 2026-05-26). [sfx_storage](controllers/lib/sfx_storage/) bypasses Arduino's `SD_MMC.*` / `LittleFS.*` wrappers — they wrap `fs::File` (shared-ptr semantics) which surfaced cross-task / cross-core wedges on HubFX. The ESP32 path now mounts SD via `esp_vfs_fat_sdmmc_mount("/sdcard", …)` and flash via `esp_vfs_littlefs_register({.base_path="/littlefs", .partition_label="littlefs", …})`. All file ops are POSIX (`fopen`/`fread`/`fwrite`/`opendir`/`readdir`/`stat`) wrapped by `NativeFile` (move-only RAII). Per-call thread safety comes from VFS-FAT's `FF_FS_REENTRANT=1` mutex + `esp_littlefs`'s internal lock. The storage-server file-handle alias was renamed `LFSFile` → `StorageFile` (= `NativeFile` on ESP32, = Arduino `::File` on Pico). Pico side untouched (SdFat + Arduino LittleFS). See [controllers/lib/sfx_storage/README.md](controllers/lib/sfx_storage/README.md).
- **HubFX crashes leave a flash coredump — decode it, don't guess.** A firmware panic writes an ELF coredump to the `coredump` flash partition (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`). Pull + decode it in one shot with `app/go/scalefx-flash.exe coredump hubfx` (reads the partition via esptool, decodes with espcoredump + xtensa gdb against the built ELF). **The ELF must match the FLASHED build** — espcoredump refuses on a SHA mismatch, so pull BEFORE reflashing. **The ESP-IDF console is NONE** (no live panic text — see the USB-host gotcha below); the flash coredump IS the panic-debug path. Full guide + worked example: [instructions/24-COREDUMP-DEBUGGING.md](instructions/24-COREDUMP-DEBUGGING.md).
- **HubFX USB-OTG host ⟷ USB-Serial-JTAG console share ONE internal PHY (GPIO19/20).** The ExpanderService enumerates expander boards over USB-OTG **host**; the ESP32-S3 has a single internal USB PHY that USB-Serial-JTAG also uses, so a USB-Serial-JTAG **console** starves the host → plugged expanders never mount (silent: `expanders` shows none, nothing in diag). Fix (2026-06-07): `sdkconfig.defaults` → `CONFIG_ESP_CONSOLE_NONE=y` (UART0 is the COBS wire, so the console can't go there either; logs ride the DiagLog wire, panics → flash coredump). **AND** `ExpanderService::begin()` must call `usb.init()` after `usb.begin()` — `begin()` only stores port config, `init()` runs `usb_host_install()` + the daemon (the host logged "USB host up" but was never installed). The way to isolate a PHY/hardware problem from a firmware one is a minimal console-on-UART USB-host probe sketch (a few-line `usb.begin()`+`usb.init()`+`usb_host_lib_set_root_port_power(true)` that prints VID/PID on mount) — flash that first before chasing firmware. **Live hot-plug needs bigger USB-host task stacks (2026-06-07):** the IDF Host-Library enumeration driver (control transfers + config/string-descriptor parsing) runs ENTIRELY on the `usb_daemon` task stack inside `usb_host_lib_handle_events()` — 4096 B overflowed on the first live connect → DoubleException (`exccause 0x42`) with a smashed SP (the daemon stack can't even save exception state, so the backtrace is unreadable; the *named crashed task* is the tell). Bumped `usb_daemon` 4096→8192, CDC driver task + the deferred-work task (`usb_open`, later renamed `usb_worker`) 4096→6144/8192 in [esp_usb_host.cpp](controllers/lib/sfx_usb/usb/esp32/esp_usb_host.cpp). FreeRTOS stack-overflow detection is already maxed (`CANARY` + `WATCHPOINT_END_OF_STACK`) but a violent frame can leap past the end-of-stack watchpoint word → raw DoubleException instead of a clean panic, so the *only* real fix is sizing the stack. **Live unplug→replug restart — the USB auto-recovery timer (2026-06-07):** with the stacks sized, a live *replug* still PANIC-rebooted the hub, and crucially wrote **no coredump** (a panic that can't save its own dump ⇒ DoubleException on a smashed stack). Cause: every disconnect arms a 5 s recovery timer whose callback ran `EspUsbHost::resetBus()` — a root-port power-cycle **plus a blocking `vTaskDelay(500ms)`** — **directly on the FreeRTOS timer-service task** (`CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=3120 B`). The deep USB-HCD frame overflows that tiny stack (and/or fires mid-replug, yanking root-port power). **Auto-recovery is NEEDED** — the HubFX has a USB hub chip with downstream expander ports, and the ESP-IDF ext_port driver disables a downstream port after a single failed reset with no retry, so the power-cycle is the only way a wedged port comes back. Fix (2026-06-07): keep auto-recovery ON but make `recoveryTimerCb` do nothing heavy on the timer task — it only calls `requestBusReset()`, a shallow non-blocking `xQueueSend` of a `PendingWork{BusReset}` onto the `usb_worker` task (8 KB stack, the renamed/generalized ex-`usb_open` deferred-work task) where `resetBus()` actually runs. **Two separate things had to come off the 3120 B timer task, and the second is the non-obvious one:** (1) `resetBus()`'s deep HCD power-cycle (deferred to the worker); and (2) **the callback's own `SFX_LOG_WARN` — even a single DiagLog line overflows the timer stack**, because `DiagLog::emitLive → NativeUartStream::write → uart_write_bytes → xRingbufferSend` is ~2 KB deep (a 128-byte `vsnprintf` plus the UART ringbuffer). The first fix attempt (defer resetBus, keep the log) still crashed *on the log line* — `exccause 0x41 DebugException` = the end-of-stack watchpoint catching the overflow cleanly (a healthy-enough stack to save the coredump, which is how we finally saw the backtrace). So `requestBusReset()` counts dropped requests into a stat instead of logging. **Lesson: a FreeRTOS software-timer callback runs on the shared 3120 B timer-service task — do NO deep/blocking work AND NO DiagLog/UART logging there; queue to a worker task. The human-visible "bus reset" log now comes from `resetBus()` on the worker.** **Diagnosing a coredump-less reset:** `esp_reset_reason()` is logged at boot over the DiagLog wire (console is NONE) — `PANIC`/`BROWNOUT`/`INT_WDT`/`TASK_WDT` each point a different way; a PANIC with a *stale* coredump (SHA mismatch on `coredump hubfx`) means the dump couldn't be written ⇒ suspect a stack-overflow DoubleException, and bisect by disabling suspects (here: auto-recovery) rather than chasing an unreadable backtrace.
- **Audio source teardown is a Rule-15 cross-task hazard.** `WavState::source` (raw `IAudioSource*`) is read by the decoder task (`refillDrainBuffer`, Core 0) and freed by the producer/command task (`play()`/`stop()`/EOF). A rapid stop/start freed it mid-refill → `LoadProhibited@0` (found via coredump, 2026-05-31). NEVER call raw `destroyAudioSource(ws.source)`; always use `AudioMixer::destroyChannelSourceSafe(ws)` — it clears `active` then waits (seq_cst busy-flag handshake with the decoder's `decoderBusy`) before the destructor. Regression net: [tests/host/go_integration/engine_stress](tests/host/go_integration/engine_stress).
- **Servo intent is normalised; the role owns the calibrated travel (`SERVO_SET_POS_NORM` 0x55).** Effects drive a servo by a position FRACTION `[0, kPosNormFull=10000]` (0 = calibrated min-µs end, 10000 = max-µs end); `ServoActuatorRole::setNormalizedTarget` maps it LINEARLY onto the role's LIVE `[_minUs,_maxUs]` and reflects for REV via `setTarget`. So a caller never needs the limits and a re-calibration is honoured on the very next command. GunFx yaw/pitch convert their RC pulse → fraction at the edge; LandingFx deploy/retract send full/zero. This is the Rule 42 intent-verb model — don't reach back to absolute µs from an effect (that's how the "deploy stops short of the calibrated end" bug started). Wire-mirror: `roles.ServoSetPosNorm` / `CmdServoSetPosNorm` / `PosNormFull`.
- **`ServoActuatorRole::rebuildProfileLimits` ASSIGNS `_profile.min/max = _minUs/_maxUs`, it does not clamp.** The motion integrator (`MotionProfile1D`) clamps every target to `_profile.min/max`, which must EQUAL the role's calibration window. The old code only pulled the profile *inward* (min up / max down), so WIDENING a calibration left `_profile.maxUs` stuck at the prior narrower value and the servo never reached the new endpoint (the 2.21.2 fix; diag trace: Studio sent max 1620, role logged 1587 every time). When you add a limit setter, set the profile window in BOTH directions.
- **`CONFIG_RELOAD` does NOT re-attach an already-attached hub-local role.** `topo.attachRole` is a no-op when the role is already bound, so a Studio *Apply* (save `/hubfx.yaml` + reload) does NOT re-push `ports[].profile` into a live role. The ONLY path that updates a running servo's limits is the **live** `SERVO_SET_PROFILE` push (Studio's calibration Save / `SetPortProfile`). Persisted-but-not-live-pushed profile changes only take effect on the next **boot/attach** (fresh `emplace`). Diagnose servo-limit issues by subscribing and reading the `[servo] setprofile … min … max …` / `[servo] pos_norm …` instrumentation in `role_service.cpp`, not by re-Applying.

## Things to avoid

- Adding commands / features to [controllers/hubfx/pico/](controllers/hubfx/pico/) — frozen.
- Raw Pico SDK / ESP-IDF calls in cross-platform code under [controllers/lib/](controllers/lib/) — use `sfx_platform.h` macros. (Exception: platform-gated policy implementations, e.g. [sfx_storage/storage/esp32/](controllers/lib/sfx_storage/storage/esp32/), legitimately call native APIs inside `#if SFX_PLATFORM_ESP32`.)
- `volatile` for any inter-core variable.
- Returning `bool` from `*Client` command methods.
- Global pointer + setter injection for singletons.
- Hardcoding controller names in ad-hoc VS Code tasks — use the parameterized ones.
- Emojis, comments in `tasks.json` (must be pure JSON), or marketing prose in code.

## Memory (auto-loaded)

User-level notes live in `~/.claude/projects/c--data-code-scalefx/memory/` and are indexed by `MEMORY.md`. A project overview memory already tracks implementation status and known gaps — check it before asserting what is / isn't implemented.
