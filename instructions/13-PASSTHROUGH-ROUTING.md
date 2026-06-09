# HubFX Pass-Through Routing — Auto-route by Packet Type Range

> **⚠ Status (2026-06-09): the type-range routing core below is SUPERSEDED.**
> The `SlaveServer` / `forwardToSlave` packet-type-range router and the typed
> per-board `*Client` forwarding are GONE. Routing is now **transparent
> expander roles (Rule 58)**: the hub addresses any expander's roles by an
> opaque `PortRef{guid, kind, idx}` over role-agnostic transport
> (`TOPOLOGY_ROLE_FORWARD` 0x8F command, `ROLE_QUERY`/`RESPONSE` 0xA6/0xA7,
> `ROLE_EVENT` 0x8E telemetry, `ROLE_BULK_ATTACH` 0x57 bringup) driven by the
> hub's `TopologyService`. Go drives it through `client.RoleTarget`
> (`client.Role(guid)`) — one role-I/O path, no typed forwarding clients, no
> `SLAVE_ENUM` discovery. See
> [31-GUID-PORT-ROUTING.md](31-GUID-PORT-ROUTING.md) and
> [32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md) §3–4.
>
> **What survives:** the **§4.4 mandatory board-prefix CLI disambiguation**
> (Rule 30) is still valid — though the CLI commands are now **flat
> hyphenated** (`gear-reset`, `gun-fire`, `light-servo`, `hub-slaves`), NOT
> the colon form (`gear:reset`) shown in the §4.4 examples below.
>
> The §1–3 + §5–6 sections are kept as a historical record of the retired
> type-range scheme.
>
> **Scope:** HubFX ESP32-S3 firmware, `serial/hubfx/hubfx.h`, Go SDK, Studio.
>
> **Design (retired).** The hub is a transparent type-range router. Any
> inbound packet whose type falls in a slave range is forwarded verbatim to
> the matching attached slave; the slave's response is forwarded back upstream
> verbatim with the original correlation tag. No envelope wrapping, no slot
> indices, no subcmd byte — clients send slave packets exactly as if they
> were talking to the slave directly.
>
> **What this is NOT.** Pass-through is for **external hosts** (Studio, CLI,
> future tooling) talking to slaves *through* the hub. It is NOT the path
> the hub uses to push its own boot YAML out to its attached slaves — that
> is hub-internal application logic and runs over the existing typed
> `LightFxClient` / `GunFxClient` / `GearControlClient` calls via a plain
> push function (`pushLightFxConfigToSlave(cfg, client)`, see
> [12-LIGHTFX-MODERNIZATION.md](12-LIGHTFX-MODERNIZATION.md) §4.2).
>
> Two surfaces, intentionally independent:
>
> | Surface | Source of action | Wire | Code path |
> |---------|------------------|------|-----------|
> | **External pass-through** (this doc) | Studio / CLI on the user's PC | Slave-range packets sent to the hub; verbatim relay both ways | `SlaveServer::forwardToSlave` → `BusClient::sendCommand` |
> | **Hub-internal config-apply** ([12](12-LIGHTFX-MODERNIZATION.md)) | HubFX firmware itself | None — typed `LightFxClient` over USB Host | `pushLightFxConfigToSlave(cfg, client)` → `client.ledMasterBrightness/...` |

---

## 1. Why this design

The previous iterations (`SLAVE_ROUTE_*` 0x96–0x98 subcmd; `ROUTE_DOWN/UP/UP_ASYNC`
0xB3–0xB5 slot envelope) were both retired. Three reasons the current scheme
is simpler:

1. **Slave packet ranges are already disjoint.** GunFX 0x01–0x2F, LightFX
   0x40–0x5F, GearControl 0x60–0x7F. The packet-type byte alone identifies
   the target board, so there is nothing to disambiguate.
2. **Async packets carry their own source.** Slave-range async packets (e.g.
   `LANDING_LIGHT_STATUS` 0x56, `GEAR_DOOR_STATUS` 0x72) are equally
   distinguished by type — no slot byte or wrapping needed.
3. **Clients stay protocol-uniform.** A client's `LightFxApi.LedSet(...)`
   builds the same wire bytes whether the connection is to a LightFX board
   directly or to a HubFX with a LightFX attached. The Go API layer has no
   `routeSlot` field and no envelope codec.

One slave per type — no multi-instance addressing. If two LightFX boards are
ever needed, that requires reintroducing slot routing; for the current
hardware (one of each type per hub) the type-range scheme is sufficient.

---

## 2. Wire format

### 2.1 Reserved hub packets (0xB1–0xB2 carved out of HubFX range 0x80–0xAF)

| Type | Name              | Dir | Payload |
|------|-------------------|-----|---------|
| 0xB1 | `SLAVE_ENUM_REQ`  | host → hub | `[]` |
| 0xB2 | `SLAVE_ENUM_RESP` | hub → host | `[count:u8]` then per slot: `[slot:u8][type:u8][connected:u8][ready:u8][nameLen:u8][name:str]` |

Clients use `SLAVE_ENUM_REQ` to learn which slave types are currently
attached. The Go engine refreshes this on connect (`Engine.RefreshSlaveAttachment()`)
and uses it to gate which command groups it accepts when on a HubFX
connection (see §4).

### 2.2 Auto-routed packets

Any packet whose type is in a slave range, sent to the hub, is forwarded
verbatim to the matching slave:

| Range       | Target board  |
|-------------|---------------|
| 0x01–0x2F   | GunFX         |
| 0x40–0x5F   | LightFX       |
| 0x60–0x7F   | GearControl   |

The slave's response is forwarded back upstream verbatim with the original
correlation tag:

- Typed RESP (e.g. `LIGHT_STATUS_RESP` 0x5B) → forwarded as-is, original tag.
- Plain ACK → forwarded as `CorePacket::ACK` with the original tag.
- NACK → forwarded as `CorePacket::NACK` with the original tag, error code
  and optional message preserved.

If no matching slave is attached, the hub responds NACK with
`HubFxError::SLAVE_NOT_CONNECTED` (0x81).

### 2.3 Async forwarding

Per-slot async pumps (`SlaveServer::wireAsyncPumps`) install a callback on
every attached slave. When a slave emits an unsolicited packet (`tag ==
TAG_ASYNC`), the hub forwards it upstream **verbatim** with `TAG_ASYNC` —
**provided** its type lies in a slave range. This keeps slave-internal
heartbeats (slave's own `CorePacket::STATUS` at 1 Hz) off the upstream wire,
since those would collide with the hub's own STATUS broadcast.

### 2.4 Tag handling

- Inbound: the host allocates an outer tag T. The hub forwards on a
  separately allocated internal tag T' (issued by the per-slot `BusClient`),
  matches the slave's response on T', and re-emits upstream with the
  original T. The host correlates by T as for any other request.
- Async: `TAG_ASYNC` (0xFF) is preserved on forwarding.

The hub maintains no host-visible tag-mapping table — internal mapping lives
entirely inside the per-slot `BusClient` (it allocates and matches its own
tags for `sendCommand()`).

---

## 3. Server side (HubFX ESP32-S3)

### 3.1 `SlaveServer::forwardToSlave(type, payload, len)`

```
1. target = slaveTypeForPacketType(type)        // 0x01..0x2F → GunFX, etc.
2. client = registry.getClient(target)
3. if !client → NACK SLAVE_NOT_CONNECTED
4. result   = client->sendCommand(type, payload, len)
5. if result.success:
       respType = client->lastResponseType()
       if respType != 0 → sendRawPacket(respType, currentTag(),
                                        client->lastResponsePayload(),
                                        client->lastResponseLen())
       else             → sendAck()
   else:
       sendNack(result.errorCode, result.errorMessage)
```

`BusClient::lastResponseType/Payload/Len` already buffer the most recent
typed RESP — same mechanism used elsewhere for `STATUS_REQ` forwarding.

### 3.2 Async pump (`SlaveServer::wireAsyncPumps`)

```cpp
client->onAsyncPacket([this](uint8_t type, const uint8_t* p, size_t n) {
    if (slaveTypeForPacketType(type) == SlaveType::Unknown) return;
    sendRawPacket(type, CoreProtocol::TAG_ASYNC, p, n);
});
```

Idempotent — re-calling rebinds. Called once after every slave is registered
in `setup()`.

### 3.3 Slave registry enumeration

`SlaveServer::handleSlaveEnum()` iterates `registry[]` and emits one entry
per slot. Empty slots emit `[slot, type=Unknown, connected=0, ready=0,
nameLen=0]` so the host knows the slot exists but is empty.

---

## 4. Client side

### 4.1 Go SDK

The `apiClient` has no routing field — it sends slave packets directly:

```go
type apiClient struct { conn *protocol.Connection }
```

The board APIs (`LightFxApi`, `GunFxApi`, `GearControlApi`) build their
packets exactly as for a direct connection. When the active connection is to
HubFX, those bytes go to the hub and the hub forwards to the matching slave.

### 4.2 Engine slave-attachment tracking

`Engine.RefreshSlaveAttachment()` calls `HubFxApi.SlaveEnum()` after the
INIT_READY handshake (only when `ControllerType == CtrlHubFX`). The result
populates `Engine.attachedSlaves map[string]bool` keyed by controller type
(e.g. `"lightfx"`). `Engine.CanRouteVia(slaveCtrl)` returns true when on
HubFX with that slave attached, and `Engine.Dispatch` widens the accepted
command groups accordingly.

### 4.3 Studio

When `Engine.ControllerType == CtrlHubFX`, the per-slave Svelte tabs become
active for any slave whose type appears in `attachedSlaves`. Tab code is
unchanged: it issues the same `LightFxApi.*` / `GearControlApi.*` calls as
on a direct connection, and the hub auto-routes them.

### 4.4 CLI / Studio Console — mandatory board prefix

Wire-format auto-routing makes the host-side text command surface ambiguous
on a hub: `servo 1 1500` could mean `gear:servo` OR `light:servo` OR
`gun:servo`, and the engine has no signal to pick one. The CLI dispatcher
therefore enforces a **mandatory prefix** on every board command (Rule 30):

| Group       | Prefix   | Example              |
|-------------|----------|----------------------|
| LightFX     | `light:` | `light:servo 1 1500` |
| GearControl | `gear:`  | `gear:reset all`     |
| GunFX       | `gun:`   | `gun:trigger on 600` |
| HubFX       | `hub:`   | `hub:slaves`         |
| Universal (Core / Firmware / Storage / Config) | *(none)* | `connect`, `init`, `file.list`, `config.save` |

Bare board names error with the prefixed candidates as a hint:

```
scalefx> servo 1 1500
✗ Command 'servo' requires a board prefix. Did you mean: gear:servo, gun:servo, light:servo
```

The same form is enforced on direct connections too — muscle memory
transfers verbatim when the same script later runs through a hub. Studio's
Console panel echoes the prefixed form for both typed input and any
GUI-mirrored echoes (currently all echoes are universal storage/config, so
no rewrite was needed).

Implementation: `CmdGroup.Prefix` field (`engine/types.go`); `FlatCommands`
keys entries as `<prefix>:<name>` and stamps the prefix into the stored
`Usage` so `help <cmd>` and error messages show the canonical form;
`Dispatch` and `CmdHelp` call `suggestPrefixed(name)` to surface the hint
when a user types a bare board command. Wire format is unchanged — the
prefix is a CLI surface convention only.

---

## 5. Use cases (external host only)

Reminder: this is for **PC ↔ hub ↔ slave**. Hub-internal config-apply uses
the typed `LightFxClient` directly via `pushLightFxConfigToSlave(cfg, client)`
— see [12-LIGHTFX-MODERNIZATION.md](12-LIGHTFX-MODERNIZATION.md) §4.2.

### 5.1 Studio sends a LightFX command through the hub

User clicks *Bind landing group 1 to channels 5+6 on servo 2* in the LightFX
tab. Studio (via the engine handler) calls `LightFxApi.LandingLightBind(...)`
exactly as on a direct connection. Wire trace:

```
host → hub  : LANDING_LIGHT_BIND tag=T  [slot=1, servoId=2, mask=0x30, br=100]
hub  → slv  : LANDING_LIGHT_BIND tag=T' [...]                    (auto-routed)
slv  → hub  : ACK tag=T'
hub  → host : ACK tag=T                                          (verbatim)
```

Same shape on NACK: the slave's error code surfaces as a hub NACK with the
slave's error message string preserved.

### 5.2 Async slave packet surfaces in the right Studio pane

A LightFX slave broadcasts a `LANDING_LIGHT_STATUS` (0x56) unsolicited:

```
slv  → hub  : LANDING_LIGHT_STATUS tag=ASYNC [slot=1, phase=2, finished=0]
hub  → host : LANDING_LIGHT_STATUS tag=ASYNC [...]               (verbatim)
```

The host's `lightfx.Register()` handler decodes it normally and the LightFX
tab updates. The packet-type byte alone identifies the source — the host has
no need to know whether the connection is direct or hub-routed.

### 5.3 Why this is *not* the right tool for hub-internal config-apply

When HubFX boots, it loads `/lightfx.yaml` into a `LightProgramConfig`, and
`lightConfig.onLoaded(...)` calls `pushLightFxConfigToSlave(cfg, *client)` —
a plain function that calls `client.ledMasterBrightness`,
`client.servoSettings`, `client.landingLightBind` directly on the typed
`LightFxClient`. The hub already owns the typed client, so adding a routing
envelope would only buy an extra serialization hop.

Routing usage is gated to "code that does not own a typed client" — i.e. an
external host on the other end of the USB cable.

---

## 6. Migration history

- **v0.x (subcmd)** — `SLAVE_ROUTE_GUNFX/LIGHTFX/GEARCONTROL` (0x96–0x98)
  carried a subcmd byte and dropped the slave's response payload. Removed.
- **v0.y (slot envelope)** — `ROUTE_DOWN/UP/UP_ASYNC` (0xB3–0xB5) and
  `INVALID_SLOT/SLOT_EMPTY` (0x90–0x91) wrapped traffic in a slot-keyed
  envelope. Removed in HubFX firmware v1.0.0.
- **v1.0.0 (current)** — auto-routing by packet-type range. No envelope.
  `SLAVE_ENUM_REQ/RESP` retained for client discovery.
