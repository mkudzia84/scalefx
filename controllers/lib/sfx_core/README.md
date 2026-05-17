# sfx_core

Generic component-server template used by **every ScaleFX board that
exposes ports** — HubFX (master, for its own local components) and
expander boards alike.  See
[`instructions/15-GENERIC-EXPANDER-REFACTOR.md`](../../../instructions/15-GENERIC-EXPANDER-REFACTOR.md)
for the architectural context.

## Contents

```
core/                          server-side bits (board firmware)
├── core_server.h              — CoreServer<TServos, TPwms, TLedsDed, TLedsBor, TBattery>
├── core_server.ipp            — implementation (template definitions)
└── board_identifier.h         — runtime-assigned, flash-persisted identifier (/board.yaml)

client/                        master-side bits
├── core_client.h              — CoreClient (typed CommandResult API + observer chain)
└── core_client.cpp            — wire-format encode / decode + async event fanout

library.json                   — PlatformIO manifest
```

`sfx_core` is symmetric: it ships **both** the firmware-side server
(under `core/`) and the master-side client (under `client/`), mirroring
the pattern used by `sfx_storage`, `sfx_config`, and `sfx_peripherals/led`.
HubFX firmware pulls in *both* (it acts as a master to expanders AND
runs CoreServer locally for its own ports); expander firmware pulls in
only the server.

## What it owns

- Wire-level dispatch of the entire `0x01..0x7F` `ComponentPacket` range
  (component enumeration, identifier get/set, servo/PWM/LED commands,
  PWM_RECONFIGURE, battery info / reconfigure, async events).
- Lifecycle: `IDLE` → `SLAVE`/`DIRECT` → `IDLE` (no `STANDALONE` per
  the pivot — boards never run autonomously).
- Keepalive watchdog → `enterSafeState()` on timeout in SLAVE mode
  (DIRECT mode does not enforce).
- Async-event emission to master (`SERVO_TARGET_REACHED`,
  `LED_QUEUE_DONE`) wired from the component-collection callbacks.

## What boards instantiate

A slave board firmware is now ~50 lines:

1. Pick channel counts: `ServoCollection<N>`, `PwmCollection<M>`,
   `LedCollection<K, TGpio>`.
2. Supply per-channel `ServoSpec` / `PwmSpec` / `LedSpec` arrays
   describing pins + capability flags.
3. Construct one `CoreServer<TServos, TPwms, TLedsDed, TLedsBor>`,
   `bind()` the collections + identifier + flash storage callbacks.
4. Hook `SfxServer::onInit / onShutdown / onKeepalive` to the
   board's lifecycle entry points.
5. Call `core.update()` from `loop()`.

## Implementation status

| File | Status |
|---|---|
| `core/core_server.h`   | landed — full API declared, two-LED-pool split |
| `core/core_server.ipp` | landed — dispatch routing, mode-change cleanup, async emitters |
| `client/core_client.*` | landed — typed CommandResult API + observer chain |

### Known follow-ups

- **PWM driver gap.**  `PwmCollection::setDuty` currently uses
  `analogWrite()` as a placeholder.  A typed `sfx_peripherals/pwm/`
  `PwmOutput` driver is needed for proper frequency control.
- **BATCH support.**  `ComponentPacket::BATCH_EXEC` / `BATCH_LOAD` /
  `BATCH_TRIGGER` packet IDs are defined in `serial/components/`
  but the CoreServer dispatch code isn't wired yet.

## Build

The library auto-discovers via PlatformIO when a project's
`platformio.ini` includes the parent `controllers/lib/` directory in
its lib path.  No special compile flags.
