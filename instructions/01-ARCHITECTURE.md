# System Architecture

> **REFERENCE DOCUMENT:** Read this to understand how the system works before making changes.

---

## System Topology

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              HubFX (Client)                              │
│                          RP2040 with USB Host                            │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐   │
│  │ GunFxClient      │  │ LightFxClient    │  │ Other Clients...     │   │
│  │ (USB Port 0)     │  │ (USB Port 1)     │  │ (USB Port N)         │   │
│  └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────────┘   │
└───────────┼─────────────────────┼─────────────────────┼─────────────────┘
            │ USB                 │ USB                 │ USB
            ▼                     ▼                     ▼
┌───────────────────┐  ┌───────────────────┐  ┌────────────────────────┐
│ GunFX Pico        │  │ LightFX Pico      │  │ Other Servers...       │
│ (Server)          │  │ (Server)          │  │                        │
│ - Muzzle flash    │  │ - 8 LED channels  │  │                        │
│ - Smoke heater    │  │ - LED sequences   │  │                        │
│ - 3 servos        │  │ - 3 servos        │  │                        │
│                   │  │ - Power monitor   │  │                        │
└───────────────────┘  └───────────────────┘  └────────────────────────┘
```

---

## Packet Format

```yaml
Binary_Packet:
  structure: "[type:u8][len:u8][payload:0-64][crc8:u8]"
  fields:
    - name: "type"
      size: 1
      description: "Packet type identifier"
    - name: "len"
      size: 1
      description: "Payload length (0-64)"
    - name: "payload"
      size: "0-64"
      description: "Command-specific data"
    - name: "crc8"
      size: 1
      description: "CRC-8 over type+len+payload"

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
Core_Packets:  # 0xF0-0xFF - All controllers
  INIT:        { type: 0xF0, direction: "C→S", payload: "none" }
  SHUTDOWN:    { type: 0xF1, direction: "C→S", payload: "none" }
  KEEPALIVE:   { type: 0xF2, direction: "C→S", payload: "none" }
  INIT_READY:  { type: 0xF3, direction: "S→C", payload: "[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]" }
  STATUS:      { type: 0xF4, direction: "S→C", payload: "[counter:u32LE][uptime:u32LE][freeRam:u32LE][moduleData...]" }
  STATUS_REQ:  { type: 0xF5, direction: "C→S", payload: "none" }
  ACK:         { type: 0xF6, direction: "S→C", payload: "none" }
  NACK:        { type: 0xF7, direction: "S→C", payload: "[error_code:u8]" }
  REBOOT:      { type: 0xF8, direction: "C→S", payload: "none" }
  BOOTSEL:     { type: 0xF9, direction: "C→S", payload: "none" }

GunFX_Packets:  # 0x01-0x2F
  TRIGGER_ON:      { type: 0x01, payload: "[rpm:u16]" }
  TRIGGER_OFF:     { type: 0x02, payload: "[fan_delay_ms:u16]" }
  SRV_SET:         { type: 0x10, payload: "[id:u8][pulse_us:u16]" }
  SRV_SETTINGS:    { type: 0x11, payload: "[id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]" }
  SRV_RECOIL_JERK: { type: 0x12, payload: "[id:u8][jerk_us:u16][variance_us:u16]" }
  SMOKE_HEAT:      { type: 0x20, payload: "[on:u8]" }
  SMOKE_SETTINGS:  { type: 0x21, payload: "[pulsing:u8][speed:u8][high:u8][low:u8][pulse_ms:u16][spindown_ms:u16]" }

LightFX_Packets:  # 0x40-0x5F
  LED_SET:           { type: 0x40, payload: "[ch:u8][brightness:u8]" }
  LED_OFF:           { type: 0x41, payload: "[ch:u8] (0=all)" }
  LED_SEQ_CLEAR:     { type: 0x42, payload: "[ch:u8]" }
  LED_SEQ_ADD:       { type: 0x43, payload: "[ch:u8][event_type:u8][params...]" }
  LED_SEQ_START:     { type: 0x44, payload: "[ch:u8]" }
  LED_SEQ_STOP:      { type: 0x45, payload: "[ch:u8]" }
  LED_SEQ_RESTART:   { type: 0x46, payload: "[ch:u8]" }
  LED_SEQ_STATUS:    { type: 0x47, payload: "[ch:u8]" }
  LED_STATUS:        { type: 0x48, payload: "none" }
  LED_SEQ_QUEUE:     { type: 0x49, payload: "[ch:u8]" }
  SERVO_SET:         { type: 0x50, payload: "[id:u8][pulse:i16]" }
  SERVO_SETTINGS:    { type: 0x51, payload: "[id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]" }
  POWER_STATUS:      { type: 0x58, payload: "none" }
  POWER_CONFIG:      { type: 0x59, payload: "[shunt_mohm:u16][max_current_ma:u16]" }
```

---

## Class Hierarchy

```
┌─────────────────────────────────────────────────────────────────┐
│                        serial.h (umbrella)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │ CoreProtocol (static class)                               │  │
│  │ - cobs_encode(data) → encoded                             │  │
│  │ - cobs_decode(encoded) → data                             │  │
│  │ - crc8(data, len) → checksum                              │  │
│  │ - build_packet(type, payload, len) → packet               │  │
│  │ - parse_packet(data) → {type, payload, len, valid}        │  │
│  │ - encodeInitReady(info) / decodeInitReady(payload)        │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │ ICommandHandler (interface)                               │  │
│  │ - tryProcess(type, payload, len) → CommandHandleResult    │  │
│  │ - handlerName() → const char*                             │  │
│  └───────────────────────────────────────────────────────────┘  │
│            ▲                      ▲                             │
│            │                      │                             │
│  ┌─────────┴─────────┐  ┌────────┴────────┐                    │
│  │ CoreCommandServer │  │ XxxFxServer     │                    │
│  │ handles: 0xF0-0xFF│  │ handles: module │                    │
│  │ onStatusData(cb)  │  └─────────────────┘                    │
│  │ updateFreeRam(n)  │                                        │
│  └───────────────────┘                                        │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │ CommandRouter                                             │  │
│  │ - addHandler(ICommandHandler*)                            │  │
│  │ - poll() → reads serial, routes to handlers in order      │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### StatusDataCallback

Modules provide board-specific STATUS data via a callback registered on `CoreCommandServer`.
When a STATUS_REQ is received, `CoreCommandServer` writes the 12-byte core header
(`[counter:u32LE][uptime:u32LE][freeRam:u32LE]`) and then calls the module's callback
to append module-specific bytes.

```cpp
using StatusDataCallback = std::function<size_t(uint8_t* buffer, size_t maxLen)>;
// Returns number of bytes written to buffer
```

---

## Handler Registration Pattern

> **CRITICAL:** Every server firmware MUST register `coreServer` before the module handler. Without `coreServer`, core commands (INIT, REBOOT, BOOTSEL, STATUS, etc.) will be rejected with `INVALID_COMMAND`.

```cpp
// STANDARD PATTERN: Every server controller follows this

// 1. Declare handlers (global or static)
CommandRouter commandRouter;
CoreCommandServer coreServer;
XxxFxServer xxxfxServer;

void setup() {
    Serial.begin(115200);
    
    // 2. Configure core handler
    coreServer.begin(&Serial);
    coreServer.setBoardInfo("DeviceName", FIRMWARE_VERSION, "RP2040",
                            rp2040.f_cpu() / 1000000, rp2040.getFreeHeap(),
                            BUILD_NUMBER);
    coreServer.onInit([]() { performSafeInit(); });
    coreServer.onShutdown([]() { performSafeShutdown(); });
    coreServer.onReboot([]() { rp2040.reboot(); });
    coreServer.onBootsel([]() { rp2040.rebootToBootloader(); });
    
    // 2b. Register module-specific STATUS data callback
    coreServer.onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        // Write module status bytes (appended to 12-byte core header)
        return writeModuleStatus(buf, maxLen);
    });
    
    // 3. Configure module handler with callbacks
    xxxfxServer.begin(&Serial);
    xxxfxServer.onCommand([](params) -> uint8_t {
        // Return 0 for success, error code for failure
        return performCommand(params) ? 0 : ERROR_CODE;
    });
    
    // 4. Register handlers with router (order = priority)
    //    CRITICAL: coreServer MUST be registered first!
    //    Without it, INIT/REBOOT/BOOTSEL will NACK with INVALID_COMMAND.
    commandRouter.begin(&Serial, [](uint8_t err, uint8_t type) {
        xxxfxServer.sendNack(err);  // Global error handler
    });
    commandRouter.addHandler(&coreServer);   // Priority 1: core commands
    commandRouter.addHandler(&xxxfxServer);   // Priority 2: module commands
}

void loop() {
    // 5. Process incoming packets
    commandRouter.poll();
    
    // 6. Keep free RAM reading current for STATUS responses
    coreServer.updateFreeRam(rp2040.getFreeHeap());
}
```

---

## Server Handler Macros (SFX_*)

Server `tryProcess()` switch cases use macros from `serial_core.h` to reduce boilerplate:

```cpp
// Macros available in handler switch cases:
SFX_REQUIRE_LEN(n)                    // NACK MISSING_PARAMETER if len < n
SFX_VALIDATE(cond, err)               // NACK err if !cond
SFX_DISPATCH(callback, args...)       // Call callback, ACK/NACK on result
SFX_HANDLE_CHANNEL_CMD(v, err, cb)    // Validate + dispatch single-param cmd

// Example: handling a servo set command
case GunFxPacket::SRV_SET: {
    SFX_REQUIRE_LEN(3);
    uint8_t id = payload[0];
    uint16_t pulse = getU16LE(payload + 1);
    SFX_VALIDATE(GunFxSpec::isValidServoId(id), GunFxError::SERVO_INVALID_ID);
    SFX_VALIDATE(GunFxSpec::isValidServoPulse(pulse), GunFxError::SERVO_PULSE_RANGE);
    SFX_DISPATCH(_onServoSet, id, pulse);
}
```

### Validation Namespaces

Each module defines a `Spec` namespace with constants and inline validators:
- **GunFxSpec** — servo IDs 1-3, pulse 500-2500µs, RPM 1-3000
- **LightFxSpec** — LED channels 1-8, servo IDs 1-3, INA226 limits, sequence limits

---

## Data Flow

```
                    USB Serial
                        │
                        ▼
┌───────────────────────────────────────────────────────────────┐
│                    CommandRouter.poll()                        │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ 1. Read bytes until 0x00 delimiter                      │  │
│  │ 2. COBS decode                                          │  │
│  │ 3. Verify CRC-8                                         │  │
│  │ 4. Extract type, payload, len                           │  │
│  └─────────────────────────────────────────────────────────┘  │
│                         │                                      │
│                         ▼                                      │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ For each handler in priority order:                      │  │
│  │   result = handler.tryProcess(type, payload, len)        │  │
│  │   if result == Handled: break                            │  │
│  │   if result == NotMyCommand: try next handler            │  │
│  └─────────────────────────────────────────────────────────┘  │
│                         │                                      │
│                         ▼                                      │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ Handler:                                                 │  │
│  │   - Parse payload                                        │  │
│  │   - Call registered callback                             │  │
│  │   - Send ACK (success) or NACK (error)                   │  │
│  └─────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

---

## Error Code Ranges

```yaml
Error_Ranges:
  - range: "0x00"
    name: "OK"
    description: "Success"
  
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
    namespace: "GunFxError"
    errors:
      - { code: 0x20, name: "SERVO_INVALID_ID", desc: "Servo ID out of range (1-3)" }
      - { code: 0x21, name: "SERVO_PULSE_RANGE", desc: "Pulse width outside 500-2500µs" }
      - { code: 0x22, name: "SERVO_MIN_MAX", desc: "minUs >= maxUs" }
      - { code: 0x23, name: "SERVO_NOT_CONFIGURED" }
      - { code: 0x30, name: "INVALID_FAN_SPEED" }
      - { code: 0x40, name: "INVALID_RPM", desc: "RPM out of range (1-3000)" }
      - { code: 0x41, name: "ALREADY_FIRING" }
      - { code: 0x42, name: "NOT_FIRING" }
  
  - range: "0x50-0x5F"
    namespace: "LightFxError"
    errors:
      - { code: 0x50, name: "INVALID_CHANNEL" }
      - { code: 0x51, name: "SEQ_FULL", desc: "Sequence buffer full" }
      - { code: 0x52, name: "INVALID_EVENT", desc: "Invalid event type" }
      - { code: 0x53, name: "INVALID_PARAM" }
      - { code: 0x54, name: "INVALID_SERVO" }
  
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

---

## Python Framework Architecture

```yaml
Framework_Classes:
  ScaleFXConnection:
    file: "tests/framework/connection.py"
    purpose: "Manages serial connection and packet I/O"
    methods:
      - "connect(port, init=True) → bool"
      - "send(packet) → None"
      - "send_and_wait(packet, timeout) → Response"
      - "send_expect_ack(packet) → (success, Response)"
      - "close() → None"
  
  CoreProtocol:
    file: "tests/framework/protocol.py"
    purpose: "Packet encoding/decoding"
    methods:
      - "build_packet(type, payload) → bytes"
      - "parse_packet(data) → ParsedPacket"
      - "cobs_encode(data) → bytes"
      - "cobs_decode(data) → bytes"
      - "crc8(data) → int"
  
  CommandBuilder:
    file: "tests/framework/commands.py"
    purpose: "Build command packets"
    static_methods:
      - "init() → bytes"
      - "shutdown() → bytes"
      - "status_req() → bytes"
      - etc.
  
  GunFxCommands:
    file: "tests/framework/commands.py"
    purpose: "Build GunFX-specific packets"
  
  LightFxCommands:
    file: "tests/framework/commands.py"
    purpose: "Build LightFX-specific packets"
```

---

## Key Implementation Rules

```yaml
DO:
  - Use little-endian for multi-byte values
  - Return 0 from callbacks on success
  - Return specific error codes on failure
  - Register CoreCommandServer before module handlers
  - Use std::function for callbacks (allows lambdas)
  - Use SFX_* macros in tryProcess() for ACK/NACK handling

DONT:
  - Don't modify ICommandHandler interface
  - Don't use blocking delays in callbacks (>10ms)
  - Don't allocate memory in interrupt context
  - Don't assume packet payload is null-terminated
  - Don't hardcode magic numbers (use constants)
```
