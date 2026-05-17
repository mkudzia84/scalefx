# sfx_board — Board-Side Runtime Framework

Single library housing both ends of "what makes a ScaleFX board firmware":

- **`server/`** — the variadic-policy composer (`BoardServer<...>`) and
  every framework piece a controller plugs into it: lifecycle service,
  COBS frame loop, indicator-LED runtime, generic-expander component
  runtime, chunked-stream writer, board-identifier persistence.
- **`client/`** — the master-side typed client (`CoreClient`) for talking
  to a slave running `ComponentServicePolicy`.

## Files

### server/

| File | Purpose |
|------|---------|
| `board_server.{h,cpp}` | `BoardServerBase` (Stream / framer / wire helpers / device name / I²C scan / lifecycle hooks) + `BoardServer<...UserPolicies>` (variadic composer + `process()` loop + dispatch + capabilities). |
| `board_service.{h,cpp}` | `BoardServicePolicy` — INIT / SHUTDOWN / REBOOT / BOOTSEL / KEEPALIVE / STATUS / STATUS_REQ / IDENTIFY / I2C_SCAN / DIAG_HISTORY / STATUS_UPDATE (0xEE..0xFF). Always present in `BoardServer<>`. |
| `component_service.{h,ipp}` | `ComponentServicePolicy<TServos, TPwms, TLedsDed, TLedsBor, TBattery>` — the generic-expander runtime (dispatches the 0x10..0x7F `ComponentPacket` range, emits async events: SERVO_TARGET_REACHED, LED_QUEUE_DONE, PWM_STALL, BATTERY_ALERT, …). |
| `board_identifier.h` | Runtime-assigned, flash-persisted slave identifier (UTF-8, max 32 bytes, `/board.yaml`). |
| `stream.{h,cpp}` | `StreamWriter` — chunked data streaming over COBS with CRC-16/CCITT (`STREAM_BEGIN` / `STREAM_DATA` / `STREAM_END` at 0xA4–0xA6). |

### client/

| File | Purpose |
|------|---------|
| `core_client.{h,cpp}` | `CoreClient` — typed `CommandResult` master-side API for ComponentServicePolicy slaves. Observer chains for async events. |

## Composition model

A firmware instantiates **one** `BoardServer<...UserPolicies>`. Two
mandatory policies (`BoardServicePolicy` for lifecycle, `IndicatorServicePolicy`
for status LEDs) are prepended automatically; everything else is user-supplied:

```cpp
using HubFxBoard = sfx_core::BoardServer<
    AudioServicePolicy<Mixer>,
    StorageServicePolicy<Esp32StoragePolicy>,
    BatteryServicePolicy<Ina226Battery>,
    UsbHostServicePolicy,
    EngineServicePolicy,
    ConfigServicePolicy>;

HubFxBoard board;

void setup() {
    board.begin("HubFx", FIRMWARE_VERSION, BUILD_NUMBER);
    board.setConnectionTimeoutEnabled(false);              // master: no watchdog
    board.policy<BatteryServicePolicy<Ina226Battery>>().bindBattery(...);
    board.enableI2CScan(Wire);
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
INA226 batteryMonitor(Wire, 0x40);
board.addExpectedI2CDevice(0x40, &batteryMonitor);
board.enableI2CScan(Wire);   // wires the I2C_SCAN (0xFB) callback into BoardServicePolicy
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
| `sfx_platform` | `sfx_platform.h` (SfxMutex / SFX_DELAY_MS / SFX_FREE_HEAP / SFX_REBOOT / sfxGetBoardId) |
| `sfx_serial` | wire primitives, `CorePacket` / `SerialError` / `CommandResult`, `BusClient` (for `core_client.cpp`), generic-expander wire format (`components/`) |
| `sfx_peripherals` | `LedControl` (indicator LEDs), `I2CDevice` (I²C scan), `ComponentKind` collection types pulled by `ComponentServicePolicy` |
| `sfx_config` | `ConfigStore` for `BoardIdentifier` flash persistence |
