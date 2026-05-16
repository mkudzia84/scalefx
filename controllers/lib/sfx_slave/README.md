# sfx_slave

Generic slave-server template that every post-pivot ScaleFX slave
firmware uses.  See
[`instructions/15-GENERIC-SLAVE-REFACTOR.md`](../../../instructions/15-GENERIC-SLAVE-REFACTOR.md)
for the architectural context.

## Contents

```
slave/                         server-side (slave firmware) bits
├── slave_server.h             — SlaveServer<TServos, TPwms, TLeds, TBattery> declaration
├── slave_server.ipp           — implementation (template definitions)
└── board_identifier.h         — runtime-assigned, flash-persisted identifier (/board.yaml)

client/                        master-side bits
├── slave_client.h             — SlaveClient (typed CommandResult API + observer chain)
└── slave_client.cpp           — wire-format encode / decode + async event fanout

library.json                   — PlatformIO manifest
```

`sfx_slave` is intentionally symmetric: it ships **both** the slave-
firmware server (under `slave/`) and the master-firmware client
(under `client/`), mirroring the pattern used by `sfx_storage`,
`sfx_config`, and `sfx_peripherals/led`.  HubFX firmware pulls in the
client; slave firmware pulls in the server.  Same library, different
entry points.

## What it owns

- Wire-level dispatch of the entire `0x01..0x7F` `SlavePacket` range
  (component enumeration, identifier get/set, servo/PWM/LED commands,
  PWM_RECONFIGURE, battery info / reconfigure, async events).
- Lifecycle: `IDLE` → `SLAVE`/`DIRECT` → `IDLE` (no `STANDALONE` per
  the pivot — boards never run autonomously).
- Keepalive watchdog → `enterSafeState()` on timeout in SLAVE mode
  (DIRECT mode does not enforce).
- Async-event emission to master (`SERVO_TARGET_REACHED`,
  `LED_PROGRAM_DONE`) wired from the component-collection callbacks.

## What boards instantiate

A slave board firmware is now ~50 lines:

1. Pick channel counts: `ServoCollection<N>`, `PwmCollection<M>`,
   `LedCollection<K, TGpio>`.
2. Supply per-channel `ServoSpec` / `PwmSpec` / `LedSpec` arrays
   describing pins + capability flags.
3. Construct one `SlaveServer<TServos, TPwms, TLeds>`, `bind()` the
   collections + identifier + flash storage callbacks.
4. Hook `SfxServer::onInit / onShutdown / onKeepalive` to the
   slave's lifecycle entry points.
5. Call `slave.update()` from `loop()`.

See [`examples/gunfx_slave_example.cpp`](examples/gunfx_slave_example.cpp)
for a full working sketch.

## Implementation status (2026-05-06)

| File | Status |
|---|---|
| `slave_server.h`     | landed — full API declared |
| `slave_server.ipp`   | landed — first cut; dispatch + safe-state + async emitters complete |
| `examples/gunfx_slave_example.cpp` | reference only — not built |

### Known follow-ups (next sessions)

- **PWM driver gap.**  `PwmCollection::setDuty` currently uses
  `analogWrite()` as a placeholder.  A typed
  `sfx_peripherals/pwm/PwmOutput` driver is needed for proper
  frequency control (LEDC on ESP32, hardware PWM slices on RP2040,
  expander BAM for I/O-expander pins).
- **LedManager program-completion event.**  `LedCollection::runProgram`
  needs a callback bridge from `LedEventSeq` → `_onProgramDone`
  when a non-repeating program ends.  The plumbing exists; the
  bottom-half wire is TBD.
- **Master-side `SlaveApi`.**  The Go SDK + master-side C++ client
  still talk the legacy per-board protocols.  Migration of the
  master happens once the slave side is stable.
- **ServoControl method names.**  The `.ipp` assumes
  `setTarget()`, `isAtTarget()`, `currentPositionUs()`,
  `targetPositionUs()`, `velocityUsPerSec()` — verify against the
  existing `ServoControl` header before first compile and rename
  if needed.

## Build

The library auto-discovers via PlatformIO when a project's
`platformio.ini` includes the parent `controllers/lib/` directory in
its lib path (the existing slave projects do this).  No special
compile flags.
