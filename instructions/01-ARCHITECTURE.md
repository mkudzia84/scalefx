# System Architecture

> **REFERENCE DOCUMENT:** Read this to understand how the system works before making changes.

---

## System Topology

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              HubFX (Client)                              │
│                          RP2040 with USB Host                            │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐   │
│  │ GunFxClient      │  │ LightFxClient    │  │ GearControlClient    │   │
│  │ (extends         │  │ (extends         │  │ (extends             │   │
│  │  BusClient)      │  │  BusClient)      │  │  BusClient)          │   │
│  └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────────┘   │
└───────────┼─────────────────────┼─────────────────────┼─────────────────┘
            │ USB                 │ USB                 │ USB
            ▼                     ▼                     ▼
┌───────────────────┐  ┌───────────────────┐  ┌────────────────────────┐
│ GunFX Pico        │  │ LightFX Pico      │  │ GearControl Pico       │
│ (Server)          │  │ (Server)          │  │ (Server)               │
│ PicoServer +      │  │ PicoServer +      │  │ PicoServer +           │
│ GunFxServer       │  │ LightFxServer     │  │ GearControlServer      │
│ - Muzzle flash    │  │ - 8 LED channels  │  │ - 3 landing gears      │
│ - Smoke heater    │  │ - LED sequences   │  │ - INA226 current mon   │
│ - 3 servos        │  │ - 3 servos        │  │ - Battery monitor      │
│                   │  │ - Landing lights  │  │ - Yaw servo            │
└───────────────────┘  └───────────────────┘  └────────────────────────┘
```

---

## Packet Format

```yaml
Binary_Packet:
  structure: "[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]"
  fields:
    - name: "type"
      size: 1
      description: "Packet type identifier"
    - name: "tag"
      size: 1
      description: "Correlation tag (1-255 for request/response, 0 for async)"
    - name: "len"
      size: 2
      description: "Payload length, u16 little-endian (0-512, protocol max 65535)"
    - name: "payload"
      size: "0-512"
      description: "Command-specific data (MAX_PAYLOAD_SIZE = 512)"
    - name: "crc8"
      size: 1
      description: "CRC-8 over type+tag+len(2)+payload"

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
  NACK:        { type: 0xF7, direction: "S→C", payload: "[error_code:u8][reason_text...]" }
  REBOOT:      { type: 0xF8, direction: "C→S", payload: "none" }
  BOOTSEL:     { type: 0xF9, direction: "C→S", payload: "none" }
  ERROR:       { type: 0xFA, direction: "S→C", payload: "[error_code:u8][message...]" }
  I2C_SCAN:    { type: 0xFB, direction: "C→S", payload: "none" }
  I2C_SCAN_RESULT: { type: 0xFC, direction: "S→C", payload: "[numExp:u8][N×(addr,found,id)][numExtra:u8][M×addr]" }

GunFX_Packets:  # 0x01-0x2F
  TRIGGER_ON:      { type: 0x01, payload: "[rpm:u16LE]" }
  TRIGGER_OFF:     { type: 0x02, payload: "[fan_delay_ms:u16LE]" }
  SRV_SET:         { type: 0x10, payload: "[id:u8][pulse_us:u16LE]" }
  SRV_SETTINGS:    { type: 0x11, payload: "[id:u8][min:u16LE][max:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]" }
  SRV_RECOIL_JERK: { type: 0x12, payload: "[id:u8][jerk_us:u16LE][variance_us:u16LE]" }
  SMOKE_HEAT:      { type: 0x20, payload: "[on:u8]" }
  SMOKE_SETTINGS:  { type: 0x21, payload: "[pulsing:u8][speed:u8][high:u8][low:u8][pulse_ms:u16LE][spindown_ms:u16LE]" }

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
  SERVO_SET:         { type: 0x50, payload: "[id:u8][pulse:i16LE]" }
  SERVO_SETTINGS:    { type: 0x51, payload: "[id:u8][min:u16LE][max:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]" }
  LANDING_LIGHT_BIND:    { type: 0x52, payload: "[slot:u8][servoId:u8][ledCh:u8][deployUs:u16LE][retractUs:u16LE][brightness:u8]" }
  LANDING_LIGHT_UNBIND:  { type: 0x53, payload: "[slot:u8] (0=all)" }
  LANDING_LIGHT_DEPLOY:  { type: 0x54, payload: "[slot:u8] (0=all)" }
  LANDING_LIGHT_RETRACT: { type: 0x55, payload: "[slot:u8] (0=all)" }
  LED_MASTER_BRIGHTNESS: { type: 0x56, payload: "[pct:u8]" }
  POWER_STATUS:      { type: 0x58, payload: "none" }
  POWER_CONFIG:      { type: 0x59, payload: "[shunt_mohm:u16LE][max_current_ma:u16LE]" }

GearControl_Packets:  # 0x60-0x7F
  GEAR_DEPLOY:       { type: 0x60, payload: "[gear_id:u8]" }
  GEAR_RETRACT:      { type: 0x61, payload: "[gear_id:u8]" }
  GEAR_STOP:         { type: 0x62, payload: "[gear_id:u8]" }
  GEAR_ALL:          { type: 0x63, payload: "[action:u8] (0=retract,1=deploy,2=stop)" }
  SERVO_SET:         { type: 0x64, payload: "[id:u8][pulse_us:u16LE]" }
  SRV_SETTINGS:      { type: 0x65, payload: "[id:u8][min:u16LE][max:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]" }
  GEAR_CONFIG:       { type: 0x66, payload: "[gear_id:u8][flags:u8][stall_mA:u16LE][timeout_ms:u16LE]" }
  DOOR_CONFIG:       { type: 0x67, payload: "[gear_id:u8][open0:u16LE][close0:u16LE][open1:u16LE][close1:u16LE]" }
  YAW_CONFIG:        { type: 0x68, payload: "[gear_id:u8][neutral:u16LE][min:u16LE][max:u16LE]" }
  YAW_INPUT:         { type: 0x69, payload: "[position_us:u16LE]" }
  GEAR_CALIBRATE:    { type: 0x6A, payload: "[gear_id:u8][timeout_s:u8]" }
  GEAR_CALIB_STATUS: { type: 0x6B, payload: "[gear_id:u8][phase:u8][current:u16LE][peak:u16LE][stall:u16LE][finished:u8][errorReason:u8]" }
  GEAR_CALIB_CANCEL: { type: 0x6C, payload: "[gear_id:u8]" }
  BATTERY_CONFIG:    { type: 0x6D, payload: "[enabled:u8][auto_deploy:u8]" }
  DOOR_MODE:         { type: 0x6E, payload: "[gear_id:u8][mode:u8][delay_ms:u16LE]" }
```

---

## Class Hierarchy

### Server Side (Pico Controllers)

```
ICommandHandler (interface)
  │ tryProcess(type, payload, len) → CommandHandleResult
  │ handlerName() → const char*
  │
  └── BusServer (base class — ACK/NACK helpers, range routing)
        │ begin(Stream*) / end()
        │ sendAck() / sendNack() / sendError() / sendRawPacket()
        │
        ├── CoreCommandServer (0xF0-0xFF)
        │     INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE, STATUS_REQ, I2C_SCAN
        │     onStatusData(callback) — module status append
        │     updateFreeRam(n) — for STATUS response
        │
        ├── GunFxServer (0x01-0x2F)
        │     onTriggerOn(), onTriggerOff(), onServoSet(), onServoSettings()
        │     onSmokeHeat(), onSmokeSettings()
        │
        ├── LightFxServer (0x40-0x5F)
        │     onLedSet(), onLedOff(), onLedSeq*(), onServoSet(), onServoSettings()
        │     onLandingLight*(), onLedMasterBrightness()
        │
        └── GearControlServer (0x60-0x7F)
              onGearDeploy(), onGearRetract(), onGearStop(), onGearAll()
              onServoSet(), onServoSettings(), onGearConfig(), onDoorConfig()
              onYawConfig(), onYawInput(), onGearCalibrate(), onBatteryConfig()
              onDoorMode()
```

### Client Side (HubFX)

```
SerialBus (low-level COBS framing over USB CDC)
  │ begin(UsbHost*, port) / process()
  │ sendPacket(type, payload, len, tag)
  │
  └── BusClient (base — INIT handshake, tag queue, ACK/NACK handling)
        │ sendCommand(type, payload, len) → CommandResult
        │ sendInit() / onReady() / onError()
        │ resultQueue() → ResultQueue&
        │
        ├── GunFxClient
        │     triggerOn(rpm), triggerOff(delay), setServoPosition(), ...
        │     All methods return CommandResult
        │
        ├── LightFxClient
        │     ledSet(), ledOff(), ledSeqAdd(), servoSet(), requestStatus(), ...
        │     All methods return CommandResult
        │
        └── GearControlClient
              gearDeploy(), gearRetract(), gearAll(), servoSet(), requestStatus(), ...
              All methods return CommandResult
```

### Server Infrastructure (PicoServer)

```
PicoServer  (composes everything for Pico server controllers)
  │
  ├── CommandRouter (Chain of Responsibility dispatcher)
  │     addHandler(ICommandHandler*) — ordered priority
  │     poll() — read serial, COBS decode, CRC verify, route
  │
  ├── CoreCommandServer (first handler — always registered)
  │     INIT/SHUTDOWN/REBOOT/BOOTSEL/KEEPALIVE/STATUS
  │
  ├── IndicatorLedManager (GP13 connection, GP14 error)
  │     setErrorCondition() / setWarningCondition()
  │     update() — called automatically by server.loop()
  │
  └── Device name (e.g., "GunFX-A1B2" from Pico board ID)
```

### StatusDataCallback

Modules provide board-specific STATUS data via a callback registered on `CoreCommandServer`:

```cpp
using StatusDataCallback = std::function<size_t(uint8_t* buffer, size_t maxLen)>;

// STATUS response = 12-byte core header + module callback data
// Core header: [counter:u32LE][uptime:u32LE][freeRam:u32LE]

server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
    buf[0] = myFlag;
    CoreProtocol::putU16LE(&buf[1], myValue);
    return 3;  // bytes written
});
```

---

## Client Response Handling Design

### Tag Correlation (ResultQueue)

Every command sent by `BusClient::sendCommand()` is assigned a unique correlation tag (1-255, wrapping). The server echoes this tag in its response (ACK, NACK, or data response), allowing the client to match responses to requests even when multiple commands are in flight.

```
Client                                  Server
  │                                       │
  │  sendCommand(LED_SET, tag=0x07)       │
  │──────────────────────────────────────>│
  │                                       │ (processes command)
  │              ACK (tag=0x07)           │
  │<──────────────────────────────────────│
  │  ResultQueue resolves tag 0x07 → Ack  │
```

**Blocking mode** (default): `sendCommand()` spins calling `process()` until the matching tag arrives or timeout. Returns `CommandResult`.

**Non-blocking mode**: `sendCommand()` returns immediately with `CommandResult::Ack()` (optimistic). Use `ResultQueue::onTagResponse()` for async notification.

### Three Response Categories

Every command falls into one of three categories based on how the server responds:

#### 1. Direct ACK/NACK — Instant Commands

Server sends `ACK` (success) or `NACK(errorCode)` via `SFX_DISPATCH`. The operation completes before the ACK is sent. `BusClient::handlePacket()` resolves the tag automatically.

```cpp
// Server side (serial_xxxfx.h — handleModulePacket)
case XxxPacket::LED_SET: {
    SFX_REQUIRE_LEN(2);
    SFX_DISPATCH(_ledSetCallback, channel, brightness);  // ACK sent by macro
}

// Client side — tag resolved automatically by BusClient::handlePacket()
CommandResult result = client.ledSet(1, 128);  // Blocks for ACK
```

#### 2. Data Response — Query Commands

Server sends a typed response packet (e.g., `LED_STATUS_RESP`) instead of a plain `ACK`. The client's `onModulePacket()` override must treat this data response as an **implicit ACK** and resolve the tag manually.

```cpp
// Server side — sends data response using stored tag
case LightFxPacket::LED_SEQ_STATUS:
    _ledSeqStatusCallback(channel, status);
    sendSeqStatus(status);  // Sends LED_SEQ_STATUS_RESP with current tag
    return CommandHandleResult::Handled;  // No SFX_DISPATCH (handled manually)

// Client side — onModulePacket resolves tag as implicit ACK
void LightFxClient::onModulePacket(uint8_t type, uint8_t tag, ...) {
    case LightFxPacket::LED_SEQ_STATUS_RESP:
        // Parse response data, fire callback
        if (_seqStatusCallback) _seqStatusCallback(status);
        // Treat as implicit ACK — resolve tag for blocking callers
        if (tag != CoreProtocol::TAG_ASYNC) {
            _lastCommandResult = CommandResult::Ack();
            _resultQueue.resolve(tag, _lastCommandResult);
        }
        break;
}
```

**CRITICAL:** Without the `_resultQueue.resolve()` call, any blocking `sendCommand()` waiting for this tag would time out.

#### 3. Long-Running Commands — Deferred Completion

Server sends immediate `ACK` (command accepted) but the physical operation takes seconds. The client receives the ACK quickly, but completion is signaled later via STATUS polling or async data packets.

```cpp
// Server side — SFX_DISPATCH sends ACK immediately
case GearControlPacket::GEAR_DEPLOY: {
    SFX_REQUIRE_LEN(1);
    SFX_DISPATCH(_gearDeployCallback, gearId);  // ACK = "accepted"
    // Physical deploy takes 5-30 seconds...
}

// Client side — ACK means "started", poll STATUS for GearState::DEPLOYED
CommandResult result = client.gearDeploy(0);  // Returns quickly (ACK)
// Monitor via: client.onStatus(cb) or client.requestStatus()
```

**Special case — GEAR_CALIBRATE:** The server stores the calibrate request's tag and echoes it on every `GEAR_CALIB_STATUS` packet. When `finished==true`, the client resolves the tag, allowing blocking callers to wait for the full calibration to complete:

```cpp
// Server stores tag: _calibTag = _currentTag in handleModulePacket()
// Server echoes tag: sendCalibStatus() uses _calibTag
// Client resolves tag when calibration finishes:
if (cs.finished && tag != CoreProtocol::TAG_ASYNC) {
    _resultQueue.resolve(tag, isError ? Nack(...) : Ack());
}
```

### onModulePacket() Contract

Every `BusClient` subclass MUST override `onModulePacket()` and follow these rules:

1. **Parse the data** from the payload
2. **Fire any registered callbacks** with the parsed data
3. **Resolve the tag** via `_resultQueue.resolve(tag, result)` if `tag != TAG_ASYNC`
4. For ongoing operations (like calibration), resolve only on the **final** packet (`finished == true`)

```cpp
// Template for handling a data response packet:
case XxxPacket::RESPONSE_TYPE:
    if (len >= expectedLen) {
        // 1. Parse data
        // 2. Fire callback
        if (_myCallback) _myCallback(data);
    }
    // 3. Resolve tag (implicit ACK)
    if (tag != CoreProtocol::TAG_ASYNC) {
        _lastCommandResult = CommandResult::Ack();
        _resultQueue.resolve(tag, _lastCommandResult);
    }
    break;
```

---

## Operations Classification

Commands are classified by response timing. This affects client design and blocking behavior.

### Instant Commands (ACK = complete)

Server processes the command fully and returns ACK before the response. Safe to block.

```yaml
GunFX:
  - TRIGGER_ON           # Starts firing loop (immediate state change)
  - TRIGGER_OFF          # Stops firing
  - SRV_SET              # Sets servo target (profiling runs async, but command is accepted)
  - SRV_SETTINGS         # Configuration only
  - SRV_RECOIL_JERK      # Configuration only
  - SMOKE_HEAT           # Enable/disable heater (GPIO toggle)
  - SMOKE_SETTINGS       # Configuration only

LightFX:
  - LED_SET              # Set LED brightness (GPIO/PWM)
  - LED_OFF              # Turn off LED(s)
  - LED_MASTER_BRIGHTNESS # Set master brightness percentage
  - LED_SEQ_CLEAR        # Clear sequence queue
  - LED_SEQ_ADD          # Add event to queue
  - LED_SEQ_START        # Start sequence playback
  - LED_SEQ_STOP         # Stop sequence playback
  - LED_SEQ_RESTART      # Restart sequence from beginning
  - SERVO_SET            # Sets servo target (profiling runs async)
  - SERVO_SETTINGS       # Configuration only
  - LANDING_LIGHT_BIND   # Configuration only
  - LANDING_LIGHT_UNBIND # Configuration only

GearControl:
  - SERVO_SET            # Sets servo target
  - SRV_SETTINGS         # Configuration only
  - GEAR_CONFIG          # Configuration only
  - DOOR_CONFIG          # Configuration only
  - YAW_CONFIG           # Configuration only
  - BATTERY_CONFIG       # Configuration only
  - DOOR_MODE            # Configuration only
  - YAW_INPUT            # Direct servo mapping
  - GEAR_STOP            # Emergency stop (immediate motor cutoff)
  - GEAR_CALIB_CANCEL    # Cancels running calibration
```

### Query Commands (data response = implicit ACK)

Server responds with a typed data packet instead of plain ACK. Client resolves tag in `onModulePacket()`.

```yaml
Core:
  - STATUS_REQ → STATUS (0xF4)            # Core + module status data
  - I2C_SCAN → I2C_SCAN_RESULT (0xFC)     # Device discovery results

LightFX:
  - LED_SEQ_STATUS → LED_SEQ_STATUS_RESP (0x5A)   # Per-channel sequence info
  - LED_STATUS → LED_STATUS_RESP (0x5B)            # All channel status
  - LED_SEQ_QUEUE → LED_SEQ_QUEUE_RESP (0x5D)      # Queue contents

GearControl:
  - STATUS_REQ → STATUS (0xF4)             # Gear states, motor current, battery
```

### Long-Running Commands (ACK = accepted, monitor for completion)

Server sends immediate ACK but the physical operation takes seconds. Client uses STATUS polling or async callbacks to detect completion.

```yaml
LightFX:
  - LANDING_LIGHT_DEPLOY    # Servo motion 1-3 seconds
  - LANDING_LIGHT_RETRACT   # Servo motion 1-3 seconds
  # Completion: poll STATUS for servo position arriving at target

GearControl:
  - GEAR_DEPLOY             # Open doors → run motor → optionally close doors (5-30s)
  - GEAR_RETRACT            # Open doors → run motor → close doors (5-30s)
  - GEAR_ALL                # All 3 gears simultaneously (5-30s)
  # Completion: poll STATUS for GearState::DEPLOYED or GearState::RETRACTED

  - GEAR_CALIBRATE          # Multi-phase motor calibration (10-60s)
  # Completion: GEAR_CALIB_STATUS packets with finished=true resolve the tag
  # Server echoes the original request tag on all CALIB_STATUS packets
```

---

## Handler Registration Pattern

All Pico server controllers use `PicoServer` which handles handler registration order automatically:

```cpp
PicoServer server;
GunFxServer gunfxServer;

void setup() {
    server.begin("GunFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]()     { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });

    gunfxServer.begin(&Serial);
    gunfxServer.onTriggerOn([](uint16_t rpm) -> uint8_t { ... });

    server.core().onStatusData([](uint8_t* buf, size_t max) -> size_t { ... });
    server.addModuleHandler(&gunfxServer);  // Core auto-registered first
}

void loop() {
    server.loop();       // Protocol, timeout, indicators
    updateHardware();    // Module-specific
    delay(1);
}
```

For core-only controllers (no module commands):
```cpp
server.addModuleHandler(nullptr);  // Core protocol only (e.g., NoOp)
```

---

## Indicator LED Standard

All Pico server controllers use identical indicator LED behavior on GP13/GP14, managed automatically by PicoServer via `IndicatorLedManager`.

```yaml
LED_0_Connection:
  pin: GP13
  waiting_for_init: "Blink every 500ms"
  connected: "Solid ON"
  connection_lost: "OFF"

LED_1_Error:
  pin: GP14
  normal: "OFF"
  error: "Blink every 200ms (setErrorCondition)"
  warning: "Blink slow (setWarningCondition)"
```

### State Transitions

```
Power On → initialized=false, watchdog=false → LED 0 blinks, LED 1 off
   │
   ▼ INIT received
Connected → initialized=true, watchdog=false → LED 0 solid, LED 1 off
   │
   ▼ Keepalive timeout (15s)
Lost → initialized=false, watchdog=true → LED 0 off, LED 1 off
   │
   ▼ INIT received again
Connected → (cycle repeats)
```

---

## Server Handler Macros (SFX_*)

Server `handleModulePacket()` switch cases use macros from `serial_core.h`:

```cpp
SFX_REQUIRE_LEN(n)                    // NACK MISSING_PARAMETER if len < n
SFX_VALIDATE(cond, err)               // NACK err if !cond
SFX_DISPATCH(callback, args...)       // Call callback, ACK/NACK on result
SFX_HANDLE_CHANNEL_CMD(v, err, cb)    // Validate + dispatch single-param cmd

// Example:
case GunFxPacket::SRV_SET: {
    SFX_REQUIRE_LEN(3);
    uint8_t id = payload[0];
    uint16_t pulse = CoreProtocol::getU16LE(payload + 1);
    SFX_VALIDATE(GunFxSpec::isValidServoId(id), GunFxError::SERVO_INVALID_ID);
    SFX_VALIDATE(GunFxSpec::isValidServoPulse(pulse), GunFxError::SERVO_PULSE_RANGE);
    SFX_DISPATCH(_onServoSet, id, pulse);
}
```

### Validation Namespaces

Each module defines a `Spec` namespace with constants and inline validators:
- **GunFxSpec** — servo IDs 1-3, pulse 500-2500µs, RPM 1-3000
- **LightFxSpec** — LED channels 1-8, servo IDs 1-3, sequence limits
- **GearControlSpec** — gear IDs 0-2, servo IDs 0-7, stall current, door modes

---

## Data Flow (Server)

```
                    USB Serial
                        │
                        ▼
┌───────────────────────────────────────────────────────────────┐
│                  CommandRouter.poll()                          │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ 1. Read bytes until 0x00 delimiter                      │  │
│  │ 2. COBS decode                                          │  │
│  │ 3. Verify CRC-8                                         │  │
│  │ 4. Extract type, tag, payload, len                      │  │
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
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ BusServer.tryProcess():                                  │  │
│  │   1. Check if type is in moduleRangeLow..moduleRangeHigh │  │
│  │   2. If yes: delegate to handleModulePacket()            │  │
│  │   3. If no:  return NotMyCommand                         │  │
│  └─────────────────────────────────────────────────────────┘  │
│                         │                                      │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ handleModulePacket():                                    │  │
│  │   - Parse payload with SFX_REQUIRE_LEN                   │  │
│  │   - Validate with SFX_VALIDATE                           │  │
│  │   - Dispatch via SFX_DISPATCH → callback → ACK/NACK     │  │
│  └─────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

---

## Serial Library File Structure

```yaml
Serial_Library:
  root: "controllers/lib/serial/"
  files:
    serial.h:             "Umbrella header — include this for everything"
    serial_core.h:        "CoreProtocol (COBS/CRC/endian), SerialError, CommandResult,
                           ICommandHandler, CommandRouter, SFX_* macros, callback typedefs"
    serial_core.cpp:      "CoreProtocol implementations, CorePayload encode/decode"
    serial_bus_server.h:  "BusServer base class + CoreCommandServer"
    serial_bus_server.cpp: "BusServer + CoreCommandServer implementations"
    serial_bus_client.h:  "BusClient base class (extends SerialBus)"
    serial_bus_client.cpp: "BusClient implementation"
    serial_bus.h:         "SerialBus (client-only, COBS over USB CDC)"
    serial_bus.cpp:       "SerialBus implementation (guarded by #ifndef SCALEFX_SERVER)"
    serial_usb_host.h:    "UsbHost (client-only, PIO-USB manager)"
    serial_result_queue.h: "ResultQueue — tag-correlated command/response matching"
    serial_result_queue.cpp: "ResultQueue implementation"
    serial_gunfx.h:       "GunFxServer + GunFxClient + GunFxPacket + GunFxError + GunFxSpec"
    serial_gunfx.cpp:     "GunFxClient implementation"
    serial_lightfx.h:     "LightFxServer + LightFxClient + LightFxPacket + LightFxError"
    serial_lightfx.cpp:   "LightFxClient implementation"
    serial_gearcontrol.h: "GearControlServer + GearControlClient + GearControlPacket + GearControlError"
    serial_gearcontrol.cpp: "GearControlClient implementation"
```

---

## Error Code Ranges

```yaml
Error_Ranges:
  - range: "0x00"
    name: "OK"

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
    defined_in: "serial_gunfx.h"

  - range: "0x50-0x5F"
    namespace: "LightFxError"
    defined_in: "serial_lightfx.h"

  - range: "0x60-0x6F"
    namespace: "GearControlError"
    defined_in: "serial_gearcontrol.h"

  - range: "0x70-0x8F"
    namespace: "Reserved"
    description: "Future controller modules"

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
    purpose: "Build core command packets (init, shutdown, status_req, etc.)"

  GunFxCommands:
    file: "tests/framework/commands.py"
    purpose: "Build GunFX-specific packets"

  LightFxCommands:
    file: "tests/framework/commands.py"
    purpose: "Build LightFX-specific packets"

  GearControlCommands:
    file: "tests/framework/commands.py"
    purpose: "Build GearControl-specific packets"
```

---

## Key Implementation Rules

```yaml
DO:
  - Use little-endian for ALL multi-byte values
  - Use CoreProtocol::getU16LE() / putU16LE() for endian-safe reads/writes
  - Return 0 (SerialError::OK) from callbacks on success
  - Return specific error codes on failure
  - Use PicoServer for all Pico server controllers
  - Extend BusServer for module server handlers
  - Extend BusClient for module client controllers
  - Use SFX_* macros in handleModulePacket() for ACK/NACK handling
  - Include unit suffixes on all physical measurements (_mV, _mA, _us, _ms)
  - Put packet types, error codes, and validation in serial_xxxfx.h (single file per module)

DONT:
  - Don't use blocking delays in callbacks (>10ms)
  - Don't allocate memory in interrupt context
  - Don't assume packet payload is null-terminated
  - Don't hardcode magic numbers (use constants in Spec namespace)
  - Don't create separate serial_error.h files — errors belong in serial_xxxfx.h
  - Don't bypass PicoServer for server controllers
```
