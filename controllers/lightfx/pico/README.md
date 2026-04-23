# LightFX Controller - Raspberry Pi Pico

Lighting effects controller for scale models - manages 8 LED channels with sequence animations and 3 servos.

**Version:** 0.15.0  
**Protocol:** Binary COBS with CRC-8  
**Baud Rate:** 1000000 (1Mbps)

---

## Hardware

- **MCU**: Raspberry Pi Pico (RP2040)
- **Core**: earlephilhower/arduino-pico

### Pin Assignments

#### LED Output Channels (PWM)

| Channel | GPIO | Description |
|---------|------|-------------|
| CH1     | 0    | LED Channel 1 |
| CH2     | 1    | LED Channel 2 |
| CH3     | 2    | LED Channel 3 |
| CH4     | 3    | LED Channel 4 |
| CH5     | 4    | LED Channel 5 |
| CH6     | 5    | LED Channel 6 |
| CH7     | 6    | LED Channel 7 |
| CH8     | 7    | LED Channel 8 |

#### Status LEDs

| LED    | GPIO | Function |
|--------|------|----------|
| Blue   | 24   | Connection status - slow blink when disconnected |
| Yellow | 25   | Error/warning indicator |

#### Battery Sensing

| Pin  | GPIO | Function |
|------|------|----------|
| VSYS | 29   | Battery voltage ADC (÷6.1 resistor divider, 51k/10k) |

#### Servos

| Servo   | GPIO | Description |
|---------|------|-------------|
| Servo 1 | 8    | General purpose servo — doubles as the RC PWM **"light input"** pin in STANDALONE mode. The firmware reads its pulse width and matches against input bands to auto-switch the active program. In SLAVE / DIRECT mode, GP8 reverts to a normal servo output. |
| Servo 2 | 9    | General purpose servo |
| Servo 3 | 10   | General purpose servo |

---

## Protocol

### Packet Format

Binary COBS-encoded packets terminated by 0x00 delimiter:

```
[type:u8][len:u16LE][payload:0-512 bytes][crc8:u8]
```

CRC-8 polynomial 0x07 computed over type + len + payload.

### Packet Type Ranges

| Range | Module | Description |
|-------|--------|-------------|
| 0x40-0x5F | LightFX | Controller-specific commands |
| 0xEF-0xFF | Core | Universal system commands |

---

## System Commands (0xEF-0xFF)

| Type | Name | Payload | Response | Description |
|------|------|---------|----------|-------------|
| 0xF0 | INIT | mode:u8, flags:u8 (optional) | INIT_READY | Initialize connection |

**INIT Modes:** SLAVE (0x00) = keep-alive required (default), DIRECT (0x01) = no keep-alive timeout  
**INIT Flags:** bit 0 = VERBOSE (enable async STATUS_UPDATE emissions)  
LightFX accepts both SLAVE and DIRECT modes with identical behavior.

| 0xF1 | SHUTDOWN | (none) | ACK | Safe shutdown, outputs off |
| 0xF2 | KEEPALIVE | (none) | ACK | Reset watchdog timer |
| 0xF3 | INIT_READY | (response) | — | Device info response |
| 0xF5 | STATUS_REQ | (none) | STATUS | Request status |
| 0xF6 | ACK | (none) | — | Command success |
| 0xF7 | NACK | code:u8, reason:str | — | Command failure |
| 0xF8 | REBOOT | (none) | — | Reboot (fire-and-forget) |
| 0xF9 | BOOTSEL | (none) | — | Enter bootloader |

---

## LightFX Commands (0x40-0x5F)

### LED Direct Control

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x40 | LED_SET | ch:u8, brightness:u8 | Set LED channel brightness (0-100%) |
| 0x41 | LED_OFF | ch:u8 (0=all) | Turn off LED(s) |
| 0x4A | LED_MASTER_BRIGHTNESS | pct:u8 (0-100) | Set master brightness for all LEDs |

### LED Sequences

Each LED channel has its own sequence that can hold up to 24 events.

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x42 | LED_SEQ_CLEAR | ch:u8 | Clear sequence for channel |
| 0x43 | LED_SEQ_ADD | ch:u8, event... | Add event to sequence |
| 0x44 | LED_SEQ_START | ch:u8 (0=all) | Start sequence playback |
| 0x45 | LED_SEQ_STOP | ch:u8 (0=all) | Stop sequence playback |
| 0x46 | LED_SEQ_RESTART | ch:u8 | Restart sequence from beginning |
| 0x47 | LED_SEQ_STATUS | ch:u8 | Query sequence status → LED_SEQ_STATUS_RESP |
| 0x48 | LED_STATUS | (none) | Query all LED channels → LED_STATUS_RESP |
| 0x49 | LED_SEQ_QUEUE | ch:u8 | Query sequence event queue → LED_SEQ_QUEUE_RESP |

#### Response Packets

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x5A | LED_STATUS_RESP | [ch, brightness, playing, count]×N | Status of all channels |
| 0x5B | LED_SEQ_STATUS_RESP | ch:u8, playing:u8, count:u8, index:u8, loops:u32le | Sequence status |
| 0x5D | LED_SEQ_QUEUE_RESP | ch:u8, count:u8, index:u8, playing:u8, [events...] | Event queue listing |

**LED_SEQ_QUEUE_RESP Event Format:** Each event is 4 bytes: `type:u8, duration:u16le, param1:u8`

#### LED_SEQ_ADD Event Types

| Type | Name | Parameters | Description |
|------|------|------------|-------------|
| 0x00 | ON | duration:u16le, pwmDuty:u16le, brightness:u8 | Constant on (pwmDuty 0=off, 1-100=power save) |
| 0x01 | OFF | duration:u16le | Constant off |
| 0x02 | FLASH | interval:u16le, duration:u16le, brightness:u8, duty:u8 | On/off flashing |
| 0x03 | FADE_IN | duration:u16le, unused:u16le, brightness:u8 | Fade from off to on |
| 0x04 | FADE_OUT | duration:u16le, unused:u16le, brightness:u8 | Fade from on to off |
| 0x05 | FADING | cycle:u16le, duration:u16le, min:u8, max:u8 | Sinusoidal breathing |
| 0x06 | BEACON | cycle:u16le, duration:u16le, flashPct:u8, max:u8, min:u8 | Rotating beacon flash |

**Duration = 0 (infinite):** For ON, OFF, FLASH, FADING, and BEACON, setting `duration` to 0 means the event runs indefinitely. **For ON and OFF this prevents the sequence from advancing to the next event and from looping.** Use an infinite ON or OFF as the last event in a sequence when you want a steady terminal state (e.g., `FADE_IN 1000ms → ON ∞` fades in and stays on permanently).

### Light Program Runtime

Resolved against the `programs:` section of the loaded `/lightfx.yaml`. Hub uses these to drive both its local channels and the LightFX slave from the same program index.

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x4D | LIGHT_PROGRAM_SELECT | index:u8 | Activate program by 0-based index. NACKs INVALID_PROGRAM (0x57) when no config is loaded or `index >= programCount`. |
| 0x4E | LIGHT_PROGRAM_RESET | (none) | Stop sequences, retract every landing group on the lightfx side, leave no active program |

### Servo Control

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x50 | SERVO_SET | id:u8, pulse:u16le | Set servo position (500-2500µs) |
| 0x51 | SERVO_SETTINGS | id:u8, min:u16le, max:u16le, speed:u16le, accel:u16le, decel:u16le | Configure servo |

### Landing Light Control

Coordinates a retract servo with a landing light LED channel. Up to 3 landing light slots.

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x52 | LANDING_LIGHT_BIND | slot:u8, servo_id:u8, channel_mask:u8, brightness:u8 | Bind servo (0=none) + LED channel mask (bit N → ch N+1) as a landing group |
| 0x53 | LANDING_LIGHT_UNBIND | slot:u8 (0=all) | Unbind landing light slot |
| 0x54 | LANDING_LIGHT_DEPLOY | slot:u8 (0=all) | Deploy gear, light on when arrived |
| 0x55 | LANDING_LIGHT_RETRACT | slot:u8 (0=all) | Light off immediately, then retract gear |
| 0x56 | LANDING_LIGHT_STATUS | slot:u8, phase:u8, finished:u8 | Deploy/retract progress (server→client, echoes request tag) |

### Battery / Power Safety

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0xEE | BATTERY_CONFIG | chemLen:u8, chem:str, cells:u8 | Set battery chemistry + cell count (cells=0 → auto-detect from current voltage) |
| 0x5E | BATTERY_AUTO_CUTOFF | enabled:u8 | Arm/disarm soft cutoff: when armed, hitting the per-cell low threshold disables every LED channel until the pack recovers or the board is power-cycled |

The board always monitors battery voltage on VSYS. Soft cutoff is **on by default** so a battery-powered prop won't keep flashing through a low pack — disarm when running on a bench supply or USB.

#### LANDING_LIGHT_STATUS (Deploy/Retract Progress)

Emitted by the server during landing light deploy/retract to report progress. Uses the tag from the original LANDING_LIGHT_DEPLOY or LANDING_LIGHT_RETRACT request.

**Wire format (3 bytes, server→client, echoes request tag):**
```
[slot:u8][phase:u8][finished:u8]
```

| Field | Type | Description |
|-------|------|-------------|
| slot | u8 | Landing light slot (1-3) |
| phase | u8 | LandingLightPhase enum value |
| finished | u8 | 1 if transition complete, 0 otherwise |

**LandingLightPhase enum:**
| Value | Name | Description |
|-------|------|-------------|
| 0 | RETRACTED | Servo retracted, light off |
| 1 | DEPLOYING | Servo moving to deploy position, light off |
| 2 | DEPLOYED | Servo at deploy position, light on |
| 3 | RETRACTING | Light off, servo moving to retract position |

**Emission points:**
1. Deploy command → phase=DEPLOYING
2. Servo reaches deploy target → phase=DEPLOYED, finished=1
3. Retract command → phase=RETRACTING
4. Servo reaches retract target → phase=RETRACTED, finished=1

**Tag correlation:** The client resolves the pending tag when `finished=1`, providing the equivalent of a deferred ACK for the long-running operation.

**State Machine:**
```
UNCONFIGURED → (bind)    → RETRACTED
RETRACTED    → (deploy)  → DEPLOYING    [servo moving, light OFF]
DEPLOYING    → (arrived) → DEPLOYED     [light ON]
DEPLOYED     → (retract) → RETRACTING   [light OFF immediately, servo moving]
RETRACTING   → (arrived) → RETRACTED
```

**Sequencing:**
- **Deploy:** Servo moves to deploy position → light turns ON when servo reaches target
- **Retract:** Light turns OFF immediately → servo moves to retract position

---

## LED Event Sequences

Each LED channel has its own `LedEventSeq` that can play a sequence of events:

| Event | Parameters | Description |
|-------|------------|-------------|
| LedOn | duration_ms, brightness | Constant on for duration |
| LedOff | duration_ms | Constant off for duration |
| LedFlash | interval_ms, duration_ms, brightness, duty% | On/off flashing |
| LedFadeIn | duration_ms, target_brightness | Fade from off to target |
| LedFadeOut | duration_ms, start_brightness | Fade from start to off |
| LedFading | cycle_ms, duration_ms, min, max | Sinusoidal breathing |

**Example Sequence (strobe beacon):**
```
1. LedFadeIn(500, 100)    # Fade in over 500ms
2. LedOn(1000, 100)       # Hold at full for 1s
3. LedFlash(50, 2000, 100, 50)  # Strobe for 2s
4. LedFadeOut(500, 100)   # Fade out over 500ms
5. LedOff(1000)           # Off for 1s
# Sequence loops
```

---

## Error Codes

### Generic Errors (SerialError namespace)

| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | Success |
| 0x03 | INVALID_COMMAND | Unknown command |
| 0x04 | MISSING_PARAMETER | Required parameter missing |
| 0x10 | INVALID_PARAM | Generic invalid parameter |
| 0x12 | INVALID_ID | Invalid device/channel ID |

### LightFX-Specific Errors (0x50-0x5F)

| Code | Name | Description |
|------|------|-------------|
| 0x50 | INVALID_CHANNEL | Channel number out of range (1-8) |
| 0x51 | SEQ_FULL | Sequence already has 24 events |
| 0x52 | INVALID_EVENT | Unknown event type |
| 0x53 | INVALID_PARAM | Event parameter out of range |
| 0x54 | INVALID_SERVO | Servo ID out of range (1-3) |
| 0x55 | INVALID_SLOT | Invalid landing light slot (1-3) |
| 0x56 | CHANNEL_DISABLED | LED channel is disabled (use LED_RESET / LED_ENABLE to re-arm) |
| 0x57 | INVALID_PROGRAM | LIGHT_PROGRAM_SELECT index out of range / no program config loaded |

---

## Flash Storage

LightFX includes onboard LittleFS flash storage for standalone configuration persistence.

- **Backend:** `FlashModule` singleton from `sfx_storage` library
- **Config:** `ConfigStore<LightFxConfigSchema>` — `master_brightness` + `battery: { auto_cutoff, chemistry, cell_count }`
- **Config path:** `/lightfx.yaml` (default, set by `LightFxConfigSchema::defaultPath()`)
- **Initialized at boot:** `initFlashAndConfig()` in `setup()` mounts LittleFS, loads config, and applies it (master brightness, chemistry/cells, auto-cutoff arm state)
- **Reload:** `configServer.onReloaded(applyConfig)` — `config.reload` re-applies the YAML without rebooting

#### Example `/lightfx.yaml`

```yaml
master_brightness: 100

battery:
  auto_cutoff: true
  chemistry: lipo
  cell_count: 0      # 0 = auto-detect from voltage on connect
```

The flash infrastructure enables DIRECT mode operation — when INIT is sent with `mode=DIRECT`, the board can operate standalone without HubFX, reading settings from flash.

### Board State Machine

| State | Value | Description |
|-------|-------|-------------|
| IDLE | 0x00 | Power-on default, no config loaded |
| STANDALONE | 0x01 | Config loaded from flash at boot |
| SLAVE | 0x02 | INIT received with mode=SLAVE |
| DIRECT | 0x03 | INIT received with mode=DIRECT |

Transitions: IDLE → STANDALONE (on config load) → SLAVE/DIRECT (on INIT) → IDLE (on SHUTDOWN/timeout)

---

## Architecture

Uses `PicoServer` component for common server boilerplate (serial init, device naming, indicator LEDs, core protocol, connection management). Module-specific logic is handled by `LightFxServer`.

```
┌─────────────────────────────────────────────────────────────┐
│                      PicoServer                              │
│  Serial init, device name, indicators, connection timeout   │
├─────────────────────────────────────────────────────────────┤
│                     CommandRouter                            │
│  Routes COBS packets to handlers in priority order          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────────┐  ┌─────────────────────────────┐  │
│  │  CoreCommandServer  │  │     LightFxServer            │  │
│  │  (Priority 1)       │  │      (Priority 2)           │  │
│  ├─────────────────────┤  ├─────────────────────────────┤  │
│  │ INIT, SHUTDOWN      │  │ LED_SET, LED_OFF            │  │
│  │ REBOOT, BOOTSEL     │  │ LED_SEQ_* commands          │  │
│  │ KEEPALIVE, STATUS   │  │ SERVO_SET, SERVO_SETTINGS   │  │
│  └─────────────────────┘  │ LANDING_LIGHT_* commands    │  │
│                            └─────────────────────────────┘  │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## Build & Upload

### Using PlatformIO

```bash
cd controllers/lightfx/pico

# Build
pio run

# Build and upload
pio run -t upload

# Clean build
pio run -t clean
```

### Manual BOOTSEL Method

1. Hold BOOTSEL button on Pico while connecting USB
2. RPI-RP2 drive appears
3. Copy `.pio/build/pico/firmware.uf2` to drive
4. Device auto-reboots with new firmware

---

## Version History

- **v0.15.0** - Light program runtime + remote select/reset (MINOR — additive)
  - New packets: `LIGHT_PROGRAM_SELECT (0x4D)` `[index:u8]` activates a program by 0-based index from the loaded `/lightfx.yaml`; `LIGHT_PROGRAM_RESET (0x4E)` (no payload) stops sequences, retracts every landing group on the lightfx side, leaves no active program. New error: `INVALID_PROGRAM (0x57)` returned when no config is loaded or the index is past `programCount`.
  - `LightProgramManager` is now actually wired into `lightfx_pico.ino` (previously the program manager class existed but the firmware used an inline `applyLightProgramConfigLocal()` that only configured servos and landing bindings — the `programs:` YAML section was parsed but never executed). The manager owns the loaded config; LED-sequence loading + group-policy deploy/retract per program now run through it. `applyConfig()` calls `progMgr.loadConfig(cfg.lightProgram)` instead of inlining the apply logic.
  - New `LightProgramManager::resetProgram()` mirrors `selectProgram()` board-filtering — only retracts groups with at least one channel on this board's side, so a reset on the slave doesn't disturb hub-only groups.
  - Studio / hub now drive program switching across both boards in lock-step: hub picks the program from /lightfx.yaml's input bands, runs its own local channels, then sends `LIGHT_PROGRAM_SELECT` to the slave so the lightfx-side channels sync. Hub-side input layer arrives in HubFX Phase 0.
  - CLI: `program <index>` and `program.reset` commands wired through `app/go/engine/handlers/lightfx/handler.go`.
  - Wire format: 7-file Rule 2 checklist applied (`lightfx.h` + server/client + `lightfx_pico.ino` + Go protocol/api/handler + this README). No breaking changes — additive packets only.
  - Bumped FIRMWARE_VERSION 0.14.0 → 0.15.0.

- **v0.14.0** - Cross-board light-program schema (MINOR — additive YAML fields)
  - `LightProgramConfig::LandingGroup` splits `channelMask` → `lightfxChannelMask` (8 bits, slave-side) + `hubfxChannelMask` (6 bits, hub-local LEDs); adds `servoBoard[8]` ("lightfx" \| "hubfx", default "lightfx").
  - `LightProgramConfig::ChannelDef` adds `board[8]` (default "lightfx") so a single program can span both boards.
  - `applyLightProgramConfigLocal()` now consumes `g.lightfxChannelMask` and only binds a servo when `LightProgramBoard::isLightFx(g.servoBoard)`. Hub-side channel-defs in the same `/lightfx.yaml` are silently skipped on the slave.
  - YAML keys: `landing_groups[].lightfx_channels` / `hubfx_channels` / `servo_board`, `programs[].channels[].board`. Legacy `channel_mask` is still parsed and treated as `lightfx_channels` (Rule 11 — append-only).
  - `LightProgramManager` (shared lib) gains a `board` parameter on `begin()`; the same orchestrator class now drives either side.
  - Wire format unchanged: `LANDING_LIGHT_BIND (0x52)` still carries an 8-bit lightfx-only `channelMask`. The hub masks the hubfx-side bits before pushing to the slave.
  - Bumped FIRMWARE_VERSION 0.13.0 → 0.14.0.

- **v0.13.0** - Delete applier stack; plain config-apply function (MINOR — refactor)
  - Removed `LocalLightFxApplier` + `SingleApplierRouter` + `BoardOrchestrator` + `LightProgramPolicy` wiring from `lightfx_pico.ino`.
  - Replaced with a single plain function `applyLightProgramConfigLocal(const LightProgramConfig&)` that walks the config and calls `ledManager.setMasterBrightness`, per-servo `setLimits/setMaxSpeed/setAcceleration/setDeceleration/setReversed/setTarget`, and per-group `landingLights[i].unconfigure()/setSlot()/configure(...)` directly.
  - Motivation: the one-instance-per-board invariant (a lightfx pico applies only its own config) made the compile-time policy dispatch pure overhead. Matches the hub-side simplification in HubFX 0.38.0 (`pushLightFxConfigToSlave`).
  - Shared-lib directory `controllers/lib/sfx_boards/lightfx/applier/` deleted.
  - Bumped FIRMWARE_VERSION 0.12.1 → 0.13.0

- **v0.12.1** - Larger YAML parser pool (PATCH — bug fix + headroom)
  - YAML parser pool bumped from default (128 nodes / 4 KB strings) to a board-local `LightFxYamlPool` (1024 nodes / 32 KB strings / depth 16).
  - Reason: Studio writes a richer `/lightfx.yaml` (programs × channels × events, landing groups, servo bindings) than the firmware schema consumes. The parser still has to allocate a node per key/sequence-item while scanning, even though most are ignored at populate() time. The default pool exhausted around line 100, breaking `config.reload`.
  - `LightFxConfigSchema::populate` is now templated on `TPool` so `ConfigStore` can instantiate it with any preset.
  - **Heap is fully transient**: `ConfigStore::loadFromString` creates the parser on the stack; `parse()` heap-allocates the pools via `new[]` and the destructor (`~YamlParser → reset → delete[]`) frees them as soon as the call returns. So the larger pool only affects the brief peak during `config.reload`/boot — never persistent heap.
  - Peak transient: ~56 KB during parse (~24 KB nodes + 32 KB strings). RP2040 has 264 KB SRAM with ~230 KB free → 4× headroom even during the parse window. Comfortably handles a fully populated Studio config (8+ programs × 8 channels × 16 events plus landing groups and servo bindings).

- **v0.12.0** - Battery monitor + soft cutoff (MINOR — additive)
  - New: BATTERY_AUTO_CUTOFF (0x5E) command — disable every LED channel when the pack drops below the chemistry's per-cell low threshold
  - New YAML schema fields: `master_brightness`, `battery: { auto_cutoff, chemistry, cell_count }` — applied on boot and on `config.reload` via `applyConfig()`
  - STATUS payload extended to 25 bytes (byte 24 = battery flags: bit0=autoCutoff, bit1=lowVoltageTriggered) — older clients reading 24-byte payloads still work (Rule 11)
  - VERBOSE source: board now sets `StatusUpdateSource::LIGHTFX` so async STATUS_UPDATE is filterable per board
  - Studio: `LightFxTab` now mirrors GearControl — battery card with voltage bar / chemistry / cell count, live-push (~350ms), per-board YAML auto-load on connect
  - Bumped FIRMWARE_VERSION 0.11.0 → 0.12.0

- **v0.11.0** - Landing light groups (MINOR — wire-format break in 0x52)
  - LANDING_LIGHT_BIND payload reshaped: `slot, servo, channel_mask, brightness` (mask covers up to 8 LEDs per group, servo is optional). See [instructions/11-LANDING-LIGHT-GROUPS.md](../../../instructions/11-LANDING-LIGHT-GROUPS.md).

- **v0.6.0** - Landing light progress reporting
  - New: LANDING_LIGHT_STATUS (0x56) emitted during deploy/retract sequences
  - Reports phase transitions (RETRACTED → DEPLOYING → DEPLOYED, and reverse)
  - Uses tag correlation from original DEPLOY/RETRACT request
  - Client auto-resolves pending tag when finished=1

- **v0.5.0** - Brightness 0-100 scale
  - Changed brightness from 0-255 PWM to 0-100% human-readable scale
  - Applies to: LED_SET, LED_SEQ_ADD events, LANDING_LIGHT_BIND brightness
  - LedControl component converts 0-100 → 0-255 PWM at hardware output
  - Master brightness (0-100%) combined with channel brightness at output
  - All LED events (LedOn, LedFlashing, LedFadeIn, etc.) use 0-100 scale

- **v0.4.0** - Master brightness control
  - New `LedControl::setMasterBrightness_pct()` in components library
  - Global 0-100% brightness scaling applied at PWM output
  - New command: LED_MASTER_BRIGHTNESS (0x4A)
  - Reset to 100% on SHUTDOWN/INIT
  - STATUS response extended to 19 bytes (added master brightness)

- **v0.3.0** - Landing light sequencer
  - New LandingLight class: binds retract servo with LED channel
  - Light activates when deployment completes (servo at target)
  - Light deactivates before retraction starts
  - Up to 3 landing light slots
  - New commands: LANDING_LIGHT_BIND/UNBIND/DEPLOY/RETRACT (0x52-0x55)
  - STATUS response extended to 18 bytes (added landing light states)

- **v0.2.0** - Binary-only protocol
  - Removed text protocol support
  - Using new serial library (CoreCommandServer + LightFxServer)
  - COBS framing with CRC-8 validation
  - Removed INA226 power monitoring (board redesign)

- **v0.1.0** - Initial release
  - 8-channel LED output with PWM
  - LED event sequences
  - 3 servo outputs with motion profiling
  - Status LEDs
