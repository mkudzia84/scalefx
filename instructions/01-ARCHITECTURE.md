# System Architecture

> **REFERENCE DOCUMENT:** Read this to understand how the system works before making changes.

---

## System Topology

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              HubFX (Master)                              │
│                          RP2040 with USB Host                            │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐   │
│  │ GunFxMaster      │  │ LightFxMaster    │  │ Other Masters...     │   │
│  │ (USB Port 0)     │  │ (USB Port 1)     │  │ (USB Port N)         │   │
│  └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────────┘   │
└───────────┼─────────────────────┼─────────────────────┼─────────────────┘
            │ USB                 │ USB                 │ USB
            ▼                     ▼                     ▼
┌───────────────────┐  ┌───────────────────┐  ┌────────────────────────┐
│ GunFX Pico        │  │ LightFX Pico      │  │ Other Slaves...        │
│ (Slave)           │  │ (Slave)           │  │                        │
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
  INIT:        { type: 0xF0, direction: "M→S", payload: "none" }
  SHUTDOWN:    { type: 0xF1, direction: "M→S", payload: "none" }
  KEEPALIVE:   { type: 0xF2, direction: "M→S", payload: "none" }
  INIT_READY:  { type: 0xF3, direction: "S→M", payload: "device_info" }
  STATUS:      { type: 0xF4, direction: "S→M", payload: "telemetry" }
  STATUS_REQ:  { type: 0xF5, direction: "M→S", payload: "none" }
  ACK:         { type: 0xF6, direction: "S→M", payload: "none" }
  NACK:        { type: 0xF7, direction: "S→M", payload: "[error_code:u8]" }
  REBOOT:      { type: 0xF8, direction: "M→S", payload: "none" }
  BOOTSEL:     { type: 0xF9, direction: "M→S", payload: "none" }

GunFX_Packets:  # 0x01-0x2F
  TRIGGER_ON:     { type: 0x01, payload: "[rpm:u16]" }
  TRIGGER_OFF:    { type: 0x02, payload: "none" }
  SRV_SET:        { type: 0x10, payload: "[axis:u8][pos:u16]" }
  SRV_SETTINGS:   { type: 0x11, payload: "[axis:u8][min:u16][max:u16][speed:u8]" }
  SRV_RECOIL_JERK:{ type: 0x12, payload: "[count:u8][delay_ms:u16]" }
  SMOKE_HEAT:     { type: 0x20, payload: "[enable:u8]" }
  SMOKE_SETTINGS: { type: 0x21, payload: "[temp:u16][fan:u8]" }

LightFX_Packets:  # 0x40-0x5F
  LED_SET:        { type: 0x40, payload: "[channel:u8][brightness:u8]" }
  LED_ALL:        { type: 0x41, payload: "[brightness:u8]" }
  LED_FADE:       { type: 0x42, payload: "[channel:u8][target:u8][duration_ms:u16]" }
  LED_SEQ_START:  { type: 0x43, payload: "[channel:u8][seq_id:u8]" }
  LED_SEQ_STOP:   { type: 0x44, payload: "[channel:u8]" }
  SRV_SET:        { type: 0x50, payload: "[axis:u8][pos:u16]" }
  SRV_SETTINGS:   { type: 0x51, payload: "[axis:u8][min:u16][max:u16][speed:u8]" }
  PWR_READ:       { type: 0x55, payload: "none" }
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
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │ ICommandHandler (interface)                               │  │
│  │ - canHandle(type) → bool                                  │  │
│  │ - handle(type, payload, len) → bool                       │  │
│  └───────────────────────────────────────────────────────────┘  │
│            ▲                      ▲                             │
│            │                      │                             │
│  ┌─────────┴─────────┐  ┌────────┴────────┐                    │
│  │ CoreCommandHandler│  │ XxxFxSlave      │                    │
│  │ handles: 0xF0-0xFF│  │ handles: module │                    │
│  └───────────────────┘  └─────────────────┘                    │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │ CommandRouter                                             │  │
│  │ - addHandler(ICommandHandler*, priority)                  │  │
│  │ - route(packet) → routes to first handler that canHandle  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Handler Registration Pattern

```cpp
// STANDARD PATTERN: Every slave controller follows this

// 1. Declare handlers (global or static)
CommandRouter commandRouter;
CoreCommandHandler coreHandler;
XxxFxSlave xxxfxSlave;

void setup() {
    Serial.begin(115200);
    
    // 2. Configure core handler
    coreHandler.begin(&Serial);
    coreHandler.setBoardInfo("DeviceName", "1.0.0", "RP2040", 125, freeRam);
    coreHandler.onInit([]() { performSafeInit(); });
    coreHandler.onShutdown([]() { performSafeShutdown(); });
    coreHandler.onReboot([]() { rp2040.reboot(); });
    coreHandler.onBootsel([]() { rp2040.rebootToBootloader(); });
    
    // 3. Configure module handler with callbacks
    xxxfxSlave.begin(&Serial);
    xxxfxSlave.onCommand([](params) -> uint8_t {
        // Return 0 for success, error code for failure
        return performCommand(params) ? 0 : ERROR_CODE;
    });
    
    // 4. Register handlers with router (order = priority)
    commandRouter.begin(&Serial, [](uint8_t err, uint8_t type) {
        xxxfxSlave.sendNack(err);  // Global error handler
    });
    commandRouter.addHandler(&coreHandler);   // Priority 1: core commands
    commandRouter.addHandler(&xxxfxSlave);    // Priority 2: module commands
}

void loop() {
    // 5. Process incoming packets
    commandRouter.poll();
}
```

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
│  │   if handler.canHandle(type):                            │  │
│  │     handler.handle(type, payload, len)                   │  │
│  │     break                                                │  │
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
    namespace: "SerialError"
    errors:
      - { code: 0x01, name: "INVALID_CRC" }
      - { code: 0x02, name: "INVALID_PACKET" }
      - { code: 0x03, name: "INVALID_COMMAND" }
      - { code: 0x04, name: "INVALID_LENGTH" }
      - { code: 0x05, name: "MISSING_PARAMETER" }
      - { code: 0x06, name: "INVALID_PARAMETER" }
      - { code: 0x07, name: "BUFFER_OVERFLOW" }
      - { code: 0x08, name: "TIMEOUT" }
  
  - range: "0x10-0x1F"
    namespace: "GunFxError"
    errors:
      - { code: 0x10, name: "SERVO_INVALID_AXIS" }
      - { code: 0x11, name: "SERVO_INVALID_POSITION" }
      - { code: 0x12, name: "SMOKE_OVERHEAT" }
  
  - range: "0x20-0x2F"
    namespace: "LightFxError"
    errors:
      - { code: 0x20, name: "LED_INVALID_CHANNEL" }
      - { code: 0x21, name: "LED_INVALID_SEQUENCE" }
      - { code: 0x22, name: "SERVO_INVALID_AXIS" }
      - { code: 0x23, name: "POWER_MONITOR_ERROR" }
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
  - Register CoreCommandHandler before module handlers
  - Use std::function for callbacks (allows lambdas)
  - Call sendAck() or sendNack() after handling

DONT:
  - Don't modify ICommandHandler interface
  - Don't use blocking delays in callbacks (>10ms)
  - Don't allocate memory in interrupt context
  - Don't assume packet payload is null-terminated
  - Don't hardcode magic numbers (use constants)
```
