# ScaleFX — Claude Code Guide

Authoritative rulebook: [.github/copilot-instructions.md](.github/copilot-instructions.md) (30 rules).
Detailed workflows: [instructions/](instructions/) — numbered guides (`01-ARCHITECTURE.md`, `03-PROTOCOL-EXTENSION.md`, `04-CHANGE-PROPAGATION.md`, `05-BUILD-AND-FLASH.md`, `14-ONBOARD-COPROCESSOR.md`, `15-GENERIC-EXPANDER-REFACTOR.md`, `16-EXPANDER-BOARD-DESIGN.md`, `17-SYSTEM-SERVICES.md`, …).

This file is a compact index for Claude. When a rule below conflicts with copilot-instructions.md, that file wins — update this one to match.

## System at a glance

Multi-platform embedded effects: one **ESP32-S3 HubFX** master + up to N **Pico (RP2040)** expander boards (GunFX, LightFX, GearControl) over USB CDC at 6 Mbps. (Terminology note 2026-05-16: live code still uses `slave`/`SlaveType`/`BoardState::SLAVE`; the rename to `expander` lands atomically with the generic-expander refactor — see [instructions/15-GENERIC-EXPANDER-REFACTOR.md](instructions/15-GENERIC-EXPANDER-REFACTOR.md) and [instructions/17-SYSTEM-SERVICES.md](instructions/17-SYSTEM-SERVICES.md).)

- **Protocol:** Binary COBS, `[type:u8][tag:u8][len:u16LE][payload:0–512][crc8:u8]`, CRC-8 poly `0x07`, little-endian throughout.
- **HubFX Pico (RP2350) is OBSOLETE** — [controllers/hubfx/pico/](controllers/hubfx/pico/) is frozen reference. All new HubFX work goes in [controllers/hubfx/esp32s3/](controllers/hubfx/esp32s3/). (Rule 17.)
- **Expander wire surface (legacy per-board ranges, mostly superseded by ports/roles):** GunFX `0x01–0x2F`, LightFX `0x40–0x5F`, GearControl `0x60–0x7F`. These describe a *standalone expander's* own packets — NOT what the hub dispatches.
- **HubFX master dispatch map (the authoritative, conflict-validated allocation — every packet a `BoardOf<>` policy `ownsType()` on the hub; dispatch STOPS at the first policy whose `ownsType` returns true, so NO two policies may claim the same byte).** Validate against this before adding a packet; append at the next free value, never into a neighbour's block:
  - Ports `0x10–0x3F` · Roles `0x40–0x7F`
  - Expander `0x80–0x87` · Topology `0x88–0x8E`
  - Config `0x90–0x92` + `0xAC` · Storage `0x93–0xA3` + `0xA9` + `0xB0` · Codec `0xAA–0xAB`
  - **Effects:** LandingLight `0xB1–0xB6` · LightFX `0xB7–0xBD` · GearControl `0xBE–0xC6` **+ `0xD7–0xD9`** · EngineFX `0xC7–0xCB` · GunFX `0xCC–0xD2` · Alerts `0xD3–0xD6`
  - **Audio control `0xDA–0xE1`** · BatteryConfig `0xEE` · Board/Core lifecycle `0xEF–0xFF`
  - Free: `0x00–0x0F`, `0x8F`, `0xA4–0xA8`, `0xAD–0xAF`, `0xE2–0xED`.
  - Collision history (2026-05-22): (1) GEAR_RESET/CALIBRATE/CALIB_CANCEL `0xC7–0xC9` ate EngineFX START/STOP/STATUS → moved to `0xD7–0xD9`. (2) AudioPacket control `0x84–0x8B` overlapped Expander+Topology → moved to `0xDA–0xE1`. (3) BoardService claimed `0xEE–0xFF`, eating BatteryServicePolicy's `BATTERY_CONFIG 0xEE` → BoardService shrunk to `0xEF–0xFF`.
- **Error ranges:** Generic `0x00–0x1F`, GunFX `0x20–0x4F`, LightFX `0x50–0x5F`, GearControl `0x60–0x6F`, System `0xF0–0xFF`.

## Layout

```
controllers/
  <xxxfx>/pico/src/           firmware (Arduino-Pico, C++20) — being redesigned for generic-expander
  hubfx/esp32s3/              active HubFX (empty entry point — being rebuilt subsystem by subsystem)
  archive/                    frozen reference implementations (hubfx-esp32s3, hubfx-pico,
                              sfx_serial_legacy/{gunfx,lightfx,gearcontrol,hubfx}, sfx_boards_legacy/)
  lib/                        shared drivers + protocol
    sfx_platform/             OS/SDK abstraction (SFX_DELAY_MS, SfxMutex, SFX_FREE_HEAP, SPSC ring)
    sfx_serial/serial/        wire (CRC-8/COBS), DiagLog, client (SerialBus/BusClient/ResultQueue),
                              generic-expander wire (components/)
    sfx_peripherals/          LED, servo, PWM, I2C, INA226 + IndicatorServicePolicy / BatteryServicePolicy
    sfx_board/                BoardServer<...UserPolicies> + BoardServicePolicy + ComponentServicePolicy
                              + StreamWriter + CoreClient (server-side framework + master-side client)
    sfx_audio/ sfx_storage/ sfx_config/ sfx_usb/    subsystem drivers + their service policies
app/go/
  protocol/<mod>/             wire format mirrors of C++ headers (Go is now the source of truth
                              for the HubFx master protocol — firmware-side header is archived)
  api/<mod>.go                typed SDK
  engine/                     shared CLI/GUI dispatch engine
    handlers/<mod>/           CLI command handlers + parsers
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
app/go/scalefx-flash.exe build  <gunfx|lightfx|gearcontrol|hubfx> --no-clean
app/go/scalefx-flash.exe flash  <controller> --no-clean
cd app/go     && go build -o scalefx-cli.exe   ./cli/
cd app/go     && go build -o scalefx-flash.exe ./flash/
cd app/go/studio && wails build
```

After any C++ protocol change: `cd app/go && go build ./cli/` — compiling the Go side is the primary sync check.

## Non-negotiable rules (condensed — see copilot-instructions.md for rationale & examples)

1. **Docs are code (Rule 0).** Update the relevant README / `/instructions/` / `copilot-instructions.md` in the same commit as the code change.
2. **Protocol mirror (Rules 1, 19).** Every packet type / error code / command in `controllers/lib/sfx_serial/serial/<mod>/<mod>.h` has a twin in [app/go/protocol/<mod>/<mod>.go](app/go/protocol/). Never change one side alone.
3. **Add-a-command checklist (Rule 2, 7 files):** `<mod>.h` → `<mod>_pico.ino` → controller `README.md` → `protocol/<mod>/<mod>.go` → `api/<mod>.go` → `engine/handlers/<mod>/handler.go` → `engine/handlers/<mod>/parsers.go` (if it returns data).
4. **Client methods return `CommandResult`, never `bool` (Rule 3).** Query responses are implicit ACKs — resolve the tag in `onModulePacket()`. Skip `TAG_ASYNC`.
5. **Endianness is always little-endian (Rule 4).** `payload[0] | (payload[1] << 8)` in C++, `binary.LittleEndian.*` in Go.
6. **Physical units in names (Rule 5).** `voltage_mV`, `timeout_ms`, `current_mA` — never bare `voltage`. Applies to fields, params, method names, and wire-format comments.
7. **Use `BoardServer<...UserPolicies>` (Rule 6).** `using MyBoard = sfx_core::BoardServer<AudioServicePolicy<Mixer>, StorageServicePolicy<...>, ...>;` then `board.begin(name, ver, build)` + `board.process()` in loop(). `BoardServicePolicy` (lifecycle) and `IndicatorServicePolicy` (status LEDs) are prepended automatically — never add them to `UserPolicies`. New protocol-exposed subsystems become new `*ServicePolicy` types satisfying `sfx_core::SystemServicePolicy`. The legacy `SfxServer` / `addModuleHandler()` / `BusServer` / `CommandRouter` / `ICommandHandler` shapes were deleted — anything still referencing them is stale.
8. **Reuse before writing (Rule 7).** Check `controllers/lib/` first. Enhance the library instead of inlining hardware I/O in controller firmware. New I2C drivers extend `I2CDevice`.
9. **Indicator LEDs + error ranges (Rule 8).** `board.indicators().setErrorCondition(...)`. Every C++ error constant has a same-valued Go constant.
10. **Firmware version + build number (Rules 9, 10).** Bump `BUILD_NUMBER` on every flash (auto-incremented by `build_and_flash.py`). Bump `FIRMWARE_VERSION` proactively — MAJOR for wire-breaking, MINOR for additive, PATCH for logic-only. Verify after flash via `init` in the CLI.
11. **Backward-compatible payload extension (Rule 11).** Append-only; default value = no-op; server checks `len` to detect optional fields.
12. **Channel enable/disable (Rule 12).** Guard at top: `if (!_enabled) return ERROR_DISABLED;` Disabled = not persisted, distinct error code, reflected in STATUS flags.
13. **Error lifecycle (Rule 13).** Explicit reset command → transitions to UNKNOWN, clears reason, rejects actions until cleared. Not reboot-only.
14. **Singletons for board-unique resources (Rule 14).** C++11 thread-safe static local; private ctor; deleted copy/move; idempotent `begin()`. Current: `DiagLog`, `SdCardModule`, `FlashModule`, `AudioMixer`. Per-channel hardware arrays are NOT singletons.
15. **Dual-core thread safety (Rule 15, HubFX/ESP32-S3 & RP2350).** Any cross-core variable is `std::atomic<T>` with explicit memory order: `release` on writes, `acquire` on reads, `relaxed` for same-core counters. `volatile` is banned for cross-core (MMIO-only). Annotate ownership in a comment. Extract `atomic<T*>` to a local before `->`.
16. **Platform-native APIs (Rule 16).** Controller code uses the SDK directly. Shared `controllers/lib/` code uses [sfx_platform.h](controllers/lib/sfx_platform/platform/sfx_platform.h) macros (`SFX_DELAY_MS`, `SFX_FREE_HEAP`, `SfxMutex`, …) — never raw Pico SDK or ESP-IDF in shared code. `delay()`/`sleep_ms()`/`delayMicroseconds()` are banned on all cores.
17. **Policy-based templates for compile-time dispatch (Rule 18).** Server-side framework is `sfx_core::BoardServer<...UserPolicies>` (in `sfx_board`) — a variadic policy composer. Every protocol-exposed subsystem is a `*ServicePolicy` satisfying the `sfx_core::SystemServicePolicy` concept: `kCapabilityBits`, `begin(BoardServerBase*)`, `ownsType(type)`, `handle(type, payload, len)`, `update()`. Existing policies: `BoardServicePolicy`, `IndicatorServicePolicy`, `ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>`, `AudioServicePolicy<TMixer>`, `StorageServicePolicy<TPolicy>`, `BatteryServicePolicy<TBattery>`, `ConfigServicePolicy`. Other template templates: `ConfigStore<TSchema, TPool>`, peripheral collections. No virtual when only one instance exists per binary. **Codebase is C++20 (`-std=gnu++20` on every board)** — every policy/schema template is gated by a `concept` + `requires`-clause (`SystemServicePolicy`, `StoragePolicy`, `BatteryPolicy`, `ConfigSchema`, `GpioExpander`, `HwPwmExpander`, `LedBrightnessExpander`); when adding a new template, define its concept in the same header and gate with `requires`.
18. **Tests live in [tests/](tests/) (Rule 21).** Never under `controllers/*/test/` or `app/go/tests/`. Go diagnostic tools use `replace scalefx => ../../app/go` in `go.mod`.
19. **Release notes are mandatory (Rule 22).** Generate from git log of controller dir + `lib/` + `serial/<mod>/<mod>.h`. Categorize: New Features / Bug Fixes / Protocol Changes / Breaking (⚠️) / Internal. Explain MAJOR/MINOR/PATCH impact.
20. **Decoded event structs + decoders live in `engine/handlers/<mod>/` (Rule 19 extension).** Three-file split per board:
    - `types.go` — JSON-tagged structs + pure `Decode*` functions (no I/O, no formatting). One decoder, many consumers.
    - `format.go` — `Handler.FormatXxx(*Xxx)` methods that render decoded structs to the CLI `h.E.Out`.
    - `handler.go` — commands + `Register(eng)` that wires everything with **inline closures** (no `parseXxx`/`handleXxx` wrapper methods). Listeners are `engine.Observers[T]` — Studio adds via `.Add(fn)`; the CLI formatter is pre-seeded for the sync `status` path and fires only when observers are registered on the broadcast path (otherwise the 1 Hz broadcast is silent).
    - `parsers.go` — **CLI-only query-response renderers** (reply to `seq.status`, `slaves`, etc.). Never broadcast/async — those go through the Observers chain.
    Never re-decode packets in `studio/app.go` or `cli/*`. Studio forwards decoded structs as Wails events from `handlers.RegisterDefaults(eng) → *handlers.Registry`.
21. **No backward-compatibility scaffolding in refactors.** When restructuring existing code, delete dead fields / removed flags / "pre-vN" fallbacks outright. Rule 11 (append-only wire-format extension) still applies — that's protocol compat across firmware versions, not code compat across git revisions. Do not invent `XxxFieldPresent` booleans, keep deleted helpers as thin wrappers, or leave `// removed for back-compat` comments. One way to do it: the current way.
22. **No thin wrappers.** If a method only decodes + fires an observer (or only calls another function), inline it at the call site. `parseXxxStatus` / `handleXxxBroadcast` that wrapped a decode+Fire are banned — use the closure form in `Register()` directly.
23. **Studio config validation (Rule 23).** Every board tab in `app/go/studio/frontend/src/lib/tabs/` runs its config through a board-specific verifier (`<board>-verifier.ts` implementing `ConfigVerifier<T>`) and visually surfaces issues (`class:verify-error` / `class:verify-warn` bindings via a `sev(path)` helper). Save buttons open `<SaveConfigDialog verifyResult={...} />` which gates flash on zero errors. Reference: [light-verifier.ts](app/go/studio/frontend/src/lib/config/light-verifier.ts), [gearcontrol-verifier.ts](app/go/studio/frontend/src/lib/config/gearcontrol-verifier.ts).
24. **Tooltips + live push (Rule 24).** Every interactive setting in `app/go/studio/frontend/src/lib/` gets a `title=` tooltip (verb-led, with units + range). Every config field validates locally on change and debounce-pushes (~350ms) to the board on success — dedup via `lastPushed`, surface state via `.push-pending/.push-sent/.push-invalid`. Position/jog controls are commands (no debounce). Save still required to persist. Reference: [ServoWidget.svelte](app/go/studio/frontend/src/lib/components/ServoWidget.svelte).
25. **Shared-module commands are universal; peer capacity is dynamic (Rule 25).** Commands backed by a shared firmware module (`sfx_storage`, `sfx_config`) live in a `CmdGroup` with `Controller: ""` — never inside a board-filtered group. No `for k, v := range h.E.ConfigCommands()` merge inside board handlers. `Engine.SetControllerType(ct)` is the single setter that records peer type AND propagates `FileApi.SetPeerMaxPayload` (Pico 512 vs ESP32 2048) — never assign `e.ControllerType` directly except when clearing to `""`. Use named constants `api.PicoMaxPayload`/`Esp32MaxPayload`/`UploadHeaderSize`/`UploadChunkSize`, never inline `508`/`2044`.
26. **Per-board config YAML + auto-hydrate on connect (Rule 26).** Each controller stores config in its own YAML file: GearControl `/gearcontrol.yaml`, LightFX `/lightfx.yaml`, HubFX `/hubfx.yaml`, GunFX `/gunfx.yaml` (reserved), legacy fallback `/config.yaml`. The firmware schema's `defaultPath()` is the single source of truth — never embed the filename elsewhere. Studio maps `ControllerType → path` via [app.go:configPathFor](app/go/studio/app.go). Every board tab with a `BoardConfigDriver<T>` calls `autoLoadOnConnect(driver, ['<controllerType>'])` from `onMount` — [config-loader.ts](app/go/studio/frontend/src/lib/config/config-loader.ts) downloads via `DownloadConfig()`, runs `driver.parseYaml` → `driver.applyState` once per `(port × controllerType)`, and echoes parse/apply to the console (including a `debug` trace of the parsed state summary). Hardcoded defaults are fallback only when the board has no YAML yet. No progress dialog for auto-load. New tabs opt in when they gain a driver.
27. **Canonical YAML style — indented block sequences (Rule 27).** All emitters (Studio TS generators, reference `controllers/*/pico/config.yaml`, any new CLI author) emit sequence items 2 spaces under the parent key + continuations 4 spaces under. Parsers accept three input forms: indented block, YAML-spec compact form (item at same column as parent key, back-compat with older on-device files), AND **single-line flow collections** (`port: { kind: pwm, idx: 0 }`, `- { id: 0, state: "on" }`, `[900, 1200]` — nestable, must close on the same line) for hand-authored leaf objects — but never EMIT compact or flow form (a Studio Save round-trips flow → block). The two custom parsers (firmware `YamlParser::parseFlowNode`, Studio TS `parseFlowValue`) must stay in lock-step on accepted forms; Go uses `gopkg.in/yaml.v3` (full YAML, the hand-rolled `engine/config_schema.go` parser is archived). Canonical example in [controllers/lib/sfx_config/README.md](controllers/lib/sfx_config/README.md). The original studio bug that motivated this rule: compact-form files silently dropped retracts/pins/door_modes/battery because the TS parser broke at the first sibling-indent `- ` line.
28. **Shared servo calibration dialog (Rule 28).** Per-board servo configuration uses the shared [ServoCalibrationDialog](app/go/studio/frontend/src/lib/dialogs/ServoCalibrationDialog.svelte). Tabs render a compact summary (range/speed/REV) + `⚙ Calibrate Servo…` button; the dialog handles live jog, debounced cfg push, Save/Cancel. NEVER inline a servo panel (sliders for min/max/speed/accel/decel) inside a binding row. Migrating a tab? Delete the inline helpers outright (Rule 21) — no `// kept for back-compat` stubs.
29. **Battery card layout (Rule 29).** Boards with battery monitoring use the canonical card layout: `.batt-display` (bar + voltage + pct + LOW / CUTOFF FIRED warnings), then a `form-row` with the auto-cutoff/auto-deploy toggle + push-badge + Apply, then a `form-row` with chemistry + cell count + push-badge. Card lives in the LEFT column. Two live-push keys: `battery` (chem+cells) and `battery.cutoff` (toggle). Reuse the GearControl CSS classes verbatim (`batt-display`, `batt-bar-track`, `batt-bar-fill`, `batt-info`, `batt-voltage`, `batt-pct`, `batt-warn`).
30. **Mandatory board-prefix on CLI commands (Rule 30).** Every board command group sets `CmdGroup.Prefix`: LightFX → `light`, GearControl → `gear`, GunFX → `gun`, HubFX → `hub`. The CLI dispatcher (and Studio Console) require the prefixed form for board commands (`light:servo`, `gear:reset`, `gun:trigger`, `hub:slaves`); bare names error with `"requires a board prefix. Did you mean: …"`. Universal groups (Core, Firmware, Storage/Config) leave `Prefix` empty — `connect`, `init`, `status`, `file.list`, `sd.status`, `config.save`, etc. stay bare. Wire format is unchanged; the prefix is a UX layer that disambiguates name overlap (`servo`, `reset`, `enable`, `battery`) which becomes acute once a hub fans out to multiple slave types — see [13-PASSTHROUGH-ROUTING.md](instructions/13-PASSTHROUGH-ROUTING.md) §4.4. Studio's typed APIs (`LightFxApi.*`, `GearControlApi.*`, …) are unaffected; only text dispatch carries the prefix.
31. **Port direction is fixed at declaration; input count ≤ available UART peripherals (Rule 31).** Each port in a board's `kServoPorts` / `kPwmPorts` / `kHBridgePorts` / `kInputPorts` list has a hard-coded direction — there is **no runtime swap between input and output**. `Servo` / `Pwm` / `HBridge` are output-only port kinds; `Input` is the single input port kind and is **multi-modal at the role layer** (PPM pulse capture vs UART RX for SBUS / Jeti EX / CRSF — selected when a role attaches). Because each declared `InputPort` may need a UART peripheral at any moment, the **number of `InputPort`s a board declares MUST NOT exceed the number of UART peripherals available** on the platform (so role attachment can never starve on UART). ESP32-S3: 3 UARTs total, UART0 = console → max 2 inputs. RP2040/RP2350: 2 UARTs total, UART0 typically console → max 1 input (extra UARTs via PIO are not counted). HubFX rev currently declares 1 InputPort on IN_1 (GPIO5); IN_2..IN_12 are output-only `ServoPort` headers — IN_1 is the only header that can carry an inbound RC / SBUS / Jeti stream.
33. **Eliminate redundant template parameters — infer from carriers (Rule 33).** A class / helper / free function MUST NOT take a template parameter recoverable from another it already has. Every "carrier" type (mixer, board, schema, store) re-exports its template args as nested typedefs (`AudioMixer<TI2S, TCodec>` publishes `using I2SOutput = TI2S;` + `using Codec = TCodec;`) so downstream helpers go single-arg and recover the rest via `Carrier::Member`. When a helper needs board-local config keyed by a type (codec → pins/supply), use a **trait specialization** the board declares next to its pin map (`template <> struct CodecAdapter<TAS5825PCodec> { … };`), NOT a second template param — static dispatch, `if constexpr`-strippable primary template = sensible default. Every multi-platform service policy ships its `using XxxService = XxxServicePolicy<PlatformImpl>;` in the library so sketches never spell the policy. Free-function templates rely on argument deduction (`wireUploadExclusivity<Mixer>(board.policy<StorageService>())`). Don't reach for `std::function` / runtime hooks when a carrier typedef or trait specialization gives the same flexibility statically. Reference refactor: [esp_dual_core_audio.h](controllers/lib/sfx_audio/audio/esp_dual_core_audio.h).

## Key architectural touchstones

- **Handler macros** (`sfx_serial/serial/core/core.h`): `SFX_REQUIRE_LEN(n)`, `SFX_VALIDATE(cond, err)`, `SFX_DISPATCH(cb, ...)`, `SFX_HANDLE_CHANNEL_CMD(v, err, cb)`.
- **STATUS payload** = 22-byte core header (`counter:u32, uptime:u32, freeRam:u32, lastActivity_ms:u32, keepaliveCount:u32, boardState:u8, initFlags:u8`) + module callback data via `board.core().onStatusData(cb)`. Board states: `IDLE(0)`, `STANDALONE(1)`, `SLAVE(2)`, `DIRECT(3)`.
- **INIT_READY / IDENTIFY (0xFE)** — same length-prefixed payload (`nameLen:u8, name, verLen:u8, ver, platLen:u8, plat, cpuMHz:u32LE, freeRam:u32LE, buildNum:u32LE, capabilities:u32LE`). IDENTIFY does NOT activate hardware → used for safe type detection; INIT activates. HubFX auto-inits at boot (IDENTIFY suffices); slaves still need INIT. **Capabilities** is a Rule 11 append-only u32 bitmask, computed at compile time by `BoardServer<...>` as the OR of every policy's `kCapabilityBits` and seeded into `BoardServicePolicy` during `begin()`. Flags: `FLASH(1<<0)`, `SD(1<<1)`, `AUDIO(1<<2)`, `USB_HOST(1<<3)`, `ENGINE(1<<4)`, `CONFIG(1<<5)`, `SLAVE_BUS(1<<6)`, `BATTERY(1<<7)`. Mirrored in [protocol/core/core.go](app/go/protocol/core/core.go) as `CapFlash`/`CapSd`/… plus `HasCapability()` / `CapabilityNames()` helpers; surfaced through `Engine.Capabilities()` / `Engine.HasCapability(want)` and Studio's `DeviceCapabilities()` Wails binding. Studio's file manager (`FsStorageStatus`) gates flash/SD probes on these bits — never speculatively probe a backend the device does not advertise.
- **Response categories** (drives client design, see [instructions/01-ARCHITECTURE.md](instructions/01-ARCHITECTURE.md)):
  - **Instant** — `SFX_DISPATCH` → auto ACK/NACK; client gets `CommandResult` back from `sendCommand(...)`.
  - **Query** — server emits a typed response packet (no `SFX_DISPATCH`); client uses `sendQuery(reqType, payload, len, respType, out)` which captures the typed response in `out` and resolves the tag as `CommandResult::Ack()`.
  - **Long-Running** — immediate ACK on start; final state arrives via STATUS broadcast or async packets. Skip tag resolution when `tag == SfxWire::TAG_ASYNC`.
- **setup() sequence** (any controller): (1) `board.begin(prefix, version, buildNumber, ledPin, errPin)` — wires Serial, DiagLog, indicator pins, every policy, lifecycle callbacks, capability advertisement; (2) hardware init that needs to happen before INIT; (3) `board.policy<P>().bindXxx(...)` for policies that take external dependencies (battery sensor, USB registry, …); (4) optional `board.addExpectedI2CDevice(...)` + `board.enableI2CScan(Wire)`; (5) `board.onInit([](mode, flags){…})` / `board.onShutdown([](){…})` for controller-specific hooks. Then `loop()` is one line: `board.process()`.
- **Go engine** is shared between CLI and Wails Studio. Each controller exposes `Register(eng *Engine)` that wires status parser, async parsers, and command group. Called by `engine/handlers/handlers.go:RegisterDefaults()`. Don't duplicate dispatch logic in `studio/app.go`.
- **Config persistence** uses `ConfigServicePolicy` (multi-store path-routed) + `ConfigStore<TSchema>` — YAML-first per-device schema in LittleFS. Every controller that includes `ConfigServicePolicy` in its `BoardServer<...>` pack gets `config.reload/save/status` automatically.

## Board GUID

Every ScaleFX board exposes a stable hardware-derived **GUID** so masters can tell two boards of the same kind (e.g. two LightFX expanders) apart and persist per-board state across reconnects.

- **Source.** `sfxGetBoardId(out, maxLen)` in [sfx_platform.h](controllers/lib/sfx_platform/platform/sfx_platform.h) produces an **8-char uppercase hex string** (4 bytes). Pico: last 4 bytes of `pico_unique_board_id_t` (8-byte OTP flash unique-id). ESP32: last 4 bytes of the factory MAC (`esp_efuse_mac_get_default`). Both are immutable per silicon and survive reflashing.
- **Surface on the wire.** `BoardServerBase::buildDeviceName(prefix)` in [board_server.cpp](controllers/lib/sfx_board/server/board_server.cpp) emits `deviceName = "<Prefix>-<last 4 hex chars>"` — e.g. `"GunFx-3C4D"`. That suffix is the **canonical GUID** broadcast in `INIT_READY` / `IDENTIFY` payloads (16 bits / 65 536 values). Sufficient at ScaleFX scale (a hub hosts ≤ 2 expanders today) but not collision-proof at fleet scale — log a GUID collision if it ever fires.
- **Extracting the GUID.** Strip everything up to and including the last `-` in `deviceName`. Friendly per-board *aliases* live in `/hubfx.yaml`'s `expanders:` block (alias → GUID), not on the board — see the alias bullet below. (The old `/board.yaml` `BoardIdentifier` label was retired with the generic-expander refactor.)
- **Master-side use.** `ExpanderServicePolicyT<>` on HubFX issues `IDENTIFY` to every freshly-mounted CDC device, decodes the response, extracts the GUID suffix, and stores the spec in a GUID-keyed history slot. This is what lets the master re-apply cached per-board state when a board reconnects on a different USB port.
- **Battery telemetry.** A BATTERY-capable expander (e.g. LightFX/GunFX with `BatteryServicePolicy<AdcDividerBatteryT<…>>`) appends a battery section to its STATUS via `onStatusData`. The hub's `ExpanderServicePolicyT<>` polls each Ready such expander's `STATUS_REQ` on a slow cadence (`kBatteryPollIntervalMs`), decodes the tail (offset `STATUS_CORE_HEADER_SIZE`), caches it per slot, and appends it to `EXPANDER_SYSTEM_INFO_RESP` (Rule 11) — surfaced by the CLI `system-info`. Assumes the expander's `onStatusData` emits the battery section FIRST (true for the stock LightFX/GunFX boards).
- **Config aliases for expander ports.** `/hubfx.yaml`'s `expanders:` block declares each remote board once as `alias → guid` + a nested `ports:` (port→role) list (parsed in [hubfx_config.h](controllers/hubfx/esp32s3/src/config/hubfx_config.h), flattened into the master port table with each port stamped with the board GUID). Effect sub-files address ports by `board: <alias>` (resolved to the GUID by `portRefFromNode` against a table seeded when `/hubfx.yaml` loads first; `guid:` raw form still works). Hub-local roles attach at boot; expander roles attach (and re-attach on reconnect) via `ExpanderService::onReady(...)` — NOT `onIdentified`, which fires before `Handshake::Ready` and gets its forwarded commands dropped. See [instructions/19-HUBFX-CONFIG-SCHEMA.md](instructions/19-HUBFX-CONFIG-SCHEMA.md). Aliases are config-only (never on the wire); the CLI `topo-ports` now annotates each port with its attached role.
- **Adding a stronger GUID later** (16-bit collision concern): append a full-width `[guidLen:u8][guid:N]` field to the `INIT_READY` / `IDENTIFY` payload — Rule 11 append-only, old masters keep working. Producer: `sfxGetBoardId` already returns 8 hex chars; just emit them all instead of slicing to 4.

## Specific gotchas

- **BOOTSEL is command `0xF9`** (binary packet), not just a button hold. Flash CLI sends it, then waits for the RPI-RP2 USB drive. `BUILD_NUMBER` auto-increments on every build even if flash verification times out — trust the `INIT_READY` buildNum, not the source define.
- **ESP32-S3 esptool is v5.2.0 standalone (~12 MB), gitignored.** Get it via `scalefx-flash tools download`; it's searched in workspace `tools/esptool/`, next to the binary, then `PATH`.
- **Upload protocol — WINDOWED (mode=2) is the default; BATCH (mode=3, formerly STREAM) is the fast path.** WINDOWED uses per-window ACK + flow control + per-chunk CRC-16, default window size 32 (~64 KB). BATCH sends raw 512 KB segments with per-segment ACK — fastest for large files. Both modes now share a **single-core** pipeline: 64 KB PSRAM fill buffer, synchronous SD write from the main loop (the old dual-core SPSC-ring + writer-task path was retired). Uploads are **exclusive on HubFX** — the main loop skips audio/engine/USB/diagnostics while `storageServer.isUploadActive()` is true, and `onUploadStart`/`onUploadEnd` fires in both modes so firmwares can stop/resume competing subsystems. See [instructions/10-UPLOAD-PROTOCOL-REFACTOR.md](instructions/10-UPLOAD-PROTOCOL-REFACTOR.md).
- **`LANDING_LIGHT_BIND` (0x52) byte 2 is `channelMask`, not a single channel ID** — breaking change v0.8→v0.9. Bit N → LED channel N+1, up to 8 channels per group, servo optional. See [instructions/11-LANDING-LIGHT-GROUPS.md](instructions/11-LANDING-LIGHT-GROUPS.md).
- **AudioTools `InputMixer<float>` is broken** (accumulator bug); `SineWaveGenerator` has an amplitude bug. Workaround: keep the full pipeline in `int16_t`. See [instructions/08-AUDIOTOOLS.md](instructions/08-AUDIOTOOLS.md).
- **Console output size formatting** (CLI + parsers, see [instructions/09-CONSOLE-OUTPUT.md](instructions/09-CONSOLE-OUTPUT.md)): `<1 KB → B`, `<1 MB → KB`, `<1 GB → MB`, else `GB`. Match this exactly when writing new parsers.
- **`INA226::begin()` is strict on identity (not lenient).** Reads MFG/DIE IDs before any write and returns `false` on non-canonical chips (`!= 0x5449 / 0x2260`) without touching the chip's registers. Counterfeits at INA addresses have been observed to corrupt OTHER chips on the shared I²C bus when written to — HubFX rev hit this with a clone @ 0x40 wedging the PCA9685 @ 0x70. Boot diag still surfaces `bootMfgId()` / `bootDieId()` for the clone via `isCanonical()`. Full investigation + bisection trail in [instructions/18-HUBFX-INA-CLONE-WEDGE.md](instructions/18-HUBFX-INA-CLONE-WEDGE.md).

## Things to avoid

- Adding commands / features to [controllers/hubfx/pico/](controllers/hubfx/pico/) — frozen.
- Raw Pico SDK / ESP-IDF calls in [controllers/lib/](controllers/lib/) — use `sfx_platform.h` macros.
- `volatile` for any inter-core variable.
- Returning `bool` from `*Client` command methods.
- Global pointer + setter injection for singletons.
- Hardcoding controller names in ad-hoc VS Code tasks — use the parameterized ones.
- Emojis, comments in `tasks.json` (must be pure JSON), or marketing prose in code.

## Memory (auto-loaded)

User-level notes live in `~/.claude/projects/c--data-code-scalefx/memory/` and are indexed by `MEMORY.md`. A project overview memory already tracks implementation status and known gaps — check it before asserting what is / isn't implemented.
