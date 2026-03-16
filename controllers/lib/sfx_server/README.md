# sfx_server — Server Controller Boilerplate

Common lifecycle infrastructure for all ScaleFX server controllers. Eliminates boilerplate by encapsulating serial init, device naming, indicator LEDs, core protocol handling, connection timeout, and I2C bus scanning into a single `SfxServer` class.

**Used by:** GunFX, LightFX, GearControl, NoOp (Pico servers), HubFX (ESP32-S3 master hub).

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `sfx_server.h` | ~240 | `SfxServer` class + nested `IndicatorLedManager` |
| `sfx_server.cpp` | ~190 | Implementation (init, loop, timeout, I2C scan) |

## Architecture

```
    ┌──────────────────────────────────────────────────┐
    │                  SfxServer                       │
    │                                                  │
    │  ┌─────────────┐  ┌──────────────────────────┐   │
    │  │ CommandRouter│  │  CoreCommandServer       │   │
    │  │ (priority    │  │  INIT, SHUTDOWN, REBOOT, │   │
    │  │  chain)      │  │  BOOTSEL, STATUS,        │   │
    │  │              │  │  I2C_SCAN, DIAG_HISTORY  │   │
    │  │  1. Core ────┼──┤                          │   │
    │  │  2. Module   │  └──────────────────────────┘   │
    │  │  3. Module   │                                 │
    │  └──────────────┘  ┌──────────────────────────┐   │
    │                    │  IndicatorLedManager      │   │
    │                    │  LED 0: Connection        │   │
    │                    │  LED 1: Error / Warning   │   │
    │                    └──────────────────────────┘   │
    │                                                  │
    │  buildDeviceName()  ──── Board ID → "GunFX-A1B2" │
    │  checkConnectionTimeout() ──── 15s watchdog      │
    │  I2C scan ──── expected + extra device discovery  │
    └──────────────────────────────────────────────────┘
```

## Usage Pattern

Every server controller follows this template:

```cpp
#define FIRMWARE_VERSION "0.3.0"
#define BUILD_NUMBER 5

SfxServer server;
MyModuleServer myModule;

void setup() {
    // 1. Initialize server infrastructure
    server.begin("MyCtrl", FIRMWARE_VERSION, BUILD_NUMBER);

    // 2. Register lifecycle callbacks
    server.onInit([]()     { resetHardware(); });
    server.onShutdown([]() { safeHardware();  });

    // 3. Initialize module handler
    myModule.begin(&Serial, server.deviceName());
    // ... register module callbacks ...

    // 4. Register STATUS data callback
    server.core().onStatusData([](uint8_t* buf, size_t max) -> size_t {
        buf[0] = myState;
        return 1;
    });

    // 5. Add module handler to router (core is auto-registered first)
    server.addModuleHandler(&myModule);
}

void loop() {
    server.loop();       // Protocol, timeout, indicators
    updateHardware();    // Module-specific work
    SFX_DELAY_MS(1);
}
```

## What `begin()` Does

1. **USB Serial init** — `Serial.begin(6000000)` with 3-second wait for connection
2. **Device name** — builds unique name from board ID (e.g., `"GunFX"` + last 4 hex of board ID → `"GunFX-A1B2"`)
3. **Indicator LEDs** — initializes connection (default GP13) and error (default GP14) LEDs. Pass `-1` to disable.
4. **DiagLog** — initializes diagnostic log singleton with serial output
5. **CoreCommandServer** — configures board info (name, version, platform, CPU MHz, heap, build number)
6. **System callbacks** — registers INIT → `doInit()`, SHUTDOWN → `doShutdown()`, REBOOT → `doShutdown()` + `SFX_REBOOT()`, BOOTSEL → `doShutdown()` + `sfxRebootToBootloader()`

## What `loop()` Does

1. **`_router.process()`** — reads serial, decodes COBS, dispatches to handler chain
2. **Activity forwarding** — updates `CoreCommandServer` activity timestamp from router
3. **Free RAM update** — calls `SFX_FREE_HEAP()` for STATUS response
4. **Connection timeout** — if enabled, triggers `doShutdown()` after 15s inactivity
5. **Indicator update** — updates LED states based on connection/error/warning flags

## Indicator LED State Machine

### LED 0 — Connection (default GP13)

| State | Behavior | Meaning |
|-------|----------|---------|
| Waiting | Blink 500ms | Power on, no INIT received |
| Connected | Solid ON | INIT received, operating normally |
| Watchdog | OFF | Connection lost (15s timeout) |

### LED 1 — Error/Warning (default GP14)

Three-tier priority (highest wins):

| Condition | Behavior | Rate |
|-----------|----------|------|
| Error | Fast blink | 200ms |
| Warning | Slow blink | 500ms |
| Normal | OFF | — |

### Setting Conditions

```cpp
// In loop(): controllers set error/warning flags
server.indicators().setErrorCondition(hasCriticalFault);
server.indicators().setWarningCondition(lowVoltage);

// SfxServer manages connection state automatically:
// - doInit()     → connected=true, watchdog=false
// - doShutdown() → connected=false
// - timeout      → doShutdown() + watchdog=true
```

## Connection Timeout

Default: **15,000 ms** of serial inactivity triggers `doShutdown()`.

**Slave controllers** (GunFX, LightFX, GearControl): timeout is enabled — if HubFX stops polling, the slave shuts down safely.

**Master controllers** (HubFX): timeout is disabled — the hub operates autonomously.

```cpp
// HubFX disables timeout because it's the master
server.setConnectionTimeoutEnabled(false);
server.indicators().setConnected(true);  // Always operational
```

## Command Handler Chain

`addModuleHandler()` uses lazy initialization:

1. **First call** → creates `CommandRouter`, registers `CoreCommandServer` at priority 1
2. **Subsequent calls** → adds module handlers in order (priority 2, 3, ...)

This guarantees core commands (INIT, STATUS, REBOOT, etc.) are always handled first.

```cpp
// Single-domain controller (GunFX, LightFX, GearControl)
server.addModuleHandler(&gunfxServer);

// Multi-domain controller (HubFX — audio, engine, storage, slaves)
server.addModuleHandler(&audioServer);
server.addModuleHandler(&engineServer);
server.addModuleHandler(&storageServer);
server.addModuleHandler(&slaveServer);
```

## I2C Bus Scan

Optional feature for discovering I2C devices on the bus. Activated by calling `enableI2CScan()`, which registers a callback with `CoreCommandServer` for the `I2C_SCAN` packet type.

```cpp
// In setup():
INA226 batteryMonitor;
TAS5825M audioCodec;

server.addExpectedI2CDevice(0x40, &batteryMonitor);  // INA226 at 0x40
server.addExpectedI2CDevice(0x4C, &audioCodec);       // TAS5825M at 0x4C
server.enableI2CScan(Wire);
```

### Scan Response

The scan reports two categories:

1. **Expected devices** — pre-registered via `addExpectedI2CDevice()`. Each reports:
   - `address` — 7-bit I2C address
   - `found` — ACK received on the bus (`I2CDevice::probe()`)
   - `identified` — device-specific identity check passed (`I2CDevice::isAvailable()`)

2. **Extra devices** — found on the bus but not in the expected list. Reports address only.

The I2C scan probes addresses 0x08–0x77 (standard 7-bit range).

## Cross-Platform Compatibility

`SfxServer` is fully cross-platform with **no ESP32-specific code**. All platform differences are handled by `sfx_platform.h` abstractions:

| Operation | Pico (RP2040/RP2350) | ESP32-S3 |
|-----------|----------------------|----------|
| Serial | UART over USB CDC | UART0 via USB-UART bridge |
| Board ID | Flash unique ID (`pico_get_unique_board_id`) | MAC address (`esp_efuse_mac_get_default`) |
| Reboot | `rp2040.reboot()` | `esp_restart()` |
| Bootloader | `rp2040.rebootToBootloader()` (BOOTSEL/UF2) | `esp_restart()` (no equivalent — uses OTA/esptool) |
| Free heap | `rp2040.getFreeHeap()` | `esp_get_free_heap_size()` |
| CPU MHz | `F_CPU / 1000000` | `getCpuFrequencyMhz()` |
| Delay | `busy_wait_ms()` | `vTaskDelay(pdMS_TO_TICKS())` |
| LED control | `digitalWrite()` (Arduino) | `digitalWrite()` (Arduino) |
| I2C | `Wire` (Arduino TwoWire) | `Wire` (Arduino TwoWire) |

No conditional compilation (`#ifdef`) exists in `sfx_server.h` or `sfx_server.cpp`. Everything resolves through the platform abstraction layer.

### Indicator LED Pin Defaults

The default GPIO pins (13, 14) are overridable via `begin()` parameters. Pass `-1` to disable a pin:

```cpp
// Custom pins for boards with different LED wiring
server.begin("HubFX", VERSION, BUILD, /*connectionPin=*/2, /*errorPin=*/4);

// ESP32-S3 DevKitC-1: onboard LED on GPIO48, no error LED
server.begin("HubFX", VERSION, BUILD, /*connectionPin=*/48, /*errorPin=*/-1);
```

## Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `BAUD_RATE` | 6,000,000 | USB serial baud rate |
| `CONNECTION_TIMEOUT_ms` | 15,000 | Inactivity before watchdog shutdown |
| `BLINK_WAITING_ms` | 500 | Connection LED blink rate (waiting) |
| `BLINK_ERROR_ms` | 200 | Error LED blink rate |
| `BLINK_WARNING_ms` | 500 | Warning LED blink rate |
| `MAX_EXPECTED_I2C` | 8 | Max registered I2C devices for scan |

## Dependencies

| Dependency | Reason |
|------------|--------|
| `sfx_platform` | Platform abstraction (delays, heap, reboot, board ID, mutexes) |
| `sfx_serial` | `CoreProtocol`, `CommandRouter`, `CoreCommandServer`, `BusServer`, `ICommandHandler`, `DiagLog` |
| `sfx_peripherals` | `LedControl` (indicator LEDs), `I2CDevice` (I2C scan) |
| `Arduino` | `Serial`, `Wire`, `pinMode`, `digitalWrite`, `millis` |

**No dependency on:** sfx_storage, sfx_audio, sfx_usb.

## Backward Compatibility

The type alias `PicoServer = SfxServer` exists for backward compatibility but is deprecated. Use `SfxServer` directly.
