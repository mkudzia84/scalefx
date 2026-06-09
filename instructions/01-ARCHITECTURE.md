# System Architecture

> **REFERENCE DOCUMENT:** Read this to understand how the system works before making changes.
>
> **See also: [32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md)** — Mermaid diagrams of the four core subsystems (storage / audio / ports-roles-topology / effects→ports) on the current `BoardServer<...UserPolicies>` codebase. Read it alongside this doc.

---

## System Topology

One **ESP32-S3 HubFX master** (pure ESP-IDF, no Arduino) runs every effect. Up to N **Pico (RP2040) expander boards** (LightFX, GearControl, …) attach over USB CDC at 6 Mbps. Expanders are *thin*: they only expose physical **ports** and let the hub attach **roles** and drive them — all effect logic (gun firing, gear sequencing, LED programs, landing lights, audio, engine) lives in the hub's effect `*ServicePolicy` classes under `controllers/hubfx/esp32s3/src/effects/`.

```
┌──────────────────────────────────────────────────────────────────────────┐
│                       HubFX master (ESP32-S3, pure ESP-IDF)               │
│                                                                          │
│   BoardServer< BoardServicePolicy, IndicatorServicePolicy,               │
│                PortServicePolicy, RoleServicePolicy,                      │
│                StorageServicePolicy<Esp32StoragePolicy>,                  │
│                AudioServicePolicy<Mixer>, EngineServicePolicy,            │
│                GunFxServicePolicy, LandingLightServicePolicy,             │
│                GearControlServicePolicy, … , TopologyServicePolicy >      │
│                                                                          │
│   • all effect logic runs here       • USB-HOST enumerates expanders     │
│   • TopologyService forwards role drive/query/telemetry to expanders     │
└───────────────┬──────────────────────────────────┬───────────────────────┘
        USB CDC │                          USB CDC │
                ▼                                  ▼
┌──────────────────────────────┐   ┌──────────────────────────────────────┐
│ LightFX Pico (thin expander) │   │ GearControl Pico (thin expander)     │
│ BoardOf<…, PortCapacity<…>>  │   │ BoardOf<…, PortCapacity<…>>          │
│  • 8 PWM ports (LED chans)   │   │  • 7 servo ports (door + yaw)        │
│  • 3 servo ports             │   │  • 3 H-bridge ports (gear motors,    │
│  • ADC battery sensor        │   │    each w/ INA226 stall sensor)      │
│  exposes ports → hub attaches│   │  exposes ports → hub attaches        │
│  LedAnimator / ServoActuator │   │  ServoActuator / BiDcMotor roles     │
└──────────────────────────────┘   └──────────────────────────────────────┘
```

> **Terminology:** live code still says `slave`/`SlaveType`/`BoardState::SLAVE`; the rename to `expander` is deferred backlog. Grep the code for the current name.

---

## Packet Format

```yaml
Binary_Packet:
  structure: "[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]"
  fields:
    - name: "type"
      size: 1
      description: "Packet type identifier"
    - name: "tag"
      size: 1
      description: "Correlation tag (1-255 for request/response, 0 for async)"
    - name: "len"
      size: 2
      description: "Payload length, u16 little-endian (0-512, protocol max 65535)"
    - name: "payload"
      size: "0-512"
      description: "Command-specific data (MAX_PAYLOAD_SIZE = 512)"
    - name: "crc8"
      size: 1
      description: "CRC-8 over type+tag+len(2)+payload"

CRC8:
  polynomial: 0x07
  initial: 0x00
  implementation: |
    uint8_t crc8(const uint8_t* data, size_t len) {
        uint8_t crc = 0;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int j = 0; j < 8; j++) {
                crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
            }
        }
        return crc;
    }

COBS_Encoding:
  purpose: "Eliminate 0x00 bytes from data stream"
  delimiter: 0x00
  wire_format: "[COBS_encoded_data][0x00]"
```

---

## Packet Type Registry

```yaml
Core_Packets:  # 0xEF-0xFF - All controllers
  STATUS_UPDATE: { type: 0xEF, direction: "S→C", payload: "[source:u8][updateType:u8][data:variable]", notes: "async, unsolicited, requires VERBOSE flag" }
  INIT:        { type: 0xF0, direction: "C→S", payload: "[mode:u8][flags:u8] (optional, backward-compatible)" }
  SHUTDOWN:    { type: 0xF1, direction: "C→S", payload: "none" }
  KEEPALIVE:   { type: 0xF2, direction: "C→S", payload: "none" }
  INIT_READY:  { type: 0xF3, direction: "S→C", payload: "[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]" }
  STATUS:      { type: 0xF4, direction: "S→C", payload: "[counter:u32LE][uptime:u32LE][freeRam:u32LE][moduleData...]" }
  ERROR:       { type: 0xF5, direction: "S→C", payload: "[error_code:u8][message...]" }
  ACK:         { type: 0xF6, direction: "S→C", payload: "none" }
  NACK:        { type: 0xF7, direction: "S→C", payload: "[error_code:u8][reason_text...]" }
  REBOOT:      { type: 0xF8, direction: "C→S", payload: "none" }
  BOOTSEL:     { type: 0xF9, direction: "C→S", payload: "none" }
  STATUS_REQ:  { type: 0xFA, direction: "C→S", payload: "none" }
  I2C_SCAN:    { type: 0xFB, direction: "C→S", payload: "none" }
  I2C_SCAN_RESULT: { type: 0xFC, direction: "S→C", payload: "[numExp:u8][N×(addr,found,id)][numExtra:u8][M×addr]" }
  LOG_MESSAGE: { type: 0xFD, direction: "S→C", payload: "[level:u8][millis:u32LE][message:str]", notes: "async, unsolicited" }
  IDENTIFY:    { type: 0xFE, direction: "C→S", payload: "none", response: "IDENTIFY (0xFE) with INIT_READY payload format, no state change" }
  DIAG_HISTORY: { type: 0xFF, direction: "C→S", payload: "none", response: "ACK + buffered LOG_MESSAGE packets" }
```

### Effect / port / role / topology packets

There are **no per-board packet ranges** any more. The hub owns the whole map: each effect / subsystem `*ServicePolicy` claims its own packet bytes via `ownsType()`, dispatch stops at the first policy that owns the byte (so two policies must never claim the same byte). The authoritative, conflict-validated allocation lives in **`CLAUDE.md` / `.github/copilot-instructions.md` → "HubFX master dispatch map"** — before adding a packet, grep the actual `ownsType()` predicates in `controllers/hubfx/esp32s3/src/`, never trust a static table here. Highlights:

```yaml
Ports:        0x10-0x3F          # PortServicePolicy
Roles:        0x40-0x7F          # RoleServicePolicy (per-role codecs)
Expander:     0x80-0x87          # ExpanderServicePolicy
Topology:     0x88-0x8F + 0xA6-0xA7   # TopologyServicePolicy — role forward/query/event/bulk-attach
Storage:      0x93-0xA5 + 0xA9 + 0xB0
Audio:        0xDA-0xE1
Effects:      LandingLight 0xB1-0xB6 · LightFX 0xB7-0xBD · GearControl 0xBE-0xC6
              EngineFX 0xC7-0xCB · GunFX 0xCC-0xD2 + 0xE2-0xE5 · Alerts 0xD3-0xD6
```

A *standalone Pico expander's own* packets (its own ports/roles surface) are a separate concern from what the hub dispatches — see [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md).

---

## Server Framework — `BoardServer<...UserPolicies>`

Every board (hub master AND Pico expander) is composed from **one** variadic class: `sfx_core::BoardServer<TStream, ...UserPolicies>` (`controllers/lib/sfx_board/server/board_server.h`). It owns the wire stream, the COBS frame reader, the policy tuple, the ACK/NACK wire helpers, the device-name + I²C-scan helpers, the connection-timeout watchdog, and the lifecycle callbacks. The legacy `SfxServer` / `BusServer` / `CoreCommandServer` / `CommandRouter` / `ICommandHandler` / per-board `*Server` / `*Client` shapes are **deleted** — anything still referencing them is stale.

### SystemServicePolicy — the unit of composition

Every protocol-exposed subsystem is a `*ServicePolicy` satisfying the `sfx_core::SystemServicePolicy` concept:

```cpp
struct MyServicePolicy {
    static constexpr uint32_t kCapabilityBits = CoreCapability::SOMETHING;  // OR'd into IDENTIFY caps
    bool begin(sfx_core::BoardServerBase* ctx);          // cache context, init state
    bool ownsType(uint8_t type) const;                   // claim a packet-type byte
    sfx_core::CommandHandleResult handle(uint8_t type,   // handle a claimed packet
                                         const uint8_t* payload, size_t len);
    void update();                                       // ticked from loop()
};
```

### Dispatch — first-owner-wins

`BoardServerBase` reads COBS frames off the wire, verifies CRC-8, extracts `[type, tag, payload, len]`, then offers each packet to **every** policy's `ownsType()` in pack order. The **first** policy whose `ownsType()` returns true gets `handle()` — there is no per-board packet-range routing. Because dispatch STOPS at the first owner, **no two policies may claim the same byte** (see the dispatch map in `CLAUDE.md`).

```
                       BoardServer composition (pack order)
┌─────────────────────────────────────────────────────────────────────┐
│ BoardServicePolicy   (auto)  — INIT/SHUTDOWN/REBOOT/BOOTSEL/KEEPALIVE │
│                                STATUS, IDENTIFY, DIAG_HISTORY, I2C    │
│                                onStatusData(cb) — module status append│
│ IndicatorServicePolicy (auto)— connection / error LEDs               │
│ PortServicePolicy    (BoardOf)— physical port registry (0x10–0x3F)   │
│ RoleServicePolicy    (BoardOf)— attach + drive roles (0x40–0x7F)     │
│ …UserPolicies…               — Storage / Audio / Engine / GunFx /    │
│                                Landing / Gear / Topology / Config …   │
└─────────────────────────────────────────────────────────────────────┘
   board.core()        → BoardServicePolicy&     (lifecycle / status / INIT)
   board.indicators()  → IndicatorServicePolicy& (connection / error LEDs)
   board.policy<P>()   → P&                       (any policy in the pack)
```

`BoardServicePolicy` (lifecycle) and `IndicatorServicePolicy` (status LEDs) are **prepended automatically** by `BoardServer` — never list them in `UserPolicies`.

### BoardOf<> — the expander shorthand

Most boards (every Pico expander, and the hub) are declared via `sfx_core::BoardOf<TBoard, TStream, PortCapacity<NServo,NPwm,NHBridge,NInput>, ...ExtraPolicies>` (`board_of.h`), a CRTP base that turns a board's static `kServoPorts` / `kPwmPorts` / `kHBridgePorts` / `kInputPorts` descriptor tuples into a wired `BoardServer<...>`. `BoardOf` auto-prepends, in order:

```
BoardServicePolicy → IndicatorServicePolicy → PortServicePolicy → RoleServicePolicy → …ExtraPolicies…
```

`PortCapacity<…>` sizes the per-kind port registry binding arrays exactly (over-generous caps waste DRAM — each slot embeds a role `std::variant`). See [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) for the full walkthrough and the live LightFX / GearControl sketches.

### Roles & transparent expander dispatch (Rule 58)

A board's physical ports carry **roles** (`ServoActuator`, `LedAnimator`, `Heater`, `DcMotor`, `BiDcMotor`, …) attached at runtime by `RoleServicePolicy`. The hub drives, queries, and receives telemetry from an **expander's** roles *transparently* via `TopologyServicePolicy` — the role packet travels as **opaque bytes** addressed by `PortRef{guid, kind, idx}` (`guid==""` → hub-local) over four role-agnostic wire primitives:

| Primitive | Type | Purpose |
|-----------|------|---------|
| `TOPOLOGY_ROLE_FORWARD` | `0x8F` | command (ACK/NACK) |
| `TOPOLOGY_ROLE_QUERY` / `RESPONSE` | `0xA6` / `0xA7` | request-response query |
| `TOPOLOGY_ROLE_EVENT` | `0x8E` | GUID-tagged async telemetry |
| `ROLE_BULK_ATTACH` | `0x57` | declarative full role set at connect |

The hub never decodes the inner role packet. To expose a new role: add its codec (`protocol/roles` + a firmware role handler) + one line in `role_registry.h`'s `roleKindFor<>()` + a one-line `RoleTarget` wrapper on the Go side — **no per-role transport, forward, event, or hub `switch`.** See [32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md) § ports-roles-topology and `controllers/lib/sfx_board/server/role_registry.h`.

### Singletons (Board-Unique Resources)

Any class with exactly one instance per board — whether wrapping a hardware peripheral or a logical registry — is implemented as a singleton using C++11 thread-safe static local initialization.

```
DiagLog::instance()          — Board-wide diagnostic logging (lib/sfx_platform/platform/)
  │ Ring buffer → COBS LOG_MESSAGE packets
  │ Mutex-protected for dual-core safety
  │ Compile-time strippable via SFX_ENABLE_DIAG_LOG=0
  │ Accessed via SFX_LOG_INFO/WARN/ERROR/DEBUG macros
  │
SdCardModule::instance()     — Single SD card (lib/sfx_storage/)
  │ Thread-safe access (ESP32: VFS-FAT mutex; Pico: pico mutex)
  │ Used by: StorageServicePolicy, ConfigStore, AudioMixer
  │
FlashModule::instance()      — Single onboard LittleFS flash (lib/sfx_storage/)
  │ Thread-safe access
  │ Used by: StorageServicePolicy, ConfigStore
  │
AudioMixer::instance()       — Single I2S audio output (lib/sfx_audio/audio/)
  │ Runs on Core 1, thread-safe buffer access
  │ Uses SdCardModule singleton internally
  │ Platform-conditional buffer sizes (Pico vs ESP32)
```

**Pattern:**
```cpp
class MyModule {
public:
    static MyModule& instance() {
        static MyModule inst;  // C++11 thread-safe
        return inst;
    }
    MyModule(const MyModule&) = delete;
    MyModule& operator=(const MyModule&) = delete;
private:
    MyModule();  // Private
};
```

**Qualifying criteria:** single hardware resource per board, single logical registry, or board-wide service used from multiple modules/cores.

**NOT singletons:** service policies (`*ServicePolicy`), per-channel hardware (servo / LED / PWM port arrays), effect units (`GunUnit`, `LandingGroup`).

### StatusDataCallback

Modules provide board-specific STATUS data via a callback registered on `BoardServicePolicy` (`board.core()`):

```cpp
using StatusDataCallback = std::function<size_t(uint8_t* buffer, size_t maxLen)>;

// STATUS response = 22-byte core header + module callback data
// Core header: [counter:u32LE][uptime:u32LE][freeRam:u32LE][lastActivity_ms:u32LE]
//              [keepaliveCount:u32LE][boardState:u8][initFlags:u8]

board.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
    buf[0] = myFlag;
    SfxWire::putU16LE(&buf[1], myValue);
    return 3;  // bytes written
});
```

---

## Connection Flow (CLI → Controller)

The CLI uses **IDENTIFY before INIT** to discover the board type without triggering side effects. This prevents expensive re-initialization on boards that auto-initialize on boot (HubFX).

### Sequence

```
CLI                                    Controller
 │                                       │
 │  Open serial port (6Mbps)             │
 │  Drain boot garbage                   │
 │                                       │
 │  IDENTIFY (0xFE)                      │
 │ ─────────────────────────────────────→│
 │                                       │  (no state changes)
 │  IDENTIFY response (0xFE)             │
 │←─────────────────────────────────────│
 │                                       │
 │  Parse name → detect controller type  │
 │                                       │
 │  [HubFX]  → Done (auto-initialized)  │
 │  [Slave]  → Send INIT (0xF0)         │
 │ ─────────────────────────────────────→│
 │                                       │  (init callbacks fire)
 │  INIT_READY (0xF3)                    │
 │←─────────────────────────────────────│
```

### Key Points

- **IDENTIFY (0xFE)** returns the same payload as INIT_READY (name, version, platform, build) but does NOT trigger init callbacks or state changes
- **HubFX (autonomous hub)**: auto-initializes on boot (codec, audio, engine, expanders); INIT from CLI would cause full re-initialization — only IDENTIFY is needed
- **Expander controllers** (LightFX, GearControl): require INIT to activate hardware; CLI sends IDENTIFY first to detect type, then INIT to start
- **Legacy fallback**: if IDENTIFY fails (NACK or timeout), CLI falls back to INIT which works on all firmware versions

### Implementation

- **Firmware**: `BoardServicePolicy::sendIdentify()` — same payload as `sendInitReady()` but with the IDENTIFY packet type and no state change
- **Go**: the connect path in `app/go/client/` (`Client.Connect` / `cmd_core.go` in `app/go/console/`) sends IDENTIFY, decodes the board name → controller type, then conditionally INITs

---

## Response Handling Design

### Tag Correlation

Every command the Go side sends gets a unique correlation tag (1-255, wrapping; `0` = `TAG_ASYNC`). The board echoes the tag on its response (ACK, NACK, or typed data response) so concurrent in-flight commands resolve to the right waiter. The tag plumbing lives in `app/go/protocol/connection.go` (`Connection.NextTag` / tag waiters — all lock-guarded, Rule 56); the firmware echoes the request tag from `BoardServerBase`.

On the Go side, a typed `client` method wraps each command (`app/go/client/<mod>.go`):

- **Instant** → `c.sendExpectACK(pkt)` — blocks for ACK/NACK.
- **Query** → `c.sendForResp(pkt, respType)` — blocks for the typed RESP packet (which doubles as the ACK).
- **Async telemetry** → not a command result at all; delivered via the events stream (`app/go/client/events.go`).

### Three Response Categories

Every command falls into one of three categories based on how the firmware responds. The category is decided in the owning policy's `handle()`:

#### 1. Instant — Direct ACK/NACK

The policy fully processes the command and returns `CommandHandleResult::Ack()` / `Nack(err)` (or uses an `SFX_DISPATCH`-style helper). Go side blocks via `sendExpectACK`.

```cpp
// Firmware — owning policy handle()
case GunPacket::GUN_FIRE_ONCE: {
    SFX_REQUIRE_LEN(1);
    handleFireOnce(payload, len);
    return CommandHandleResult::Ack();
}
```
```go
// Go — client/gunfx.go
func (g *Gun) FireOnce(id byte) error {
    return g.c.sendExpectACK(gunfx.CmdFireOnce(id))
}
```

#### 2. Query — Typed Data Response

The policy emits a typed RESP packet (echoing the request tag) instead of a plain ACK; that response IS the implicit ACK. Go side blocks via `sendForResp`.

```cpp
// Firmware — handle() sends a typed RESP with the current tag
case GunPacket::GUN_STATUS_REQ:
    handleStatusReq();                  // builds + sends GUN_STATUS_RESP
    return CommandHandleResult::Handled;
```
```go
// Go — client decodes the RESP payload
resp, err := g.c.sendForResp(gunfx.CmdStatusReq(), gunfx.StatusResp)
```

#### 3. Long-Running — ACK then monitor

The policy ACKs immediately ("accepted") and the physical operation runs across many `update()` ticks; completion arrives via the STATUS broadcast or an async event packet (consumed through the Go events stream). Gear deploy / landing-light deploy / auto-fire are long-running.

---

## Indicator LED Standard

`IndicatorServicePolicy` (auto-prepended by `BoardServer` / `BoardOf`) drives two indicator LEDs whose pins are passed to `board.begin(stream, ver, build, connectionPin, errorPin)` (Pico expanders use their own GPIO, e.g. LightFX GP24/GP25). Set error/warning state from any policy or the sketch via `board.indicators().setErrorCondition(...)` / `.setWarningCondition(...)`; `update()` is ticked automatically by `board.process()`.

```yaml
Connection_LED:
  waiting_for_init: "Blink ~500ms"
  connected:        "Solid ON"
  connection_lost:  "OFF"
Error_LED:
  normal:  "OFF"
  error:   "Blink fast (setErrorCondition)"
  warning: "Blink slow (setWarningCondition)"
```

---

## Handler Macros (SFX_*)

A policy's `handle()` switch uses macros from `serial/core/core.h` for length-checking and validation:

```cpp
SFX_REQUIRE_LEN(n)         // return Nack(MISSING_PARAMETER) if len < n
SFX_VALIDATE(cond, err)    // return Nack(err) if !cond

CommandHandleResult MyServicePolicy::handle(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case MyPacket::SET_THING: {
            SFX_REQUIRE_LEN(3);
            uint8_t  id  = payload[0];
            uint16_t val = SfxWire::getU16LE(payload + 1);
            SFX_VALIDATE(id < kMaxThings, MyError::INVALID_ID);
            applyThing(id, val);
            return CommandHandleResult::Ack();
        }
        default:
            return CommandHandleResult::NotMyCommand;
    }
}
```

---

## Data Flow (Firmware dispatch)

```
                    Wire stream (UART0 / USB CDC)
                        │
                        ▼
┌───────────────────────────────────────────────────────────────┐
│              BoardServerBase (inside board.process())          │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ 1. PacketReader: read bytes until 0x00 delimiter         │  │
│  │ 2. COBS decode                                           │  │
│  │ 3. Verify CRC-8                                          │  │
│  │ 4. Extract type, tag, payload, len                       │  │
│  └─────────────────────────────────────────────────────────┘  │
│                         │                                      │
│                         ▼                                      │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ For each policy in pack order (BoardServicePolicy first):│  │
│  │   if policy.ownsType(type):                              │  │
│  │       result = policy.handle(type, payload, len)         │  │
│  │       send ACK / NACK / (typed RESP already sent); STOP  │  │
│  │ No policy owns it → NACK INVALID_COMMAND                 │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                                │
│  Then: EffectClock::latch() + every policy.update()           │
└───────────────────────────────────────────────────────────────┘
```

> **First-owner-wins:** dispatch stops at the first policy whose `ownsType()` returns true. Two policies claiming the same byte is a silent bug — the later policy never runs. Validate every new packet against the dispatch map in `CLAUDE.md` and grep the real `ownsType()` predicates.

---

## Serial / Board Library File Structure

```yaml
Wire_Protocol:
  root: "controllers/lib/sfx_serial/serial/"
  files:
    serial.h:             "Umbrella header"
    core/core.h:          "CommandHandleResult, SerialError, CorePacket, SFX_* macros"
    core/core.cpp:        "Core payload encode/decode"
    wire.h / wire.cpp:    "SfxWire — CRC-8 / COBS / endian helpers (getU16LE/putU16LE)"
    packet_reader.h:      "PacketReader — byte-stream → framed packet"
    diag_log.h/.cpp:      "DiagLog singleton — LOG_MESSAGE / DIAG_HISTORY over the wire"
    client/bus.h:         "SerialBus — client-side COBS transport (master USB-host side)"
    client/result_queue.h:"ResultQueue — tag-correlated command/response matching"

Board_Framework:
  root: "controllers/lib/sfx_board/server/"
  files:
    board_server.h:       "BoardServer<TStream, ...UserPolicies> — the composer"
    board_of.h:           "BoardOf<TBoard, TStream, PortCapacity<…>, …> — expander shorthand"
    board_service.h:      "BoardServicePolicy — INIT/STATUS/IDENTIFY lifecycle (auto)"
    port_service.h:       "PortServicePolicy — physical port registry (auto via BoardOf)"
    role_service.h:       "RoleServicePolicy — attach + drive roles (auto via BoardOf)"
    role_registry.h:      "roleKindFor<>() / forEachAttachedRole — the ONE role enumeration map"
    effect_clock.h:       "EffectClock singleton — latched once per process()"
```

Effect `*ServicePolicy` classes live with their effect under `controllers/hubfx/esp32s3/src/effects/<effect>/` (e.g. `gunfx/gunfx_service.h`, `landing_lights/`, `gearcontrol/`). Roles live in `controllers/lib/sfx_board/roles/`.

---

## Error Code Ranges

```yaml
Error_Ranges:
  - range: "0x00"
    name: "OK"

  - range: "0x01-0x0F"
    namespace: "SerialError (General)"
    errors:
      - { code: 0x01, name: "UNKNOWN" }
      - { code: 0x02, name: "NOT_INITIALIZED" }
      - { code: 0x03, name: "INVALID_COMMAND" }
      - { code: 0x04, name: "MISSING_PARAMETER" }
      - { code: 0x05, name: "BUSY" }
      - { code: 0x06, name: "NOT_SUPPORTED" }
      - { code: 0x07, name: "PERMISSION_DENIED" }

  - range: "0x10-0x1F"
    namespace: "SerialError (Parameter)"
    errors:
      - { code: 0x10, name: "INVALID_PARAM" }
      - { code: 0x11, name: "PARAM_OUT_OF_RANGE" }
      - { code: 0x12, name: "INVALID_ID" }
      - { code: 0x13, name: "INVALID_VALUE" }
      - { code: 0x14, name: "PARAM_TOO_LONG" }

  - range: "0x20-0x4F"
    namespace: "Per-module error blocks (PortError 0x20, GunError 0x30, RoleError 0x40)"
    note: "Full comprehensive allocation lives in CLAUDE.md → Error ranges"

  - range: "0xF0-0xFF"
    namespace: "SerialError (System)"
    errors:
      - { code: 0xF0, name: "INTERNAL_ERROR" }
      - { code: 0xF1, name: "TIMEOUT" }
      - { code: 0xF2, name: "COMM_ERROR" }
      - { code: 0xF3, name: "BUFFER_OVERFLOW" }
      - { code: 0xF4, name: "CRC_ERROR" }
      - { code: 0xF5, name: "FRAMING_ERROR" }
```

> The full per-module error-code allocation (one namespace per range, collision-guarded by `tests/host/go_unit/error_collisions_test`) is in **`CLAUDE.md` → "Error ranges"**. Each module's errors live with that module (effect under `effects/<x>/`, role/infra under `lib/`), with a same-valued Go mirror.

---

## Key Implementation Rules

```yaml
DO:
  - Use little-endian for ALL multi-byte values
  - Use SfxWire::getU16LE() / putU16LE() for endian-safe reads/writes
  - Return CommandHandleResult::Ack() / Nack(err) from a policy handle()
  - Compose subsystems as *ServicePolicy (SystemServicePolicy concept) in BoardServer<...>
  - Declare expander boards via BoardOf<TBoard, TStream, PortCapacity<…>, …>
  - Use SFX_REQUIRE_LEN / SFX_VALIDATE in handle() for length + range checks
  - Include unit suffixes on all physical measurements (_mV, _mA, _us, _ms)
  - Mirror every packet/error in app/go/protocol/<mod>/<mod>.go (source of truth)
  - Implement board-unique resources as singletons (::instance(), private ctor, deleted copy/move)
  - Access singletons directly — never via pointer injection or extern globals

DONT:
  - Don't use blocking delays in effect/role code (read EffectClock, not millis(), Rule 40)
  - Don't allocate memory in interrupt context
  - Don't assume packet payload is null-terminated
  - Don't claim a packet byte two policies already own (first-owner-wins)
  - Don't reference deleted shapes: SfxServer / BusServer / BusClient / CommandRouter /
    ICommandHandler / CoreCommandServer / *Server / *Client / handleModulePacket / onModulePacket
```
