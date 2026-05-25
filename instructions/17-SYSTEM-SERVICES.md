# System Services as Composable Policies — Proposal

**Status (2026-05-16):** Wave 1 LANDED — framework headers + first
policy conversion.  Supersedes the earlier `17-STORAGE-VARIADIC-PROPOSAL.md`
which was scoped to storage only.

**Goal:** unify the master (HubFX) and the post-pivot expander boards
behind one templated `BoardServer<...Policies>` that carries every
protocol-exposed system service as a compile-time policy.  Add a
deterministic, persistent **board GUID** and **per-port GUIDs** so
multiple boards of the same type are unambiguous and every port on
every board is stably addressable for config.

## Migration status

| Wave | What landed | Status |
|---|---|---|
| 1   | `SystemServicePolicy` concept + `ServiceContext` + `BoardServer<...Policies>` template in [serial/core/system_service.h](../controllers/lib/sfx_serial/serial/core/system_service.h).  `CoreServer` (was `SlaveServer`) refactored into `ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>` in [sfx_core/core/component_service.h](../controllers/lib/sfx_core/core/component_service.h) — no `BusServer` inheritance, uses `ServiceContext*` for wire helpers. | **landed** |
| 2   | Five legacy servers converted to policies: `BatteryServicePolicy<TBattery>`, `AudioServicePolicy<TMixer>`, `ConfigServicePolicy`, `UsbHostServicePolicy`, `EngineServicePolicy`.  HubFX firmware instantiates one `BoardServer<UsbHostServicePolicy, AudioServicePolicy<Mixer>, EngineServicePolicy, ConfigServicePolicy, BatteryServicePolicy<Ina226Battery>>` — replaces five individual `addModuleHandler()` calls.  Externally-bound deps (battery sensor, config stores) accessed via `board.policy<P>().bindXxx(...)` / `.addStore(...)` after default construction.  Each policy advertises its capability bit via `kCapabilityBits`; OR'd at compile time by `BoardServer::capabilities()` and added in one shot.  Wire-helper wrappers (`sendAck`/`sendNack`/`sendRawPacket`) inline on each policy class so existing handler code + `SFX_REQUIRE_LEN`/`SFX_VALIDATE`/`SFX_DISPATCH` macros work unchanged. | **landed** |
| 2b  | `StorageServerT<TPolicy>` → `StorageServicePolicy<TPolicy>` (still single-policy axis; variadic-backend reshape per §5 deferred to a later RFC).  Sixth policy joins HubFX's `BoardServer` pack.  `StreamWriter` constructor relaxed: now takes `sfx_core::ServiceContext&` instead of `BusServer&` — storage was the only consumer; both interfaces have identical `sendRawPacket()` surface so the swap is mechanical.  HubFX's `initProtocolHandlers()` collapsed from 12 lines of `.begin()` + `addModuleHandler()` calls to 3 lines (board.begin + addModuleHandler + addCapability). | **landed** |
| 4   | `CoreCommandServer` (lifecycle) → `BoardServicePolicy`.  Templatise `SfxServer` to own `BoardServer<BoardServicePolicy, ...UserPolicies>` directly — lifecycle becomes a regular policy in the pack.  Removes the last legacy `BusServer` subclass besides `BoardServer` itself.  Touches every board's setup() since `SfxServer` becomes templated.                                                                                                  | pending    |
| 5   | Delete `BusServer` + `ICommandHandler` + `CommandRouter`.  `BoardServer` is the only dispatcher — no runtime polymorphism, no router walk.  Static dispatch via the policy tuple is the entire mechanism.                                                                                                                                                                                                                            | pending    |

**Why wave 4 is its own session:** templatising `SfxServer` is a single mechanical edit, but it propagates to every controller's `setup()` (HubFX + GunFX + LightFX + GearControl + NoOp).  The lifecycle policy itself absorbs ~365 lines of `CoreCommandServer.cpp` (INIT / IDENTIFY / STATUS / KEEPALIVE / REBOOT / I2C_SCAN / LOG_MESSAGE / DIAG_HISTORY / BATTERY_CONFIG) plus the indicator-LED state machine and keepalive watchdog that currently live in SfxServer.  Splitting waves 4 + 5 keeps each cleanly verifiable and rollbackable.

**Why wave 5 needs to wait for wave 4:** until `CoreCommandServer` is a policy, something has to be registered with `CommandRouter`.  Once nothing inherits `BusServer` except `BoardServer` itself, the dispatch resolution is fully static — `CommandRouter` and `ICommandHandler` have no purpose and delete.

**On `ICommandHandler` specifically:** today it exists so `CommandRouter` can walk a `std::array<ICommandHandler*, N>` and try each handler in turn.  With everything in one `BoardServer`, dispatch is `std::apply` over the policy tuple — compile-time resolution, no virtual lookup.  Both `ICommandHandler` and `CommandRouter` delete with `BusServer` in wave 5.

The waves are independently mergeable — every wave just converts one
subsystem and registers it as a policy alongside the legacy
`addModuleHandler` handlers for the rest.  Wave 4 is the only one with
real risk because it touches `SfxServer` and every board's setup().

This proposal is downstream of
[15-GENERIC-EXPANDER-REFACTOR.md](15-GENERIC-EXPANDER-REFACTOR.md) —
that doc removes the per-board (GunFX/LightFX/GearControl)
command-servers and replaces them with a generic `ExpanderServer`.
This doc explains what *both* master and expander then have in common,
and how their shared services compose.

## 1. Today's architecture

### 1.1 Board core — `CoreCommandServer`

Defined in [serial/core/bus_server.h](../controllers/lib/sfx_serial/serial/core/bus_server.h)
and [serial/core/core.h](../controllers/lib/sfx_serial/serial/core/core.h).

- Owns the core packet range `0xEF..0xFF`: INIT (0xF0), KEEPALIVE
  (0xF2), INIT_READY (0xF3), STATUS (0xF4), ACK/NACK (0xF6/0xF7),
  REBOOT (0xF8), BOOTSEL (0xF9), STATUS_REQ (0xFA), I2C_SCAN (0xFB),
  I2C_SCAN_RESULT (0xFC), LOG_MESSAGE (0xFD), IDENTIFY (0xFE),
  STATUS_UPDATE (0xEF), BATTERY_CONFIG (0xEE).
- `CoreBoardInfo` holds the identify payload — name, version,
  platform, CPU MHz, free RAM, build number, **`capabilities:u32`**.
- `setBoardInfo()` is called once at boot from `SfxServer::begin()`;
  `addCapability()` is called incrementally as each subsystem reports
  a successful `begin()`.
- INIT_READY and IDENTIFY share the same payload encoding via
  `CorePayload::encodeInitReady()`.  IDENTIFY is the "info only, don't
  activate hardware" variant.

### 1.2 Capability bits ([core.h:400-409](../controllers/lib/sfx_serial/serial/core/core.h#L400-L409))

| Bit | Symbol | Meaning |
|---|---|---|
| 0 | FLASH | LittleFS storage commands available |
| 1 | SD | SD card storage commands available |
| 2 | AUDIO | AudioMixer + audio playback |
| 3 | USB_HOST | USB host stack |
| 4 | ENGINE | Sound engine commands |
| 5 | CONFIG | YAML config-store commands |
| 6 | SLAVE_BUS | Master can enumerate expander boards |
| 7 | BATTERY | Battery sensor present |

The Go side mirrors this exactly in
[app/go/protocol/core/core.go](../app/go/protocol/core/core.go).

### 1.3 Storage today — single template axis

`StorageServerT<TPolicy>` ([sfx_storage/server/storage_server.h](../controllers/lib/sfx_storage/server/storage_server.h))
takes a `StoragePolicy` describing the *upload* buffer allocation
(PSRAM-ring vs heap), with backend support gated by a single static
boolean `SdSupported`.  Backend selection is runtime via the
`StorageTarget` payload byte; the server *always* registers all 14
storage commands regardless of what the board actually has.

### 1.4 Pain points

A. **Capability bits are advertised at runtime.**  Calling
   `addCapability(FLASH)` on a board with no flash compiles — the wire
   format does not catch the mismatch.

B. **Backend support is gated by platform, not by board.**  Every
   Pico is `SdSupported = false`.  A Pico variant with SD storage
   would need a forked policy.

C. **Identify storage info is only two bits.**  Clients learn "has
   flash" / "has SD" but nothing about sizes, free space, max upload
   bytes, or path conventions — those need follow-up packets.

D. **No-storage boards still link `StorageServer`.**  Compiles fine
   but pulls in 14 handlers, an upload state machine, and a buffer
   allocator that never runs.

E. **No stable per-board identity.**  USB enumeration order is not
   stable; two LightFX boards in two USB ports are indistinguishable
   to the master from one boot to the next.

F. **No stable per-port identity.**  Configs that say "front landing
   light = LED channel 3" break if a wiring rev moves the LED to
   channel 5.

G. **No symmetry between master and expander.**  HubFX's storage,
   audio, battery, config all live as separate `XxxServer` instances
   each registering against `commandRouter`.  Expanders (post-pivot)
   will have their own copies of storage / battery / config + the
   `ExpanderServer`.  There is no compile-time guarantee the two sides
   agree on *which* services exist on a given build.

## 2. Proposed shape — one composable Core, three new identifiers

Three coordinated additions:

1. **`CoreCommandServer<...SystemServicePolicies>`** — the existing
   core packet handler becomes a variadic template parameterised on
   the system services the board exposes.  HubFX and every expander
   declare their service mix as one type alias.

2. **Board GUID** — every board has a deterministic, persistent
   128-bit UUID derived from its silicon serial (UUIDv5 with a fixed
   ScaleFX namespace).  Returned in IDENTIFY.  Persistable to flash
   as `/.system/board.guid` to allow override (RMA replacement, manual
   reassignment).

3. **Port GUIDs** — every protocol-addressable component on a board
   (LED channel, servo, PWM channel, audio output, USB host port)
   carries its own deterministic UUID derived from
   `uuid5(board_guid, "port:<class>:<index>")`.  Returned via the
   existing `COMPONENT_LIST_REQ` flow that expanders use; HubFX gains
   its own response so the master itself becomes addressable port-by-port.

Effects are explicitly **out of scope** — they are internal HubFX
functionality (plain C++ classes consuming `ExpanderApi`), not
policies of `CoreCommandServer`.

### 2.1 The unified server

```cpp
// controllers/lib/sfx_serial/serial/core/bus_server.h  (new shape)

template <typename... ServicePolicies>
class CoreCommandServer : public BusServer {
    static_assert((SystemServicePolicy<ServicePolicies> && ...));

    static constexpr uint32_t kCapabilityMask
        = (ServicePolicies::kCapabilityBits | ...);   // compile-time

    static constexpr uint8_t kServiceCount = sizeof...(ServicePolicies);

public:
    /// Capabilities word baked into IDENTIFY — derived from the policy
    /// pack, not from runtime `addCapability()` calls.
    static constexpr uint32_t capabilities() { return kCapabilityMask; }

    /// Walk the pack in `setup()` to call `begin(server)` on each
    /// policy in declaration order.
    bool beginPolicies(SfxServer& server);

    /// Fill IDENTIFY payload's trailing TLV section with one
    /// descriptor per service that has anything to declare
    /// (storage backends, audio sample-rate, USB host port count …).
    uint16_t encodeServiceDescriptors(uint8_t* out, uint16_t cap) const;

protected:
    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;

private:
    // Service instances live inline — no virtuals, no heap.
    std::tuple<ServicePolicies...> _policies;
};
```

The `SystemServicePolicy` concept (defined in the same header):

```cpp
template <typename P>
concept SystemServicePolicy = requires(P p, SfxServer& s,
                                        uint8_t type, const uint8_t* pl,
                                        size_t len) {
    { P::kCapabilityBits }            -> std::convertible_to<uint32_t>;
    { P::kServiceTag }                -> std::convertible_to<uint8_t>;
    { p.begin(s) }                    -> std::convertible_to<bool>;
    { p.ownsType(type) }              -> std::convertible_to<bool>;
    { p.handle(type, pl, len) }       -> std::convertible_to<CommandHandleResult>;
    { p.encodeDescriptor(/*out*/(uint8_t*)nullptr,
                         /*cap*/(uint16_t)0) }
                                       -> std::convertible_to<uint16_t>;
};
```

Each policy:

- Declares which capability bits it contributes (`kCapabilityBits`).
- Declares which **packet types** it owns (`ownsType(type)`).
- Handles those types directly (`handle(...)` — no separate
  `BusServer` registration; the `CoreCommandServer` dispatches by
  walking the tuple).
- Optionally writes a TLV descriptor to IDENTIFY
  (`encodeDescriptor()` returns bytes written, 0 if nothing to declare).

### 2.2 What composes into the policy pack

| Service | Policy | Packet range | Notes |
|---|---|---|---|
| Storage | `StorageServicePolicy<UploadPolicy, Backends...>` | `0x93–0xAA` | Variadic over `Backends...`; see §4 |
| Audio | `AudioServicePolicy<TMixer>` | `0xA0–0xA3` | HubFX only |
| Battery | `BatteryServicePolicy<TBatteryDriver>` | `0xEE` (BATTERY_CONFIG) + STATUS append | One driver per board; today's `BatteryServerT` shape |
| Config | `ConfigServicePolicy<TStores...>` | `0xC0–0xC2` (CONFIG_RELOAD / _SAVE / _STATUS) | Variadic over the per-feature stores |
| USB host | `UsbHostServicePolicy<TRegistry>` | `0xB0–0xB2` | HubFX only |
| Expander bus | `ExpanderBusRouterPolicy` | passthrough | HubFX only — routes 0x01-0x7F to expander boards by `(ExpanderType, BoardGuid)` |
| Expander wire | `ExpanderServicePolicy<TServos, TPwms, TLeds>` | `0x10–0x3F` | Expander boards only |
| Diagnostics | (already part of `CoreCommandServer`) | core range | Not a policy — always present |
| **Effects** | **OUT OF SCOPE** | — | Internal HubFX functionality, see §3.3 |

The reason effects are out of scope: their semantics are
master-internal — they consume `ExpanderApi` to drive expander ports,
they emit typed events to Studio / CLI via the observer chain, and
they never appear on the wire as a protocol surface.  Putting them in
the policy list would imply they have a packet range and an
IDENTIFY-visible capability bit; they have neither.

### 2.3 Concrete board declarations

```cpp
// controllers/hubfx/esp32s3/src/hubfx_core.h
using HubFxCore = CoreCommandServer<
    StorageServicePolicy<Esp32UploadPolicy,
                         FlashStorageBackend,
                         SdLfsStorageBackend>,
    AudioServicePolicy<HubFxAudioMixer>,
    BatteryServicePolicy<Ina226Battery>,
    ConfigServicePolicy<HubSettingsStore,
                        LightFxConfigStore,
                        GearControlConfigStore,
                        GunFxConfigStore>,
    UsbHostServicePolicy<HubFxUsbRegistry>,
    ExpanderBusRouterPolicy
>;

// controllers/lightfx/pico/src/lightfx_core.h  (post-expander-refactor)
using LightFxCore = CoreCommandServer<
    StorageServicePolicy<PicoUploadPolicy, FlashStorageBackend>,
    ConfigServicePolicy<BoardIdentityStore>,             // /board.yaml only
    BatteryServicePolicy<Ina226Battery>,                  // optional
    ExpanderServicePolicy<ServoCollection<2>,
                          PwmCollection<0>,
                          LedCollection<8, NativeGpio>>
>;

// controllers/gunfx/pico/src/gunfx_core.h
using GunFxCore = CoreCommandServer<
    StorageServicePolicy<PicoUploadPolicy, FlashStorageBackend>,
    ConfigServicePolicy<BoardIdentityStore>,
    BatteryServicePolicy<Ina226Battery>,
    ExpanderServicePolicy<ServoCollection<1>,
                          PwmCollection<3>,
                          LedCollection<0, NativeGpio>>
>;
```

The same template instantiation handles both master and expander —
they differ only in which policies they list.

## 3. Board GUID

### 3.1 Derivation — deterministic UUIDv5

```
SCALEFX_NAMESPACE_UUID = f7e8a4b0-3c1d-4e5f-9a2b-1c8d9e0f1a2b   // generated once, hardcoded
silicon_id_bytes       = ESP32-S3:  6 bytes from efuse MAC
                         RP2040:    8 bytes from pico_get_unique_board_id()

board_guid             = uuid5(SCALEFX_NAMESPACE_UUID, silicon_id_bytes)
```

Properties:

- Same chip → same GUID, forever.
- Two boards never collide (silicon IDs are factory-unique).
- No randomness — the derivation is reproducible offline given the
  silicon ID + namespace UUID.
- Reflashing the firmware does not change the GUID (silicon ID is in
  efuse / W25Q OTP region, not in app flash).

The namespace UUID is generated once via `uuidgen`, pasted into a
single source-of-truth header, and **never changes**.  Changing it
would re-key every fielded board.

### 3.2 Boot logic

```cpp
// controllers/lib/sfx_core/identity/board_guid.h
namespace sfx_identity {

/// 128-bit board GUID, raw bytes (network byte order in the IDENTIFY
/// payload — but stored on disk as raw 16 bytes too).
struct BoardGuid { uint8_t b[16]; };

/// Compute the deterministic UUIDv5 from the running chip's silicon id.
void deriveGuid(BoardGuid& out);

/// 1) read /.system/board.guid if present → use it (override path)
/// 2) else compute via deriveGuid(), write the file, use it
/// 3) signal which path was taken via `wasDerived` (true = computed,
///    false = loaded from override file) for logging purposes only.
bool loadOrDeriveGuid(BoardGuid& out, bool* wasDerived = nullptr);

/// Explicit override write (provisioning tool path).  Refuses to
/// overwrite an existing file unless `force == true`.
bool writeGuid(const BoardGuid& guid, bool force = false);

/// Canonical "xxxxxxxx-xxxx-4xxx-Yxxx-xxxxxxxxxxxx" string for logs
/// and CLI output.  Writes 36 chars + NUL.
void formatGuid(const BoardGuid& guid, char out[37]);

}  // namespace sfx_identity
```

SHA-1 is required for UUIDv5; it's available on both platforms via
ESP-IDF and Pico SDK / MbedTLS (≈ 2 KB code).

### 3.3 Persistence — single path on both platforms

The GUID file lives at `/.system/board.guid` (16 raw bytes) inside the
existing LittleFS partition.  Reflashing the firmware preserves
LittleFS by default — neither `esptool write_flash app.bin` nor
`picotool load app.uf2` touches the FS unless explicitly told to.

**Override scenario:** to assign a specific GUID (e.g. RMA replacement
that should inherit a failed unit's identity), the flash tool sends
the override file via the existing storage upload protocol.

**Reset scenario:** `scalefx-flash flash <ctrl> --reset-identity`
explicitly deletes `/.system/board.guid` post-flash.  On next boot,
the deterministic derivation runs and writes a fresh file with the
chip's natural identity.

### 3.4 Provisioning UX — automatic by default

`scalefx-flash flash <controller>` performs an implicit provisioning
step after a successful flash:

1. Wait for board to enumerate.
2. Send IDENTIFY.
3. If returned GUID is all-zeros (legacy firmware that doesn't have
   the field) → no action.
4. If returned GUID matches the deterministic derivation → no action
   (the file was either auto-generated on first boot or already exists
   with the natural value).
5. If returned GUID differs from the deterministic derivation → log
   that the board has an override file (operator info only).

Because the derivation is deterministic, the auto-provisioning step
is *invisible* in the common case — no warnings, no prompts.  The
deterministic-fallback path produces the canonical GUID even without
explicit provisioning.

### 3.5 IDENTIFY payload — appending the GUID

Today:

```
INIT_READY / IDENTIFY payload:
  [nameLen:u8][name]
  [verLen:u8][ver]
  [platLen:u8][plat]
  [cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE][capabilities:u32LE]
```

Append (Rule 11 — older firmware decodes the new tail as zero):

```
  [boardGuid:u8[16]]                          // 16 bytes raw, network-byte-order canonical
  [serviceDescriptorsLen:u16LE]
  [ServiceDescriptor × N]:
    [serviceTag:u8]        // matches Policy::kServiceTag
    [payloadLen:u8]
    [payload:u8[payloadLen]]
```

The TLV section is the extensibility point for §4 (storage backends),
§6 (audio sample-rate, USB host port count, etc.).  Each policy's
`encodeDescriptor()` writes its own TLV; the variadic
`encodeServiceDescriptors()` concatenates them.

For a HubFX with 5 policies declaring descriptors averaging ~10 bytes
each → ~70 bytes of TLV.  Plus the 16-byte GUID.  Well inside the 512-byte
payload ceiling.

Master-side Go decoder:

```go
type IdentifyResponse struct {
    Name         string
    Version      string
    Platform     string
    CpuMhz       uint32
    FreeRam      uint32
    BuildNum     uint32
    Capabilities uint32
    BoardGuid    [16]byte
    Services     []ServiceDescriptor   // parsed from TLV
}
```

Legacy firmware → `BoardGuid` is all-zeros; `Services` is empty.
HubFX treats all-zeros as "unprovisioned legacy" and matches by type
only (logs a warning), preserving the existing matching behaviour.

## 4. Port GUIDs — components addressable by stable identity

### 4.1 Why

Configurations should target *roles* rather than *port indices*.  If
the front landing light is on LED channel 3 today and a board rev
moves it to channel 5, the config should be updatable in one place
without touching anything that doesn't physically move.  The fix:
give every port its own UUID, derived from the board GUID, and let
configs key on GUIDs.

The same mechanism handles "two LightFX boards plugged in" — each
port is globally unique because each board GUID is globally unique.

### 4.2 Derivation

```
port_guid = uuid5(board_guid, "port:" + kind + ":" + index)
```

Where `kind` is one of `led_dedicated`, `led_pwm`, `servo`, `pwm`,
`audio_out`, `usb_host`, `sd_card`, …  and `index` is the 0-based
position within that kind on this particular board.

Properties:

- Deterministic.  Same board, same firmware → same port GUIDs.
- Stable across reflashes.  No persistence needed.
- Globally unique.  Two LightFX boards' LED-3 ports have different
  GUIDs because their board GUIDs differ.
- Re-derivable from board GUID + descriptor without contacting the
  board.  Useful for offline config validation.

### 4.3 Discovery — extended COMPONENT_LIST

The expander refactor's `COMPONENT_LIST_REQ` (0x10) already enumerates
the board's components.  Today its response is a packed
`ComponentInfo[]` carrying counts + flags.  Extension (Rule 11
append-only):

```
COMPONENT_LIST_RESP payload:
  [boardGuid:u8[16]]
  [componentCount:u16LE]
  [ComponentInfo × N]:
    [portGuid:u8[16]]
    [kind:u8]
    [index:u8]
    [flags:u32LE]
    [nameLen:u8]
    [name:u8[nameLen]]    // optional human-readable label
    [paramsLen:u8]
    [params:u8[paramsLen]]   // kind-specific (max-Hz for PWM, range for servo, ...)
```

Boards may emit the port GUIDs directly or have the master derive
them from `(boardGuid, kind, index)`.  Emitting them keeps the master
agnostic of the derivation rule and lets the board override a port's
GUID if necessary (rare, but reserved for the same RMA-replacement
scenario as board GUIDs).

### 4.4 HubFX exposes its own ports

HubFX itself owns physical I/O — 8 LED outputs (PCA9685 channels),
I2S audio out, SD card, USB host ports, future servo headers.  Under
this proposal HubFX answers `COMPONENT_LIST_REQ` for its own internal
components, exactly the same way an expander does.  HubFX's
`ExpanderBusRouterPolicy` (the policy that forwards 0x01-0x7F packets
to expanders) treats `addr=0` (or a reserved "local" address byte) as
"this is for HubFX itself" and dispatches to HubFX's own
`ExpanderServicePolicy`-equivalent.

Result: from Studio's perspective, HubFX is just another expander
that happens to also expose audio + storage + USB-host services.  A
config like:

```yaml
roles:
  front_landing_light:
    port_guid: 7c4a8d09-ca3a-44d2-9c6f-1a8b1e5f2d10  # HubFX LED ch 3
  rear_gun_servo:
    port_guid: 9e2b1c44-7f8a-43e1-b9d3-2c4e5a8f9012  # External expander servo 0
```

…has no asymmetry between local and remote ports.  The dispatcher
resolves both GUIDs to `(boardGuid, kind, index)` and emits the
right packet, hub-local or expander-routed.

### 4.5 HubFX board YAML — role table

`hubfx.yaml` gains a `roles:` block and a `boards:` block:

```yaml
boards:
  - guid: 7c4a8d09-ca3a-44d2-9c6f-1a8b1e5f2d10    # the HubFX itself
    role: hub
  - guid: 9e2b1c44-7f8a-43e1-b9d3-2c4e5a8f9012    # external LightFX
    role: front_landing
  - guid: 4a1f6c33-1b8e-4c5e-a801-2d3e4f5a6b7c    # external LightFX
    role: rear_landing

roles:
  front_landing_light_1:
    board: front_landing
    port:  led:0
  rear_landing_light_1:
    board: rear_landing
    port:  led:0
  master_volume_audio:
    board: hub
    port:  audio_out:0
```

The dispatcher resolves `board: front_landing` → board GUID (looked
up in `boards:`), then `port: led:0` → port GUID (derived from the
board GUID), then sends `LED_SET_BRIGHTNESS addr=0` over the wire to
that expander.  Expanders that connect without a `boards:` entry
surface as warnings in `hub:expanders` (CLI) / Studio's expander tab.

## 5. Storage backends — first concrete service policy

The bulk of the original variadic-storage proposal carries over
unchanged, just framed as one example of a `SystemServicePolicy`.

### 5.1 Per-backend traits

```cpp
// controllers/lib/sfx_storage/server/backends/flash_backend.h
struct FlashStorageBackend {
    static constexpr uint8_t  kStorageTarget = HubFxStorage::TARGET_FLASH;
    static constexpr uint32_t kCapabilityBit = CoreCapability::FLASH;
    static constexpr uint32_t kMaxUploadBytes = 2u * 1024 * 1024;
    static constexpr uint8_t  kDescriptorTag = 0x01;
    static constexpr const char* kName       = "flash";

    using FileHandle = StorageFile;
    using Module     = FlashModule;

    static Module& module()           { return Module::instance(); }
    static bool    isMounted()        { return module().isInitialized(); }
    static uint32_t totalBytes();
    static uint32_t freeBytes();
    static uint8_t openRead (const char* p, FileHandle& f);
    static uint8_t openWrite(const char* p, FileHandle& f, bool truncate);
    // ... list / tree / delete / mkdir / info / chunk helpers ...
};

// controllers/lib/sfx_storage/server/backends/sd_lfs_backend.h
struct SdLfsStorageBackend { /* mirror, but kStorageTarget = TARGET_SD */ };
```

Concept:

```cpp
template <typename B>
concept StorageBackend = requires {
    { B::kStorageTarget } -> std::convertible_to<uint8_t>;
    { B::kCapabilityBit } -> std::convertible_to<uint32_t>;
    { B::kMaxUploadBytes } -> std::convertible_to<uint32_t>;
    { B::kDescriptorTag } -> std::convertible_to<uint8_t>;
    typename B::FileHandle;
    typename B::Module;
};
```

### 5.2 `StorageServicePolicy<UploadPolicy, Backends...>`

```cpp
template <typename UploadPolicy, typename... Backends>
class StorageServicePolicy {
    static_assert((StorageBackend<Backends> && ...));

public:
    static constexpr uint32_t kCapabilityBits = (Backends::kCapabilityBit | ...);
    static constexpr uint8_t  kServiceTag     = 0x01;

    bool   begin(SfxServer& s);
    bool   ownsType(uint8_t type) const;     // 0x93..0xAA
    CommandHandleResult handle(uint8_t type, const uint8_t* pl, size_t len);

    /// One StorageDescriptor per backend, regardless of mount state.
    uint16_t encodeDescriptor(uint8_t* out, uint16_t cap) const;

private:
    UploadPolicy _upload;
    // Dispatch by TARGET byte → match against `Backends...` pack
    template <typename Op> auto dispatchByTarget(uint8_t target, Op&& op);
};
```

The dispatch walks the pack with `if constexpr` so backends not in
the list never compile into the binary.

### 5.3 StorageDescriptor TLV

```cpp
struct StorageDescriptor {     // 14 bytes
    uint8_t  backendTag;
    uint8_t  flags;            // bit0: mounted, bit1: writable, bit2: removable
    uint32_t totalBytes;
    uint32_t freeBytes;
    uint32_t maxUploadBytes;
};

namespace StorageDescriptorFlags {
    constexpr uint8_t MOUNTED   = 0x01;
    constexpr uint8_t WRITABLE  = 0x02;
    constexpr uint8_t REMOVABLE = 0x04;
}
```

`StorageServicePolicy::encodeDescriptor()` writes:

```
[serviceTag:0x01][payloadLen:u8][backendCount:u8][StorageDescriptor × N]
```

HubFX with 2 backends → 1 + 1 + 1 + 28 = 31 bytes for the storage TLV.

### 5.4 Studio consumption

Studio's `FsStorageStatus` reads the descriptor block instead of
inferring from capability bits.  It learns "this board has flash
(3.9 MB total, 2.1 MB free), SD (32 GB total, 12 GB free)" from the
single IDENTIFY response.  Drives the dropdown of available targets
in the file picker.

The capability bits stay — they're useful as a quick "can I speak
audio to this board" check that doesn't need to parse variable-length
TLVs.  But for storage, the bits become *derived* (from `Backends::kCapabilityBit | ...`)
rather than runtime-asserted.

## 6. Other system services — same shape

Each policy follows the same template — declare its capability bits,
own a packet range, encode a TLV descriptor if it has fixed-shape
info worth exposing in IDENTIFY.

### 6.1 `AudioServicePolicy<TMixer>`

- `kCapabilityBits = CoreCapability::AUDIO`
- Owns 0xA0–0xA3 (PLAY, STOP, VOLUME, QUERY).
- Descriptor: sample rate (u32LE), channel count (u8), max
  simultaneous voices (u8) — 6 bytes.

### 6.2 `BatteryServicePolicy<TBatteryDriver>`

- `kCapabilityBits = CoreCapability::BATTERY`
- Owns BATTERY_CONFIG (0xEE) + appends a battery block to STATUS.
- Descriptor: chemistry (u8), nominal cell count (u8), cutoff voltage
  default (u16LE) — 4 bytes.

### 6.3 `ConfigServicePolicy<TStores...>`

- `kCapabilityBits = CoreCapability::CONFIG`
- Owns CONFIG_RELOAD / _SAVE / _STATUS (0xC0..0xC2).
- Descriptor: store count (u8) + per-store `[nameLen:u8][name][version:u16LE]`
  — one entry per `TStore` in the pack.

### 6.4 `UsbHostServicePolicy<TRegistry>`

- `kCapabilityBits = CoreCapability::USB_HOST | EXPANDER_BUS`
- Owns 0xB0–0xB2.
- Descriptor: port count (u8), per-port type (u8) — variable.

### 6.5 `ExpanderBusRouterPolicy`

- `kCapabilityBits = CoreCapability::EXPANDER_BUS`
- Doesn't *own* packet types — instead, claims 0x01-0x7F via a
  passthrough hook and forwards by `(ExpanderType, BoardGuid)` to the
  matching `UsbHostServicePolicy`'s registry.
- Descriptor: empty (info lives in the USB host descriptor).

### 6.6 `ExpanderServicePolicy<TServos, TPwms, TLeds>`

- `kCapabilityBits = 0`  (expander wire surface isn't a "capability"
  per se — the master discovers expander capabilities via
  `COMPONENT_LIST_REQ`).
- Owns 0x10–0x3F (the generic expander range from
  [15-GENERIC-EXPANDER-REFACTOR.md](15-GENERIC-EXPANDER-REFACTOR.md)).
- Descriptor: forwards the `COMPONENT_LIST_RESP` shape (port GUIDs +
  ComponentInfo) so IDENTIFY can carry it inline for tiny expanders.
  Larger expanders still respond to a dedicated `COMPONENT_LIST_REQ`.

## 7. Migration path

Phased so each step is independently verifiable.  GUID + port-GUID
groundwork lands first (cheap, additive), then the storage refactor,
then the broader policy-composition refactor.

| Phase | Change | Risk |
|---|---|---|
| 1 | Add `sfx_identity` module: `BoardGuid`, `deriveGuid`, `loadOrDeriveGuid`, `formatGuid`.  Hardcode `SCALEFX_NAMESPACE_UUID`.  Add SHA-1 dep if not already linked. | None — pure addition |
| 2 | Extend IDENTIFY payload to append the 16-byte GUID.  Update Go decoder (defaults to zeros for legacy).  HubFX `UsbRegistry` keyed by `(ExpanderType, BoardGuid)`. | Wire-format extension; Rule 11 compatible |
| 3 | Implement `/.system/board.guid` load/derive/write on each board's `setup()`.  Add `scalefx-flash flash --reset-identity` flag. | Local file-system path; minor risk |
| 4 | Add `StorageBackend` concept + `FlashStorageBackend` / `SdLfsStorageBackend` headers.  Leave `StorageServerT<TPolicy>` untouched. | None — pure addition |
| 5 | Add `StorageServicePolicy<UploadPolicy, Backends...>` alongside the existing class.  HubFX migrated to it; legacy `StorageServerT` deleted after Pico boards follow. | Bench upload/download tests per board |
| 6 | Extend `COMPONENT_LIST_RESP` (and the in-flight expander protocol header) with per-port GUIDs.  HubFX gains its own COMPONENT_LIST_REQ response covering hub-local ports. | Wire-format extension; Rule 11 compatible |
| 7 | Add `boards:` + `roles:` blocks to `hubfx.yaml`; dispatcher resolves role → board → port → wire address. | Studio + CLI integration |
| 8 | Introduce `CoreCommandServer<...ServicePolicies>` template.  Migrate HubFX core declaration to the new shape.  All non-policy callsites (`addCapability`, manual `addModuleHandler`) replaced by the policy pack. | Largest refactor; staged per-policy |
| 9 | Migrate expander cores (post-expander-refactor) to the same `CoreCommandServer<...>` shape. | Per-expander, one at a time |
| 10 | Studio surfaces per-port labels in tabs (using the human-readable `name` field from COMPONENT_LIST_RESP). | UI work only |

Phases 1-3 are GUID groundwork and unblock multi-board scenarios
immediately.  Phases 4-5 fix pain points (B), (D), (E) for storage.
Phase 6 fixes (F).  Phases 8-9 fix (A), (G) by unifying the server
model.

## 8. Out of scope

What this proposal explicitly does NOT address:

- **Effects.**  The HubFX's internal LightFX / GunFX / GearControl
  effect orchestrators are plain C++ classes consuming `ExpanderApi`.
  They are not policies of `CoreCommandServer`, do not have a packet
  range, and do not advertise capability bits.  Their typed event
  observers (Studio + CLI) are master-side observables, not protocol
  features.  Per
  [15-GENERIC-EXPANDER-REFACTOR.md](15-GENERIC-EXPANDER-REFACTOR.md).

- **The expander protocol surface itself.**  The wire packets, async
  events, safe-state behaviour, etc. are defined in doc 15.  This doc
  covers only how the `ExpanderServicePolicy` integrates into
  `CoreCommandServer`.

- **YAML schema for `hubfx.yaml`.**  §4.5 sketches the `boards:` /
  `roles:` blocks but a full schema lives in a separate doc once the
  GUID groundwork (phases 1-3) is in.

- **Renaming live code from `slave` → `expander`.**  That happens
  atomically with the expander refactor PRs per
  [15-GENERIC-EXPANDER-REFACTOR.md](15-GENERIC-EXPANDER-REFACTOR.md);
  this doc uses the post-rename names throughout.

- **Changing wire-level transport** (still COBS / CRC-8 over USB CDC).

- **OTA updates / network-attached boards.**  Future work.

## 9. Decisions made (vs the earlier RFC)

These were open questions in the previous `17-STORAGE-VARIADIC-PROPOSAL.md`;
recording the decisions here so they're not relitigated:

1. **GUID generation: deterministic UUIDv5, not random UUIDv4.**
   Same chip → same GUID forever; provisioning is invisible in the
   happy path.

2. **Effects are out of scope for `CoreCommandServer` policy composition.**
   Internal HubFX functionality, plain class instances driven from
   `hubfx_esp32s3.ino`.

3. **Per-board (GunFX/LightFX/GearControl) command servers go away
   entirely** per the expander refactor.  The post-refactor world has
   one `CoreCommandServer<...>` per board kind, parameterised by
   policies, not by per-board protocol headers.

4. **Slaves are renamed to expanders.**  Conceptual change only —
   live code follows during the refactor PRs.

5. **GUID storage is one path on both platforms:** `/.system/board.guid`
   in LittleFS.  Not NVS, not OTP, not silicon-only.  Provides both a
   deterministic derivation and an override mechanism.

6. **HubFX is just another COMPONENT_LIST_REQ responder.**  Its local
   ports get GUIDs and are addressable on the same footing as
   expander ports.  Unifies the role/board/port resolution model.

## 10. Cross-references

- [15-GENERIC-EXPANDER-REFACTOR.md](15-GENERIC-EXPANDER-REFACTOR.md)
  — the upstream refactor this builds on; defines the expander
  protocol, component-collection model, per-board migration plan.
- [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md)
  — single-board firmware contract; the place to look up what a
  conforming expander's `setup()` and `loop()` look like.
- [13-PASSTHROUGH-ROUTING.md](13-PASSTHROUGH-ROUTING.md)
  — how HubFX routes 0x01-0x7F to expanders; needs an update to key
  on `(ExpanderType, BoardGuid)` and to recognise `addr=0` as
  "local HubFX port" once §4.4 lands.
- [serial/core/core.h](../controllers/lib/sfx_serial/serial/core/core.h)
  — `CoreCapability::*`, `IDENTIFY` payload encoder/decoder.
- [storage_server.h](../controllers/lib/sfx_storage/server/storage_server.h)
  — current single-template-axis storage server, to be replaced by
  `StorageServicePolicy<UploadPolicy, Backends...>`.

If you're happy with this shape, phase 1 (the `sfx_identity` module
with `deriveGuid` + `loadOrDeriveGuid`) is pure addition and unblocks
everything that follows.
