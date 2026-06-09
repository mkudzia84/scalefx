# sfx_board — Board-Side Runtime Framework

Single library housing both ends of "what makes a ScaleFX board firmware":

- **`server/`** — the variadic-policy composer (`BoardServer<...>` /
  `BoardOf<...>`) and every framework piece a controller plugs into it:
  lifecycle service, COBS frame loop, indicator-LED runtime, hub-local
  **port** registry + service, the **role** dispatch service + per-family
  role handlers, the effect clock, chunked-stream writer.
- **`roles/`** — the role layer: per-port actuator / input role classes
  (`ServoActuatorRole`, `LedAnimator`, `DcMotorRole`, `BiDcMotorRole`,
  `HeaterRole`, and the RC-PWM / SBUS / Jeti EX input roles).
- **`motion/`** + **`element/`** — actuator mechanism math (servo motion
  profile, element voltage scaling) owned by the roles (Rule 42).

> Composition / dispatch / role-transport diagrams:
> [instructions/32-ARCHITECTURE-DIAGRAMS.md](../../../instructions/32-ARCHITECTURE-DIAGRAMS.md) §3.

## Files

### server/

| File | Purpose |
|------|---------|
| `board_server.{h,cpp}` | `BoardServerBase` (Stream / framer / wire helpers / device name / I²C scan / lifecycle hooks / effect-clock latch) + `BoardServer<...UserPolicies>` (variadic composer + `process()` loop + dispatch + capabilities). |
| `board_of.h` | `BoardOf<TBoard, TStream, Caps, ...ExtraPolicies>` — CRTP composer that auto-prepends `BoardServicePolicy` + `IndicatorServicePolicy` + `PortServicePolicy` + `RoleServicePolicy`, sizes the hub-local `PortRegistry` from `Caps`, and binds the wire `TStream` for vtable-free per-byte I/O (Rule 33). HubFX uses this form. |
| `board_service.{h,cpp}` | `BoardServicePolicy` — INIT / SHUTDOWN / REBOOT / BOOTSEL / KEEPALIVE / STATUS / STATUS_REQ / IDENTIFY / I2C_SCAN / DIAG_HISTORY / STATUS_UPDATE (0xEE..0xFF). Always present. |
| `effect_clock.h` | `EffectClock` singleton — latched once per `process()` pass; every effect/role reads time from it (Rule 40). |
| `stream.{h,cpp}` | `StreamWriter` — chunked data streaming over COBS with CRC-16/CCITT. |

#### Ports (hub-local port enumeration + binding)

| File | Purpose |
|------|---------|
| `port_service.{h,cpp}` | `PortServicePolicy` — enumerates hub-local ports (PORT_LIST_REQ, voltage/role metadata). Auto-prepended by `BoardOf<>`. |
| `port_registry.h` | `PortRegistry` — the fixed-capacity per-board port table (sized by `PortCapacity<...>`). |
| `port_bindings.h` / `port_descriptor.h` | Compile-time port descriptor lists (`pwm_array<>` / `servo_array<>` / `input_array<>` / `hbridge_array<>`) + runtime binding records. |

#### Roles (per-port actuator / input dispatch)

| File | Purpose |
|------|---------|
| `role_service.{h,cpp}` | `RoleServicePolicy` — ROLE_ATTACH / DETACH / LIST + drive/query dispatch; auto-prepended by `BoardOf<>`. Defers per-family handling to the handlers below. |
| `role_registry.h` | THE single role-kind ⇄ `RoleKind` map (`roleKindFor<T>()` / `forEachAttachedRole`) — Rule 58 (no per-role `switch` anywhere else). |
| `role_event_emitter.{h,cpp}` | Emits role telemetry as GUID-tagged async events. |
| `role_servo_handler.*` / `role_led_handler.*` / `role_motor_handler.*` / `role_bimotor_handler.*` / `role_heater_handler.*` | Per-family role command handlers. |
| `role_rcpwm_input_handler.*` / `role_sbus_input_handler.*` / `role_jeti_input_handler.*` | Per-protocol RC input role handlers. |

### roles/

The role classes themselves (the actuator-mechanism layer, Rule 42):
`servo_actuator_role`, `led_animator`, `dc_motor_role`, `bi_dc_motor_role`,
`heater_role`, `rc_pwm_input_role`, `sbus_input_role`, `jeti_ex_input_role`
(+ `jeti_ex_telemetry_role`, `input_broadcaster`). Mechanism math lives in
`motion/motion_profile.h` (servo trapezoidal/S-curve) and
`element/element_scaling.h` (sub-rail voltage duty scaling).

> **Legacy:** `ComponentServicePolicy<...>` / `ComponentPacket` (the old
> generic-expander runtime over 0x10..0x7F) and the master-side `CoreClient`
> are superseded by the Port + Role services above and the GUID-addressed
> Topology transport (Rule 58). The runtime-assigned `/board.yaml`
> `BoardIdentifier` is retired — per-board aliases now live in the hub's
> `/hubfx.yaml` `expanders:` block.

## Composition model

A firmware instantiates **one** board composer. The flat form
`BoardServer<...UserPolicies>` prepends two mandatory policies
(`BoardServicePolicy` lifecycle, `IndicatorServicePolicy` status LEDs); the
richer `BoardOf<TBoard, TStream, Caps, ...>` form (used by HubFX) additionally
prepends `PortServicePolicy` + `RoleServicePolicy` and sizes the hub-local
port registry from `Caps`. Everything else is user-supplied:

```cpp
class HubFxBoard : public sfx_core::BoardOf<HubFxBoard,
                                            sfx::NativeUartStream,        // wire stream (Rule 33)
                                            sfx_core::PortCapacity<10,8,0,2>,
                                            StorageService,
                                            AudioService,
                                            ConfigServicePolicy> { /* drivers, ports */ };

HubFxBoard board;

void setup() {
    board.begin(wireUart, FIRMWARE_VERSION, BUILD_NUMBER, ledPin, errPin);
    board.setConnectionTimeoutEnabled(false);              // master: no watchdog
    board.enableI2CScan(hubI2cBus());
}

void loop() {
    board.process();
}
```

`begin()` does Serial setup, device-name building (silicon ID), DiagLog
init, every policy's `begin(this)`, indicator-LED pin configuration,
board-info + capability seed into `BoardServicePolicy`, and lifecycle
callback wiring (INIT → SLAVE/DIRECT state, SHUTDOWN, REBOOT → reset,
BOOTSEL on Pico).

`process()` reads available bytes through the COBS framer, dispatches
each CRC-valid frame to the first owning policy, ticks every policy's
`update()`, updates activity / free-RAM on `BoardServicePolicy`, and
fires the connection-timeout watchdog.

## SystemServicePolicy contract

A class satisfies the `sfx_core::SystemServicePolicy` concept (C++20)
by providing:

```cpp
class MyPolicy {
public:
    static constexpr uint32_t kCapabilityBits = CoreCapability::AUDIO;  // OR'd into IDENTIFY

    bool begin(sfx_core::BoardServerBase* ctx) { _ctx = ctx; return true; }
    bool ownsType(uint8_t type) const          { return type == HubFxPacket::AUDIO_PLAY; }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len) {
        SFX_REQUIRE_LEN(2);
        SFX_VALIDATE(channelValid(payload[0]), AudioError::INVALID_CHANNEL);
        SFX_DISPATCH(_playCb, payload[0], payload[1]);
    }

    void update() { /* per-loop tick */ }

    // Optional:
    const char* getErrorMessage(uint8_t code) const { return AudioError::getMessage(code); }
};
```

Wire helpers are inherited from `BoardServerBase` via `_ctx` (no virtual
dispatch — concrete methods). Use `_ctx->sendAck()`, `_ctx->sendNack(code)`,
`_ctx->sendRawPacket(type, tag, payload, len)`, `_ctx->currentTag()`,
`_ctx->serial()`.

## Indicator LED state

Two GPIOs driven by `IndicatorServicePolicy` (always prepended):

| LED | State | Behaviour |
|-----|-------|-----------|
| Connection | Waiting | Blink 500 ms |
| Connection | Connected | Solid ON |
| Connection | Watchdog | OFF |
| Error/Warn | Error | Fast blink (200 ms) |
| Error/Warn | Warning | Slow blink (500 ms) |
| Error/Warn | Normal | OFF |

Firmware sets state via `board.indicators().setErrorCondition(true)` /
`.setWarningCondition(true)`. Connection state is managed by the
framework via INIT/SHUTDOWN/timeout.

## I²C bus scan

Optional. Register expected devices then bind the wire:

```cpp
board.addExpectedI2CDevice(0x40);
board.enableI2CScan(bus);   // wires the I2C_SCAN (0xFB) callback into BoardServicePolicy
                            // `bus` is the native SfxI2cBus (e.g. hubI2cBus())
```

The scan probes 0x08..0x77 and reports expected vs. extra devices.

## Constants

| Constant                                | Value     | Purpose                       |
|-----------------------------------------|-----------|-------------------------------|
| `BoardServerBase::BAUD_RATE`            | 6 000 000 | USB-CDC baud                  |
| `BoardServerBase::CONNECTION_TIMEOUT_ms`| 15 000    | Watchdog inactivity threshold |
| `BoardServerBase::MAX_EXPECTED_I2C`     | 8         | I²C-scan registry slots       |
| `BoardServerBase::RX_BUFFER_SIZE`       | platform  | COBS frame rx buffer          |

## Dependencies

| Dependency | Reason |
|------------|--------|
| `sfx_platform` | `sfx_platform.h` (SfxMutex / SFX_DELAY_MS / SFX_FREE_HEAP / SFX_REBOOT / `SFX_MILLIS` / sfxGetBoardId) |
| `sfx_serial` | wire primitives, `CorePacket` / `SerialError` / `CommandResult`, `BusClient` |
| `sfx_peripherals` | `LedControl` (indicator LEDs), native `SfxI2cBus` / `I2CDevice` (I²C scan), PWM / servo backends the port roles drive |
