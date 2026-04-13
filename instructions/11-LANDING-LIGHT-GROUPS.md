# Landing Light Groups — Design Document

> **Scope:** Studio UI, C++ config schema, LandingLight firmware, LightFX protocol, Go CLI.

---

## 1. Overview

Landing Light Groups replace the flat slot-based landing light system with a named,
multi-channel group model. Each group coordinates **one optional servo** with
**zero or more LED channels** to implement deploy/retract sequences.

### Key Design Principles

1. **Group = logical unit** — a group owns a name, an optional servo binding, and
   a set of LED channels. This replaces the old 1:1 "slot → servo + single LED" model.
2. **Channel assignment is per-program** — each LED channel in a program can be in
   `events` mode (plays its own sequence) or in `group` mode (controlled by the group
   state machine). The same channel can be in different modes across programs.
3. **Group policy is per-program** — each program declares what each group does:
   - `on` — deploy the group (servo moves to open position, LEDs on)
   - `off` — retract the group (LEDs off, servo closes)
   - `gear` — controlled externally by HubFX gear input
4. **Backward-compatible slot mapping** — group index 0 → slot 1, group index 1 → slot 2,
   group index 2 → slot 3. Max 3 groups (matching firmware's 3 LandingLight instances).
5. **Servo binding is optional** — a group without a servo binding still controls its
   LED channels when group policy is `on`/`off`. This enables "LED-only" groups for
   lighting setups without retract servos.

---

## 2. Data Model

### 2.1 Studio UI State (Svelte)

```typescript
interface ServoBindingState {
    servo: number          // Servo ID (1-3)
    servoPulse_us: number  // Current position for live preview
    servoMin_us: number    // Minimum pulse limit (µs)
    servoMax_us: number    // Maximum pulse limit (µs)
    servoSpeed: number     // Max speed (µs/sec)
    servoAccel: number     // Acceleration (µs/sec²)
    servoDecel: number     // Deceleration (µs/sec²)
    servoReversed: boolean // Invert open/close direction
}

interface LandingGroupState {
    name: string
    binding: ServoBindingState | null  // At most one servo per group
}
```

Channel-to-group assignment lives in the per-program channel state:

```typescript
interface ChannelState {
    mode: 'events' | 'group'
    groupIndex: number     // Which group (when mode is 'group')
    events: EventState[]   // Used when mode is 'events'
    enabled: boolean
}
```

Per-program group policy:

```typescript
type GroupPolicy = 'on' | 'off' | 'gear'

interface Program {
    name: string
    groupPolicies: GroupPolicy[]  // One per group, indexed by group index
    channels: ChannelState[]
}
```

### 2.2 C++ Config Schema (`LightProgramConfig`)

The config struct in `light_program_config.h` is updated to:

```cpp
struct LandingGroup {
    char     name[16]     = {};     // Display name
    uint8_t  servoId      = 0;     // Bound servo ID (1-3, 0 = none)
    uint8_t  channelMask  = 0;     // LED channels bitmask (bit 0 → ch1 ... bit 7 → ch8)
    uint8_t  brightness   = 100;   // Light brightness when deployed (0-100)
};

LandingGroup landingGroups[MAX_LANDING_GROUPS] = {};
uint8_t      landingGroupCount = 0;
```

Programs gain per-group policy:

```cpp
struct Program {
    char       name[16]    = {};
    uint8_t    groupPolicies[MAX_LANDING_GROUPS] = {};  // 0=off, 1=on, 2=gear
    ChannelDef channels[MAX_CHANNELS_PER_PROGRAM] = {};
    uint8_t    channelCount = 0;
};
```

Channels gain group assignment:

```cpp
struct ChannelDef {
    uint8_t  channel    = 0;       // LED channel (1-8, 0 = unused)
    uint8_t  groupIndex = 0xFF;    // 0xFF = not in group (events mode)
    // ... events as before
};
```

### 2.3 YAML Config File

```yaml
master_brightness: 100

servos:
  - id: 1
    min_us: 500
    max_us: 2500
    speed: 4000
    reversed: false

landing_groups:
  - name: "Main Gear"
    servo_id: 1
    channels: [5, 6]       # LED channels controlled by this group
    brightness: 100

input:
  channel: 1
  bands:
    - min_us: 1000
      max_us: 1400
      program: 0

programs:
  - name: "NAV"
    group_policies: [off]   # Group 0 retracted
    channels:
      - channel: 1
        events:
          - type: beacon
            cycle_ms: 1200
      - channel: 5
        group: 0            # Controlled by group 0
      - channel: 6
        group: 0

  - name: "LANDING"
    group_policies: [on]    # Group 0 deployed
    channels:
      - channel: 1
        events:
          - type: on
            brightness: 100
      - channel: 5
        group: 0
      - channel: 6
        group: 0
```

---

## 3. Wire Protocol Changes

### 3.1 LANDING_LIGHT_BIND (0x52) — MODIFIED

**Old format (v0.8.0):**
```
[slot:u8][servoId:u8][ledChannel:u8][brightness:u8]  (4 bytes)
```

**New format (v0.9.0):**
```
[slot:u8][servoId:u8][channelMask:u8][brightness:u8]  (4 bytes)
```

| Byte | Field | Description |
|------|-------|-------------|
| 0 | slot | Landing light slot (1-3) |
| 1 | servoId | Bound servo ID (1-3), 0 = no servo |
| 2 | channelMask | LED channel bitmask (bit 0 → ch1 ... bit 7 → ch8) |
| 3 | brightness | LED brightness when deployed (0-100%) |

**Change:** Byte 2 changes from single channel ID (1-8) to a channel bitmask.
This is a **breaking change** — old clients sending `ledChannel=5` will be
misinterpreted as mask 0x05 (channels 1+3).

**Examples:**
- Channel 5 only: mask = `0x10` (bit 4)
- Channels 5+6: mask = `0x30` (bits 4+5)
- Channels 1+2+3+4: mask = `0x0F`
- No channels (servo-only): mask = `0x00`

### 3.2 LANDING_LIGHT_UNBIND, DEPLOY, RETRACT — UNCHANGED

No changes to 0x53, 0x54, 0x55. Slot semantics remain the same.

### 3.3 LANDING_LIGHT_STATUS (0x56) — UNCHANGED

No changes to the async progress packet. Still reports per-slot phase/finished.

### 3.4 Version Impact

- Firmware: v0.8.0 → **v0.9.0** (MINOR — new group feature, BIND format changed)
- Build number: increment
- Note: Although BIND byte 2 semantics changed (channel ID → mask), this is pre-1.0
  and the only clients (HubFX, Go CLI, Studio) are updated simultaneously.

---

## 4. LandingLight Class Enhancement

### 4.1 Multi-LED Support

The `LandingLight` class in `controllers/lightfx/pico/src/landing_light.h` is
enhanced to support up to 8 LED channels per group:

```cpp
static constexpr uint8_t MAX_LEDS = 8;

void configure(ServoControl* servo, LedControl* leds[], uint8_t ledCount,
               uint8_t brightness = 100);
```

**State machine remains the same:**
```
UNCONFIGURED → (configure) → RETRACTED
RETRACTED    → (deploy)    → DEPLOYING    [all LEDs OFF]
DEPLOYING    → (atTarget)  → DEPLOYED     [all LEDs ON]
DEPLOYED     → (retract)   → RETRACTING   [all LEDs OFF immediately]
RETRACTING   → (atTarget)  → RETRACTED
```

**Servo-optional support:**
- If `servo == nullptr`, configure() skips servo setup
- deploy() immediately sets state to DEPLOYED (no servo motion to wait for)
- retract() immediately sets state to RETRACTED (no servo motion)

### 4.2 Servo Interaction

When a servo is bound:
- Open/close positions derived from servo's limits + reversed flag
- Servo settings are applied BEFORE the bind (via SERVO_SETTINGS command)
- LandingLight monitors servo.atTarget() for state transitions

When no servo (servo-only group not needed):
- Group still controls LED on/off based on program's group policy
- deploy() turns LEDs on immediately
- retract() turns LEDs off immediately

---

## 5. Firmware Config Loading

### 5.1 Config Application Sequence

When `ConfigStore` loads the YAML:

1. **Servos:** Apply servo hardware config (limits, speed, acceleration, reversed)
2. **Landing Groups:** For each group:
   a. Resolve LED channels from `channelMask` or `channels[]` list
   b. Resolve servo from `servoId` (if > 0)
   c. Call `landingLights[i].configure(servo, leds, count, brightness)`
3. **Programs:** Store program definitions with group policies
4. **Apply active program:** Deploy/retract groups per active program's policies

### 5.2 Program Switching

When the RX input selects a new program:

1. For each group, check `program.groupPolicies[gi]`:
   - `off` → retract group
   - `on`  → deploy group
   - `gear` → defer to HubFX gear controller input
2. For each channel in `events` mode: clear sequence, load events, start playback
3. For each channel in `group` mode: LED state managed by group deploy/retract

---

## 6. Go CLI Changes

### 6.1 Protocol (`protocol/lightfx/lightfx.go`)

Update `CmdLandingLightBind`:
```go
func CmdLandingLightBind(slot, servoID, channelMask, brightness byte) []byte
```

### 6.2 API (`api/lightfx.go`)

```go
func (a *LightFxApi) LandingBind(slot, servo, channelMask, bright byte) ApiResult
```

### 6.3 CLI Handler

Update `landing.bind` syntax:
```
landing.bind <slot> <servo> <channels> [brightness]
```

Where `<channels>` is a comma-separated list: `5,6` → mask `0x30`.

---

## 7. Studio GUI — Play Button Behavior

### 7.1 Deploy/Retract Flow

When the user clicks **Deploy** on a group:

1. **Apply servo config** (if binding exists):
   `servo.config <servoId> <min> <max> <speed> <accel> <decel> [rev]`
2. **Bind landing light** (derive channel mask from group members):
   `landing.bind <slot> <servoId> <channelMask> <brightness>`
3. **Deploy**: `landing.deploy <slot>`

When the user clicks **Retract**: `landing.retract <slot>`

### 7.2 Channel Mask Derivation

The Studio derives the channel mask for a group from all channels in the active
program that have `mode === 'group'` and `groupIndex === gi`:

```typescript
function groupChannelMask(gi: number): number {
    let mask = 0
    for (const p of programs) {
        for (let ci = 0; ci < p.channels.length; ci++) {
            if (p.channels[ci].mode === 'group' && p.channels[ci].groupIndex === gi) {
                mask |= (1 << ci)
            }
        }
    }
    return mask
}
```

---

## 8. Config Loading Feature Plan

### 8.1 LightProgramManager Integration

`LightProgramManager` (in `sfx_peripherals/led/`) is the primary class responsible
for loading config and managing program switching. It currently handles:

- Servo configs (applying limits, speed, direction to `ServoControl` objects)
- Landing group bindings (calling the `LandingBindFn` callback for each group)
- Program selection (loading LED sequences and applying group policies)

**Config application sequence (already implemented in `loadConfig()`):**

| Phase | Code Path | Status |
|-------|-----------|--------|
| 1. Servos | `applyServoConfigs(cfg)` | ✅ Implemented |
| 2. Landing groups | `applyLandingBindings(cfg)` | ✅ Updated for new group model |
| 3. Default program | `selectProgram(0)` | ✅ Calls `applyGroupPolicies()` |

### 8.2 Firmware YAML Parsing Requirements

The `yaml_parser.h` template-based parser handles:

| YAML Node | Schema DSL | Status |
|-----------|-----------|--------|
| `master_brightness` | `scalar<uint8_t>` | ✅ In schema |
| `landing_groups[]` | `seq<LandingGroup>` | ✅ In schema |
| `landing_groups[].name` | scalar | ✅ In schema |
| `landing_groups[].servo_id` | scalar | ✅ In schema |
| `landing_groups[].channel_mask` | scalar (hex or decimal) | ✅ In schema |
| `landing_groups[].brightness` | scalar | ✅ In schema |
| `input.channel` | scalar | ✅ In schema |
| `input.bands[]` | sequence | ✅ In schema |
| `programs[]` | sequence | ✅ In schema |
| `programs[].group_policies` | bare-scalar sequence | ⚠️ Manual parse needed |
| `programs[].channels[]` | sequence | ✅ In schema |
| `programs[].channels[].group` | scalar (group index) | ✅ In schema |

**Known limitation:** The YAML schema DSL `seq<>` does not support bare-scalar
sequences like `group_policies: [off, on, gear]`. These must be parsed manually
in the firmware config-loading code (iterate YAML sequence node, match strings
"off"/"on"/"gear" to `GROUP_POLICY_OFF`/`GROUP_POLICY_ON`/`GROUP_POLICY_GEAR`).

### 8.3 Program Switching Behavior

When the RX input (PPM) selects a new program via band detection:

```
new band detected → LightProgramManager::selectProgram(programIndex)
  ├─ applyGroupPolicies(program)
  │   └─ for each group gi:
  │       ├─ policy == OFF  → retract group (LEDs off, servo close if bound)
  │       ├─ policy == ON   → deploy group (LEDs on, servo open if bound)
  │       └─ policy == GEAR → no action (controlled by external gear input)
  ├─ clearAllSequences()     // stop all LED event playback
  └─ loadProgramEvents(program)
      └─ for each channel with groupIndex == 0xFF:
          load events[] into LedSequencePlayer
          start playback
```

**State transitions on program switch:**

| Previous State | New Policy | Action |
|----------------|-----------|--------|
| DEPLOYED | off | Retract (servo close → LEDs off) |
| DEPLOYED | on | No-op (already deployed) |
| DEPLOYED | gear | No-op (external control takes over) |
| RETRACTED | off | No-op (already retracted) |
| RETRACTED | on | Deploy (LEDs on → servo open) |
| RETRACTED | gear | No-op (external control takes over) |
| DEPLOYING/RETRACTING | * | Wait for motion completion, then apply |

### 8.4 Gear Input Integration (HubFX Slave Mode)

When LightFX runs as a slave to HubFX:
- HubFX monitors the gear input channel (RC receiver)
- On gear state change, HubFX sends `landing.deploy <slot>` or `landing.retract <slot>`
  to the LightFX slave
- Groups with `gear` policy are controlled exclusively by this external input
- Groups with `on`/`off` policy are controlled by the local program switch

### 8.5 Config Save Flow (Studio → Firmware)

```
Studio "Save Config" button
  └─ buildLightConfig() → LightConfig snapshot
       └─ generateLightYaml(config) → YAML text
            └─ SendCommand(`config.save`) → firmware writes to flash
```

The config is saved via the `sfx_config` library's `CONFIG_SAVE` protocol:
- Studio generates YAML text matching the C++ `LightProgramConfig` schema
- Firmware receives the YAML via the upload protocol
- `ConfigStore` validates against the schema and applies the config

### 8.6 Remaining Work Items

| Item | Priority | Status |
|------|----------|--------|
| Manual `group_policies` parsing in firmware | High | 🔲 Not implemented |
| Config reload via protocol (`CONFIG_RELOAD`) | Medium | ✅ Infrastructure exists |
| Studio → firmware YAML upload | Medium | 🔲 Needs upload protocol |
| Default program on boot from flash config | Medium | 🔲 Needs config load trigger |
| PPM input band detection → program switch | Low | 🔲 Requires PPM hardware setup |
| HubFX gear input forwarding to slave groups | Low | 🔲 Requires HubFX integration |
