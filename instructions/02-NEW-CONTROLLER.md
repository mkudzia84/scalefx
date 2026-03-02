# Creating a New Controller

> **ACTION DOCUMENT:** Step-by-step guide for creating a new server controller.

---

## Prerequisites

```yaml
Required:
  - PlatformIO CLI installed
  - Python 3.8+ with pyserial
  - Unused packet type range (see README.md)
  
Choose_Packet_Range:
  Available:
    - "0x60-0x7F"  # Recommended for next controller
    - "0x80-0x9F"
    - "0xA0-0xBF"
    - "0xC0-0xDF"
    - "0xE0-0xEF"
  Reserved:
    - "0x01-0x2F"  # GunFX
    - "0x40-0x5F"  # LightFX
    - "0x30-0x3F"  # Future use
    - "0xF0-0xFF"  # Core system
```

---

## Step 1: Create Directory Structure

```bash
# ACTION: Run these commands
mkdir -p controllers/newfx/pico/src
mkdir -p controllers/newfx/pico/tests
```

**Result:**
```
controllers/newfx/pico/
├── src/
└── tests/
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

monitor_speed = 115200

build_flags = 
    -DUSE_TINYUSB=0
    -DSCALEFX_SERVER

lib_deps =
    ${common.lib_deps}

[common]
lib_deps = 
    ../../lib/serial
    ; Add other libs as needed:
    ; ../../lib/led_control
    ; ../../lib/srv_control
```

---

## Step 3: Add Packet Types

**File:** `controllers/lib/serial/serial_newfx.h` (NEW FILE — packet types live in the module header)

**ACTION:** Define packet type namespace in the new header:

```cpp
// NewFX packet types (0x60-0x7F)
namespace NewFxPacket {
    constexpr uint8_t COMMAND_1     = 0x60;
    constexpr uint8_t COMMAND_2     = 0x61;
    constexpr uint8_t COMMAND_3     = 0x62;
    // Add more as needed, stay within 0x60-0x7F
}
```

---

## Step 4: Add Error Codes

**File:** `controllers/lib/serial/serial_error.h`

**ACTION:** Add namespace after existing ones:

```cpp
// NewFX-specific errors (0x30-0x3F recommended)
namespace NewFxError {
    constexpr uint8_t OK              = 0x00;
    constexpr uint8_t INVALID_PARAM_1 = 0x30;
    constexpr uint8_t INVALID_PARAM_2 = 0x31;
    // Add more as needed
    
    inline const char* name(uint8_t code) {
        switch (code) {
            case INVALID_PARAM_1: return "INVALID_PARAM_1";
            case INVALID_PARAM_2: return "INVALID_PARAM_2";
            default: return SerialError::name(code);
        }
    }
}
```

---

## Step 5: Create Server Handler

**File:** `controllers/lib/serial/serial_newfx.h` (NEW FILE)

```cpp
#pragma once

#include "serial_core.h"
#include "serial_error.h"
#include <functional>

// NewFX packet types (0x60-0x7F)
namespace NewFxPacket {
    constexpr uint8_t COMMAND_1 = 0x60;
    constexpr uint8_t COMMAND_2 = 0x61;
}

// NewFX validation constants
namespace NewFxSpec {
    constexpr uint8_t MAX_ID = 4;
    
    inline bool isValidId(uint8_t id)    { return id >= 1 && id <= MAX_ID; }
    inline bool isValidParam1(uint16_t v) { return v <= 10000; }
}

/**
 * NewFxServer - Command handler for NewFX controller
 * 
 * Packet range: 0x60-0x7F
 * Uses SFX_* macros for handler boilerplate (see serial_core.h)
 */
class NewFxServer : public ICommandHandler {
public:
    using Command1Callback = std::function<uint8_t(uint16_t param1, uint8_t param2)>;
    using Command2Callback = std::function<uint8_t(uint8_t id)>;
    
    void begin(Stream* serial) { _serial = serial; }
    
    void onCommand1(Command1Callback cb) { _onCommand1 = cb; }
    void onCommand2(Command2Callback cb) { _onCommand2 = cb; }
    
    // Uses SFX_* macros — see serial_core.h for definitions
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override {
        switch (type) {
            case NewFxPacket::COMMAND_1: {
                SFX_REQUIRE_LEN(3);
                uint16_t p1 = getU16LE(payload);
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

    const char* handlerName() const override { return "NewFxServer"; }

private:
    Stream* _serial = nullptr;
    Command1Callback _onCommand1;
    Command2Callback _onCommand2;
};
```

---

## Step 6: Update Umbrella Header

**File:** `controllers/lib/serial/serial.h`

**ACTION:** Add include:

```cpp
#include "serial_newfx.h"
```

---

## Step 7: Create Main Firmware

**File:** `controllers/newfx/pico/src/newfx_pico.ino`

```cpp
/**
 * NewFX Pico - [Description]
 * Firmware Version: 0.1.0
 */

#include <Arduino.h>
#include <serial.h>

// Version info
#define FIRMWARE_VERSION "0.1.0"
#define BOARD_TYPE "RP2040"
#define BUILD_NUMBER 1

// Handlers
CommandRouter commandRouter;
CoreCommandServer coreServer;
NewFxServer newfxServer;

// State variables
bool initialized = false;

// Forward declarations
void performSafeInit();
void performSafeShutdown();
uint8_t handleCommand1(uint16_t param1, uint8_t param2);
uint8_t handleCommand2(uint8_t id);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);  // Wait for USB
    
    // Configure core handler
    coreServer.begin(&Serial);
    coreServer.setBoardInfo("NewFX", FIRMWARE_VERSION, BOARD_TYPE, 
                              rp2040.f_cpu() / 1000000, rp2040.getFreeHeap(),
                              BUILD_NUMBER);
    coreServer.onInit(performSafeInit);
    coreServer.onShutdown(performSafeShutdown);
    coreServer.onReboot([]() { rp2040.reboot(); });
    coreServer.onBootsel([]() { rp2040.rebootToBootloader(); });
    
    // Register module-specific STATUS data callback
    coreServer.onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        // Append module-specific bytes to STATUS response
        // Return number of bytes written
        return 0;  // TODO: implement module status
    });
    coreServer.onInit(performSafeInit);
    coreServer.onShutdown(performSafeShutdown);
    coreServer.onReboot([]() { rp2040.reboot(); });
    coreServer.onBootsel([]() { rp2040.rebootToBootloader(); });
    
    // Configure module handler
    newfxServer.begin(&Serial);
    newfxServer.onCommand1(handleCommand1);
    newfxServer.onCommand2(handleCommand2);
    
    // Configure router
    commandRouter.begin(&Serial, [](uint8_t err, uint8_t type) {
        newfxServer.sendNack(err);
    });
    commandRouter.addHandler(&coreServer);
    commandRouter.addHandler(&newfxServer);
}

void loop() {
    commandRouter.poll();
    coreServer.updateFreeRam(rp2040.getFreeHeap());
    
    // Add periodic tasks here
}

// ============================================================
// Callbacks
// ============================================================

void performSafeInit() {
    // Initialize hardware to safe state
    initialized = true;
}

void performSafeShutdown() {
    // Shutdown hardware safely
    initialized = false;
}

uint8_t handleCommand1(uint16_t param1, uint8_t param2) {
    if (!initialized) return SerialError::INVALID_STATE;
    
    // Implement command logic
    // Return NewFxError::OK on success
    // Return specific error code on failure
    
    return NewFxError::OK;
}

uint8_t handleCommand2(uint8_t id) {
    if (!initialized) return SerialError::INVALID_STATE;
    
    // Implement command logic
    
    return NewFxError::OK;
}
```

---

## Step 8: Add Python Packet Constants

**File:** `tests/framework/packets.py`

**ACTION:** Add class:

```python
class NewFxPacket:
    """NewFX packet types (0x60-0x7F)"""
    COMMAND_1 = 0x60
    COMMAND_2 = 0x61
    COMMAND_3 = 0x62


class NewFxError:
    """NewFX error codes (0x30-0x3F)"""
    OK = 0x00
    INVALID_PARAM_1 = 0x30
    INVALID_PARAM_2 = 0x31
    
    @staticmethod
    def name(code: int) -> str:
        names = {
            0x30: "INVALID_PARAM_1",
            0x31: "INVALID_PARAM_2",
        }
        return names.get(code, CoreError.name(code))
```

---

## Step 9: Add Python Command Builders

**File:** `tests/framework/commands.py`

**ACTION:** Add class:

```python
class NewFxCommands:
    """Build NewFX command packets."""
    
    @staticmethod
    def command_1(param1: int, param2: int) -> bytes:
        """Build COMMAND_1 packet."""
        payload = struct.pack('<HB', param1, param2)  # little-endian
        return build_packet(NewFxPacket.COMMAND_1, payload)
    
    @staticmethod
    def command_2(id: int) -> bytes:
        """Build COMMAND_2 packet."""
        payload = struct.pack('<B', id)
        return build_packet(NewFxPacket.COMMAND_2, payload)
```

---

## Step 10: Add to CLI

### ACTION 10.1: Create Handler File

**File:** `tests/cli/handlers/newfx.py` (NEW FILE)

```python
"""NewFX CLI command handler."""
from typing import Dict, List, Tuple, Callable
from tests.cli.base import CommandHandlerBase, CommandInfo
from tests.framework.commands import NewFxCommands
from tests.framework.packets import NewFxPacket


class NewFxCommandHandler(CommandHandlerBase):
    """Handler for NewFX controller commands."""
    
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        return {
            'newfx.cmd1': (self.cmd_newfx_cmd1, CommandInfo(
                'newfx.cmd1', 'newfx.cmd1 <param1> <param2>',
                'Execute command 1', requires_init=True)),
            'newfx.cmd2': (self.cmd_newfx_cmd2, CommandInfo(
                'newfx.cmd2', 'newfx.cmd2 <id>',
                'Execute command 2', requires_init=True)),
        }
    
    def cmd_newfx_cmd1(self, args: List[str]):
        """NewFX command 1."""
        if len(args) < 2:
            self.print_error("Usage: newfx.cmd1 <param1> <param2>")
            return
        try:
            p1, p2 = int(args[0]), int(args[1])
            packet = NewFxCommands.command_1(p1, p2)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_success(f"Command 1 executed: p1={p1}, p2={p2}")
            else:
                self.print_response(response)
        except ValueError:
            self.print_error("Invalid parameters")

    def cmd_newfx_cmd2(self, args: List[str]):
        """NewFX command 2."""
        if len(args) < 1:
            self.print_error("Usage: newfx.cmd2 <id>")
            return
        try:
            id_val = int(args[0])
            packet = NewFxCommands.command_2(id_val)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_success(f"Command 2: id={id_val}")
            else:
                self.print_response(response)
        except ValueError:
            self.print_error("Invalid id")
```

### ACTION 10.2: Add ControllerType

**File:** `tests/cli/base.py`

```python
class ControllerType:
    GUNFX = 'gunfx'
    LIGHTFX = 'lightfx'
    NOOP = 'noop'
    NEWFX = 'newfx'    # ADD THIS
```

### ACTION 10.3: Register Handler in interactive.py

**File:** `tests/cli/interactive.py`

```python
from tests.cli.handlers.newfx import NewFxCommandHandler

# In constructor:
self.newfx_handler = NewFxCommandHandler(self.conn)

# In handler list (self._handlers):
self._handlers = [self.core_handler, self.gunfx_handler, 
                  self.lightfx_handler, self.newfx_handler]

# In get_available_commands():
if self.controller_type == ControllerType.NEWFX:
    commands.update(self.newfx_handler.get_commands())
```

### ACTION 10.4: Add Controller Detection

**File:** `tests/cli/handlers/core.py`

```python
# In INIT_READY parsing / controller detection:
name_lower = device_name.lower()
if 'newfx' in name_lower:
    self.controller_type = ControllerType.NEWFX
    self.print_info("Detected NewFX - newfx.* commands available")
```

---

## Step 11: Create Tests

**File:** `tests/newfx/test_system.py`

```python
"""NewFX system command tests."""
import pytest
from tests.framework import ScaleFXConnection, CommandBuilder

class TestNewFxInit:
    def test_init_response(self, connection):
        """Test INIT returns INIT_READY with NewFX info."""
        response = connection.send_and_wait(CommandBuilder.init())
        assert response.is_init_ready
        assert 'NewFX' in response.device_name

class TestNewFxCommands:
    def test_command_1_valid(self, connection):
        """Test COMMAND_1 with valid parameters."""
        from tests.framework import NewFxCommands
        connection.send_and_wait(CommandBuilder.init())
        
        packet = NewFxCommands.command_1(100, 5)
        success, response = connection.send_expect_ack(packet)
        assert success
    
    def test_command_1_missing_param(self, connection):
        """Test COMMAND_1 with missing parameters."""
        from tests.framework.protocol import build_packet
        from tests.framework.packets import NewFxPacket, CoreError
        connection.send_and_wait(CommandBuilder.init())
        
        # Send with empty payload
        packet = build_packet(NewFxPacket.COMMAND_1, b'')
        success, response = connection.send_expect_ack(packet)
        assert not success
        assert response.error_code == CoreError.MISSING_PARAMETER
```

---

## Step 12: Create README

**File:** `controllers/newfx/pico/README.md`

```markdown
# NewFX Pico Controller

[Description of what this controller does]

## Hardware

| Pin | Function |
|-----|----------|
| GP0 | ... |
| GP1 | ... |

## Protocol

### Packet Types (0x60-0x7F)

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x60 | COMMAND_1 | `[param1:u16][param2:u8]` | Description |
| 0x61 | COMMAND_2 | `[id:u8]` | Description |

### Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x30 | INVALID_PARAM_1 | param1 out of range |
| 0x31 | INVALID_PARAM_2 | param2 out of range |

## Build

\`\`\`bash
cd controllers/newfx/pico
pio run
\`\`\`

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 0.1.0 | YYYY-MM-DD | Initial release |
```

---

## Validation Checklist

```yaml
After_Completion:
  - [ ] "pio run" succeeds in controllers/newfx/pico/
  - [ ] "python -m py_compile tests/framework/packets.py" succeeds
  - [ ] "python -m py_compile tests/framework/commands.py" succeeds
  - [ ] "python -m py_compile tests/cli/interactive.py" succeeds
  - [ ] "python -m py_compile tests/cli/handlers/newfx.py" succeeds
  - [ ] CLI shows newfx.* commands after connecting to device
  - [ ] pytest tests/newfx/ passes with hardware
```
