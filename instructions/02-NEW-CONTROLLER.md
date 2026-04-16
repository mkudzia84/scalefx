# Creating a New Controller

> **ACTION DOCUMENT:** Step-by-step guide for creating a new server controller.

---

## Prerequisites

```yaml
Required:
  - PlatformIO CLI installed
  - Go toolchain (for CLI)
  - Unused packet type range (see below)

Choose_Packet_Range:
  Available:
    - "0x80-0x9F"  # Recommended for next controller
    - "0xA0-0xBF"
    - "0xC0-0xDF"
    - "0xE0-0xEE"
  Used:
    - "0x01-0x2F"  # GunFX
    - "0x40-0x5F"  # LightFX
    - "0x60-0x7F"  # GearControl
  Reserved:
    - "0x30-0x3F"  # Future use
    - "0xF0-0xFF"  # Core system (0xEF = STATUS_UPDATE)
```

---

## Architecture Overview

Every Pico server controller follows the same architecture:

```
┌──────────────────────────────────────────────────────────────┐
│  SfxServer  (common boilerplate — serial, indicators, etc.) │
│  ┌────────────────────────┐  ┌─────────────────────────────┐ │
│  │ CoreCommandServer      │  │ NewFxServer                 │ │
│  │ (extends BusServer)    │  │ (extends BusServer)         │ │
│  │ handles: 0xF0-0xFF     │  │ handles: 0x80-0x9F          │ │
│  └────────────────────────┘  └─────────────────────────────┘ │
│                 ↑ priority 1             ↑ priority 2        │
│  ┌──────────────┴────────────────────────┴──────────────────┐│
│  │                  CommandRouter                            ││
│  │         Chain of Responsibility dispatch                  ││
│  └───────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────┘
```

**Key classes:**
- **SfxServer** — Common boilerplate (serial, device name, indicators, CoreCommandServer, CommandRouter, connection timeout)
- **BusServer** — Base class for all server command handlers (ACK/NACK helpers, packet range routing)
- **CoreCommandServer** — Handles INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE, STATUS_REQ, I2C_SCAN (extends BusServer)
- **NewFxServer** — Your module-specific handler (extends BusServer)

**What SfxServer handles automatically:**
- USB serial init (6Mbps baud, 3s wait)
- Unique device name from Pico board ID (e.g., "NewFX-A1B2")
- Indicator LEDs on GP13/GP14 via nested `SfxServer::IndicatorLedManager`
- CoreCommandServer with board info and INIT/SHUTDOWN/REBOOT/BOOTSEL callbacks
- CommandRouter with automatic handler priority (core first, then module)
- Connection timeout / watchdog detection (15s)
- Free RAM updates in STATUS response
- I2C bus scan infrastructure (optional)

---

## Step 1: Create Directory Structure

```bash
mkdir -p controllers/newfx/pico/src
```

**Result:**
```
controllers/newfx/pico/
├── src/
│   └── newfx_pico.ino
├── platformio.ini
└── README.md
```

---

## Step 2: Create platformio.ini

**File:** `controllers/newfx/pico/platformio.ini`

```ini
; PlatformIO configuration for NewFX Pico

[env:pico]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git
board = pico
framework = arduino
board_build.core = earlephilhower

monitor_speed = 6000000

build_flags =
    -DUSE_TINYUSB=0
    -DSCALEFX_SERVER

lib_deps =
    ${common.lib_deps}

[common]
lib_deps =
    ../../lib/sfx_serial
    ../../lib/sfx_server
    ../../lib/sfx_platform
    ; Add other component libs as needed:
    ; ../../lib/led_control
    ; ../../lib/srv_control
    ; ../../lib/pwm_control
```

**Note:** `SCALEFX_SERVER` macro excludes USB Host / client-side code from the build.

---

## Step 3: Create Server Handler

**File:** `controllers/lib/sfx_serial/serial/newfx/newfx.h` (NEW FILE)

This single header contains everything for the new module: packet types, error codes, validation constants, data types, and server class.

```cpp
/*
 * Serial NewFX Protocol - Binary Protocol Client/Server
 *
 * Binary COBS protocol client/server for NewFX controller.
 *   - NewFxServer: For NewFX Pico (receives commands, extends BusServer)
 *   - NewFxClient: For HubFX (sends commands, extends BusClient)
 *
 * Packet Types (0x80-0x9F range):
 *   COMMAND_1 (0x80) - [param1:u16LE][param2:u8] Description
 *   COMMAND_2 (0x81) - [id:u8] Description
 */

#ifndef SERIAL_NEWFX_H
#define SERIAL_NEWFX_H

#include <Arduino.h>
#include <functional>
#include "serial/client/bus_client.h"
#include "serial/core/bus_server.h"

// ============================================================================
// NewFX Packet Types (0x80-0x9F range)
// ============================================================================

namespace NewFxPacket {
    constexpr uint8_t COMMAND_1 = 0x80;  // [param1:u16LE][param2:u8]
    constexpr uint8_t COMMAND_2 = 0x81;  // [id:u8]
}

// ============================================================================
// NewFX Error Codes (use your assigned error range)
// ============================================================================

namespace NewFxError {
    using namespace SerialError;  // Import generic error codes

    constexpr uint8_t INVALID_PARAM_1 = 0x70;
    constexpr uint8_t INVALID_PARAM_2 = 0x71;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case INVALID_PARAM_1: return "Invalid param1";
            case INVALID_PARAM_2: return "Invalid param2";
            default: return SerialError::getMessage(code);
        }
    }
}

// ============================================================================
// NewFX Validation Constants
// ============================================================================

namespace NewFxSpec {
    constexpr uint8_t MAX_ID = 4;

    inline bool isValidId(uint8_t id)     { return id >= 1 && id <= MAX_ID; }
    inline bool isValidParam1(uint16_t v) { return v <= 10000; }
}

// ============================================================================
// NewFxServer — Command handler (extends BusServer)
// ============================================================================

class NewFxServer : public BusServer {
public:
    // Callback types
    using Command1Callback = std::function<uint8_t(uint16_t param1, uint8_t param2)>;
    using Command2Callback = std::function<uint8_t(uint8_t id)>;

    // Callback registration
    void onCommand1(Command1Callback cb) { _onCommand1 = cb; }
    void onCommand2(Command2Callback cb) { _onCommand2 = cb; }

    const char* handlerName() const override { return "NewFxServer"; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override {
        switch (type) {
            case NewFxPacket::COMMAND_1: {
                SFX_REQUIRE_LEN(3);
                uint16_t p1 = CoreProtocol::getU16LE(payload);
                uint8_t p2 = payload[2];
                SFX_VALIDATE(NewFxSpec::isValidParam1(p1), NewFxError::INVALID_PARAM_1);
                SFX_DISPATCH(_onCommand1, p1, p2);
            }
            case NewFxPacket::COMMAND_2: {
                SFX_REQUIRE_LEN(1);
                uint8_t id = payload[0];
                SFX_VALIDATE(NewFxSpec::isValidId(id), NewFxError::INVALID_PARAM_2);
                SFX_DISPATCH(_onCommand2, id);
            }
            default:
                return CommandHandleResult::NotMyCommand;
        }
    }

    const char* getModuleErrorMessage(uint8_t code) override {
        return NewFxError::getMessage(code);
    }

    uint8_t moduleRangeLow() const override  { return 0x80; }
    uint8_t moduleRangeHigh() const override { return 0x9F; }

private:
    Command1Callback _onCommand1;
    Command2Callback _onCommand2;
};

#endif // SERIAL_NEWFX_H
```

**Key design points:**
- Server extends `BusServer` (not `ICommandHandler` directly)
- Override `handleModulePacket()` — BusServer's `tryProcess()` handles range checking
- Override `moduleRangeLow()`/`moduleRangeHigh()` for automatic packet routing
- Override `getModuleErrorMessage()` for NACK error text lookup
- Error codes, packet types, and validation all live in the same header
- Use `CoreProtocol::getU16LE()` / `CoreProtocol::putU16LE()` for endian-safe reads/writes

---

## Step 4: Update Umbrella Header

**File:** `controllers/lib/sfx_serial/serial/serial.h`

**ACTION:** Add include:

```cpp
// NewFX binary implementation
#include "newfx/newfx.h"
```

---

## Step 5: Create Main Firmware

**File:** `controllers/newfx/pico/src/newfx_pico.ino`

This is the canonical pattern used by all controllers. Study LightFX and GearControl for real examples.

```cpp
/**
 * NewFX Pico Controller v0.1.0
 *
 * Server controller for [description].
 *
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 *
 * Architecture (Chain of Responsibility):
 *   - SfxServer: Common server boilerplate (serial, indicators, core protocol)
 *   - NewFxServer: Handles module-specific commands
 *   - CommandRouter: Routes packets to handlers in priority order
 */

#include <Arduino.h>
#include <serial/serial.h>
#include <sfx_server.h>

#define FIRMWARE_VERSION "0.1.0"
#define BUILD_NUMBER 1

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

SfxServer server;
NewFxServer newfxServer;

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

void performSafeShutdown() {
    // Put ALL hardware into safe state
}

void performSafeInit() {
    performSafeShutdown();
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // 1. Initialize SfxServer
    server.begin("NewFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]() { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });

    // 2. Initialize hardware
    // ...

    // 3. Initialize module server and register callbacks
    newfxServer.begin(&Serial);
    newfxServer.onCommand1([](uint16_t p1, uint8_t p2) -> uint8_t {
        return SerialError::OK;
    });
    newfxServer.onCommand2([](uint8_t id) -> uint8_t {
        return SerialError::OK;
    });

    // 4. Register STATUS data callback
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        return 0;  // Module-specific status bytes
    });

    // 5. Optional: I2C scan
    // server.enableI2CScan(Wire);

    // 6. Finalize router
    server.addModuleHandler(&newfxServer);
}

// ============================================================================
//  MAIN LOOP
// ============================================================================

void loop() {
    server.loop();
    // updateHardware();
    // server.indicators().setErrorCondition(hasError);
    delay(1);
}
```

### setup() Pattern (6 mandatory steps)

| Step | What | Example |
|------|------|---------|
| 1 | `server.begin()` + callbacks | `server.begin("NewFX", VER, BUILD); server.onInit(...); server.onShutdown(...);` |
| 2 | Hardware init | I2C, GPIO, servo pins, LED pins |
| 3 | Module server + callbacks | `newfxServer.begin(&Serial); newfxServer.onCommand1(...);` |
| 4 | STATUS data callback | `server.core().onStatusData([](buf, max) -> size_t { ... });` |
| 5 | I2C scan (optional) | `server.enableI2CScan(Wire); server.addExpectedI2CDevice(...);` |
| 6 | Finalize router | `server.addModuleHandler(&newfxServer);` |

### loop() Pattern

| Step | What | Notes |
|------|------|-------|
| 1 | `server.loop()` | Protocol, timeout, indicators — always first |
| 2 | Hardware updates | Sequences, servos, sensors, state machines |
| 3 | Error conditions | `server.indicators().setErrorCondition(...)` / `.setWarningCondition(...)` |
| 4 | `delay(1)` | Yield CPU time |

---

## Step 6: Add Go CLI Support

### 6.1: Add Packet Constants

**File:** `app/go/protocol/newfx/newfx.go`

```go
package newfx

// NewFX packet types (0x80-0x9F)
const (
    Command1 = 0x80
    Command2 = 0x81
)

// NewFX error codes
const (
    ErrInvalidParam1 = 0x70
    ErrInvalidParam2 = 0x71
)
```

Also add to `PacketTypeName()` switch and error name maps in the appropriate files.

### 6.2: Add Command Builders

**File:** `app/go/protocol/newfx/newfx.go`

```go
func Command1(param1 uint16, param2 uint8) []byte {
    payload := make([]byte, 3)
    binary.LittleEndian.PutUint16(payload[0:], param1)
    payload[2] = param2
    return payload
}
```

### 6.3: Add CLI Handler

**File:** `app/go/engine/handlers/newfx/handler.go`

Create handler functions and register commands in the command list.

### 6.4: Add Response Parsers

**File:** `app/go/engine/handlers/newfx/parsers.go`

Add response parsers for any query commands.

---

## Step 7: Create README

**File:** `controllers/newfx/pico/README.md`

---

## Validation Checklist

```yaml
After_Completion:
  Build:
    - [ ] "pio run" succeeds in controllers/newfx/pico/
    - [ ] All other controllers still build (gunfx, lightfx, gearcontrol, noop)
    - [ ] "cd app/go && go build ./cli/" succeeds

  Runtime:
    - [ ] CLI shows newfx.* commands after connecting
    - [ ] INIT returns INIT_READY with correct device name
    - [ ] STATUS returns module data
    - [ ] GP13 blinks→solid on INIT, GP14 blinks on error

  Documentation:
    - [ ] README.md created with protocol table
    - [ ] AGENTS.md updated with new packet range
```

---

## Real-World Examples

| Controller | Complexity | Key Patterns |
|-----------|------------|--------------|
| **NoOp** | Minimal | Core-only + inline ICommandHandler for servo |
| **GunFX** | Medium | Firing state machine, smoke config, servo jerk |
| **LightFX** | Medium | 8 LED channels with sequence engine, landing lights |
| **GearControl** | Complex | I2C (INA226), motor H-bridge, calibration, battery monitor |

### Component Libraries (`controllers/lib/`)

**Always check here before writing hardware-specific code.** Libraries are split by domain:

| Component | Header | Purpose |
|-----------|--------|---------|
| SfxServer | `sfx_server.h` | Server boilerplate (REQUIRED) |
| LedControlT\<TGpio\> | `led_control.h` | Single-channel LED (on/off, PWM, events). `LedControl` = NativeGpio alias |
| LedManager\<N, TGpio\> | `led_manager.h` | Multi-channel LED manager with sequences. Default TGpio = NativeGpio |
| ExpanderBamT\<T\> | `bam_led_drv.h` | Software BAM for GPIO-only I2C expanders. IS a GPIO provider |
| NativeGpio | `native_gpio.h` | MCU GPIO wrapper (singleton). HW PWM via analogWrite |
| AW9523B | `aw9523b.h` | I2C GPIO expander with 256-step HW LED PWM |
| PCAL6416A | `pcal6416a.h` | I2C GPIO expander (GPIO only, use with ExpanderBamT for PWM) |
| LedEventSeq | `led_event_seq.h` | Looping LED animation sequences |
| ILedEvent | `led_events.h` | Built-in animations (LedOn, LedOff, LedFlashing, etc.) |
| ServoControl | `srv_control.h` | Servo with trapezoidal motion profiling |
| PwmControl | `pwm_control.h` | RC PWM input with averaging and callbacks |
| INA226 | `ina226.h` | TI INA226 power/current/voltage monitor (I2C) |
| I2CDevice | `i2c_device.h` | Base class for I2C peripherals |
| BatteryMonitor | `battery_monitor.h` | ADC battery voltage with low-voltage alerts |
| SfxServer::IndicatorLedManager | `server/sfx_server.h` (nested class) | Connection/error LED state machine |
