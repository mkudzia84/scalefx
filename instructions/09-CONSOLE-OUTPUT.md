# Console Output Schema

> **Reference document for all CLI platforms (Python, Go, C#).** Python is the canonical reference.
> All platforms MUST produce equivalent output for the same wire data.

---

## Overview

Three CLI platforms share the same binary protocol and MUST produce harmonized output:

| Platform | Location | Output Medium |
|----------|----------|---------------|
| **Python CLI** (reference) | `tests/cli/` | `print()` + colorama |
| **Go CLI** | `tools/cli/` | ANSI escape codes |
| **C# Console** | `app/win32/ScaleFXSerial/Console/` | `IConsoleOutput` interface |

### Output Method Mapping

| Semantic | Python | Go | C# (`IConsoleOutput`) |
|----------|--------|----|-----------------------|
| Labeled value | `print(f"  Label: value")` | `fmt.Printf("  Label: value\n")` | `WriteData("Label", "value")` |
| Section header | `print("  ── Title ──────")` | `fmt.Printf("  ── Title ──────\n")` | `WriteInfo("── Title ──────")` |
| Success | `print_success(msg)` | green text | `WriteSuccess(msg)` |
| Error | `print_error(msg)` | red text | `WriteError(msg)` |
| Warning | `print_warning(msg)` | yellow text | `WriteWarning(msg)` |
| Info | `print_info(msg)` | cyan text | `WriteInfo(msg)` |
| Raw line | `print(f"  text")` | `fmt.Printf("  text\n")` | `WriteLine("  text")` |

---

## Core Parsers

### INIT_READY / IDENTIFY

**Wire:** `[nameLen:u8][name:str][verLen:u8][ver:str][platLen:u8][plat:str][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]`

**Output:**
```
  Device:   {name}
  Version:  {version} (build {build})
  Platform: {platform} @ {cpu_mhz}MHz
  Free RAM: {free_ram} bytes
```

### STATUS

**Wire (20-byte header):** `[counter:u32LE][uptime_ms:u32LE][freeRam:u32LE][lastActivity_ms:u32LE][keepalives:u32LE]`
+ module-specific data after byte 20.

**Output (core header):**
```
  Commands:  {counter}
  Uptime:    {hours}h {minutes}m {seconds}s
  Free RAM:  {bytes} bytes ({KB:.1f} KB)
  Last seen: {formatted_time} ago  (keepalives: {count})
```
Then dispatches module data based on detected controller type → GunFX / LightFX / GearControl / HubFX status parser.
If no controller detected, shows raw hex of module bytes.

### I2C_SCAN_RES

**Wire:** `[numExpected:u8][{addr:u8, found:u8, identified:u8} × N][numExtra:u8][{addr:u8} × M]`

**Output:**
```
  ── I2C Bus Scan ───────────────
  Expected devices: {num}
    0x{addr:02X}: OK (found + verified)         ← found=1, identified=1
    0x{addr:02X}: FOUND (ACK but not verified)   ← found=1, identified=0
    0x{addr:02X}: MISSING (no ACK)               ← found=0
  Other devices: 0x{addr}, 0x{addr}             ← only if numExtra > 0
```

### LOG_MESSAGE (async)

**Wire:** `[level:u8][millis:u32LE][message:str]`

**Levels:** 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR

**Output:**
```
  [{secs}.{ms:03d}] {LEVEL} {message}
```
Color: DEBUG=dim, INFO=cyan, WARN=yellow, ERROR=red.

---

## GunFX Status (module data, 28 bytes)

**Wire:**
| Offset | Field | Type |
|--------|-------|------|
| 0 | flags | u8: bit0=firing, bit1=flash_active, bit2=flash_fading, bit3=heater_on, bit4=fan_on, bit5=fan_spindown |
| 1 | fanSpeed | u8 |
| 2-3 | fanOffMs | u16LE |
| 4-5 | servo0 | u16LE |
| 6-7 | servo1 | u16LE |
| 8-9 | servo2 | u16LE |
| 10-11 | rpm | u16LE |
| 12-15 | shots | u32LE |
| 16-19 | heaterMs | u32LE |
| 20 | heaterError | u8 (SmokeErrorReason) |
| 21 | fanError | u8 (SmokeErrorReason) |
| 22 | heaterDuty | u8 |
| 23 | fanDuty | u8 |
| 24-25 | batteryV_mV | u16LE |
| 26 | cellCount | u8 |
| 27 | batteryPct | u8 |

**SmokeErrorReason:** 0=NONE, 1=OVERCURRENT, 2=UNDERCURRENT, 3=TIMEOUT, 4=OVERHEAT

**Output:**
```
  ── GunFX ──────────────────────
  State:     FIRING, FLASH, HEATER, FAN       ← flags as comma list; "IDLE" if none
  Fire rate: {rpm} RPM                         ← only if firing
  Shots:     {shots}
  Fan:       speed={fanSpeed}, off in {fanOffMs}ms   ← only if fan active/spindown
  Heater:    {heaterMs/1000:.1f}s total        ← only if heaterMs > 0
  Servos:    [{servo0}µs, {servo1}µs, {servo2}µs]
  ── Smoke Errors ──────────────               ← only if errors exist
  Heater:    {error_name}                      ← red
  Fan:       {error_name}                      ← red
  ── Overcurrent Throttle ──────               ← only if duty < 255
  Heater:    throttled to {pct}% (duty {val}/255)  ← yellow
  Fan:       throttled to {pct}% (duty {val}/255)  ← yellow
  Battery:   {V:.2f}V ({mV}mV), {cellCount}S, {pct}%
```

---

## LightFX Status (module data, 24 bytes)

**Wire:**
| Offset | Field | Type |
|--------|-------|------|
| 0-7 | ledBrightness | u8×8 |
| 8 | ledSeqFlags | u8 (bit per channel) |
| 9-10 | servo0 | u16LE |
| 11-12 | servo1 | u16LE |
| 13-14 | servo2 | u16LE |
| 15-17 | landingLightStates | u8×3 |
| 18 | masterBrightness_pct | u8 |
| 19 | ledEnabledFlags | u8 |
| 20-21 | batteryV_mV | u16LE |
| 22 | cellCount | u8 |
| 23 | batteryPct | u8 |

**Landing light phases:** 0=RET, 1=DEPLOYING, 2=DEP, 3=RETRACTING

**Output:**
```
  ── LightFX ────────────────────
  LEDs:      ch1=255▶, ch3=128[DIS], ch5=64   ← or "all off"
  Master:    {pct}%                             ← only if < 100
  Servos:    [{servo0}µs, {servo1}µs, {servo2}µs]
  Lights:    slot1=DEP, slot2=RET, slot3=DEPLOYING
  Battery:   {V:.2f}V ({pct}%, {cellCount}S)
```
LED format: `ch{n}={brightness}` with `▶` if sequence playing, `[DIS]` if disabled.
Only shows channels with brightness > 0 or sequence active.

### LED_STATUS_RESP

**Wire:** 4 bytes per channel `[ch:u8][brightness:u8][seq_playing:u8][seq_count:u8]`

**Output:**
```
  ── LED Channel Status ──
  CH0: ████████ 100% | Seq: ▶ (3 events)
  CH1: ██░░░░░░  25% | Seq: ■ (0 events)
```

### LED_SEQ_STATUS_RESP

**Wire (8-9 bytes):** `[ch:u8][playing:u8][event_count:u8][current_index:u8][loop_count:u32LE][brightness:u8?]`

**Output:**
```
  ── LED {ch} Sequence Status ──
  Status:      PLAYING                         ← green; or STOPPED (yellow)
  Events:      {count}
  Current:     {index}
  Loop Count:  {loops}
  Brightness:  {brightness}%
```

### LED_SEQ_QUEUE_RESP

**Wire:** `[ch:u8][count:u8][current_index:u8][playing:u8][brightness:u8]` + per event `[type:u8][duration:u16LE][param1:u8]`

**Event types:** 0=ON, 1=OFF, 2=FLASH, 3=FADE_IN, 4=FADE_OUT, 5=FADING, 6=BEACON

**Output:**
```
  ── LED {ch} Sequence Queue (PLAYING, {count} events, brightness {bri}%) ──
  [0] ON      : 500ms (param=200) ← current
  [1] OFF     : 500ms (param=0)
  [2] FLASH   : 100ms (param=128)
```

### LANDING_LIGHT_STATUS (async)

**Wire (3 bytes):** `[slot:u8][phase:u8][finished:u8]`

**Output:**
```
  ▸ Light {slot}: {phase_name}                  ← or ✓ Light {slot}: {phase_name} complete
```

---

## GearControl Status (module data, 53 bytes)

**Wire:**
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0-10 | Gear 0 (Nose) | `[state:u8][motor_mA:u16LE][door0:u16LE][door1:u16LE][stall_mA:u16LE][shunt_10uV:i16LE]` | 11 bytes per gear |
| 11-21 | Gear 1 (Left Main) | same | |
| 22-32 | Gear 2 (Right Main) | same | |
| 33-34 | yaw_pos_us | u16LE | |
| 35 | led_flags | u8 | 5 bits: N_dep, L_dep, R_dep, CONN, ERR |
| 36-37 | battery_mV | u16LE | |
| 38 | battery_config_flags | u8 | bit0=enabled, bit1=auto_deploy, bit2=low_voltage |
| 39-41 | error_reasons | u8×3 | per gear |
| 42-43 | shuntResistance_mohm | u16LE | |
| 44-46 | door_modes (packed) | u8×3 | low nibble=pre, high nibble=post |
| 47-49 | config_flags | u8×3 | per gear, bit7=enabled |
| 50-52 | door_states | u8×3 | per gear |

**Gear states:** 0=UNKNOWN, 1=DEPLOYED, 2=RETRACTED, 3=DEPLOYING, 4=RETRACTING, 5=ERROR, 6=CALIBRATING
**Door states:** 0=CLOSED, 1=OPENING, 2=OPEN, 3=CLOSING
**Door modes:** 0=NONE, 1=FULL, 2=PARTIAL
**Error reasons:** 0=NONE, 1=STALL, 2=TIMEOUT, 3=OVERCURRENT, 4=DISABLED

**Output:**
```
  ── GearControl ────────────────
        Nose: {STATE}  [DISABLED]  {error_reason}
             motor={current}mA  shunt={mV:.1f}mV  stall={stall}mA
             doors=[{door0}µs, {door1}µs]  {doorState}  pre={mode}  post={mode}  yaw
   Left Main: ...
  Right Main: ...
  ── Global ─────────────────────
  Yaw:       {yaw}µs
  Shunt:     {mohm}mΩ ({ohm}Ω)  max={maxCurrent:.0f}mA
  Battery:   {V:.1f}V ({mV}mV), auto-deploy, LOW VOLTAGE  ← or "disabled"
  LEDs:      [N:dep, L:ret, R:off, CONN, err]
```

### GEAR_SEQ_STATUS (async)

**Wire (8 bytes):** `[gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]`

**Seq phases:** 0=IDLE, 1=OPEN_DOORS, 2=WAIT_DOORS, 3=START_MOTOR, 4=RUNNING, 5=STOP_MOTOR, 6=CLOSE_DOORS, 7=DONE, 8=ERROR

**Output:**
```
  ▸ {GearName} seq: {PHASE}, deploy/retract, {elapsed:.1f}s, [FINISHED in {elapsed:.1f}s]
```

### GEAR_DOOR_STATUS (async)

**Wire (2-6 bytes):** `[gear_id:u8][state:u8][door0_pos_us:u16LE?][door1_pos_us:u16LE?]`

**Output:**
```
  ◇ {GearName} doors: {STATE}, d0={pos}µs, d1={pos}µs
```

### GEAR_CALIB_STATUS (async)

**Wire (9-10 bytes):** `[gear_id:u8][phase:u8][current_mA:u16LE][peak_mA:u16LE][stall_mA:u16LE][finished:u8][errorReason:u8?]`

**Calib phases:** 0=IDLE, 1=CLEAR_RUN, 2=CLEAR_SETTLE, 3=DEPLOY_RUN, 4=MID_SETTLE, 5=RETRACT_RUN, 6=COMPLETE, 7=ERROR, 8=CANCELLED, 9=OPENING_DOORS, 10=CLOSING_DOORS

**Output:**
```
  ◆ {GearName} calib: {PHASE}, current={mA}mA, peak={mA}mA, stall={mA}mA, [FINISHED], reason={name}
```

---

## HubFX Status (module data, 6 bytes)

**Wire:** `[flags:u8][slaveMask:u8][loop1Count:u32LE]`

**Flags:** bit0=core1Ready, bit1=audioInit, bit2=flashReady, bit3=usbHostReady, bit4=sdCardReady
**Slave mask:** bit0=GunFX, bit1=LightFX, bit2=GearControl

**Output:**
```
  ── HubFX Status ──────────────
  Core 1:    Ready / NOT READY
             {count} iterations
  Audio:     Initialized / Not initialized
  Flash:     Ready / Not available
  SD Card:   Ready / Not available
  USB Host:  Active / Not active
  Slaves:
    GunFX: connected / not connected
    LightFX: connected / not connected
    GearControl: connected / not connected
```

### AUDIO_STATUS_RESP (v3/v4 extended)

**Wire:** `[masterVol:u8][flags:u8][sampleRate:u16LE][bitDepth:u8][maxChannels:u8][codecNameLen:u8][codecName:str][ringWriteIdx:u32LE][ringReadIdx:u32LE][ringCapacity:u32LE][bufferCapFrames:u16LE]`
Then per channel: `[vol:u8][playing:u8][looping:u8][remaining_ms:u16LE][queueSize:u8][output:u8]`
Extended (v4) per channel: `+[sampleRate:u16LE][bitDepth:u8][channels:u8][bufferFill:u16LE][filenameLen:u8][filename:str]`

**Output:**
```
  ── Audio Status ────────────────
  Master Volume: {vol}%
  Sample Rate:   {rate} Hz
  Bit Depth:     {bits}-bit
  Max Channels:  {n}
  Codec:         {name}
  Ring Buffer:   W={writeIdx} R={readIdx} cap={capacity} fill={fill}
  Buffer Cap:    {frames} frames ({ms:.1f}ms)
  ── Channels ───────────────────
  Ch {n}: vol={vol}% PLAYING looping remaining={s:.1f}s queue={q} out={outputName}
          WAV: {rate}Hz/{bits}bit/{channels}ch  buf={fill}/{total} ({pct}%)
          File: {filename}
```

### ENGINE_STATUS_RESP

**Wire (3 bytes):** `[state:u8][toggle:u8][active:u8]`

**Engine states:** 0=Stopped, 1=Starting, 2=Running, 3=Stopping

**Output:**
```
  Engine:    {state}                            ← icon: ■/▶/⏸/⏹
  Toggle:    {on/off}
  Active:    {yes/no}
```

### CONFIG_STATUS_RESP

**Wire (4 bytes):** `[loaded:u8][size:u16LE][valid:u8]`

**Output:**
```
  Config loaded: Yes/No
  Config size:   {size} bytes
  Config valid:  Yes/No
```

### SD_STATUS_RESP

**Wire (18 bytes):** `[init:u8][cardSize:u32LE][total:u32LE][free:u32LE][fatType:u8][cardType:u8][busMode:u8][used:u32LE]`

**Card types:** 0=Unknown, 1=SD, 2=SDHC, 3=SDXC
**Bus modes:** 0=SPI, 1=1-bit, 2=4-bit
**FAT types:** values represent FAT type (12, 16, 32, or exFAT=64)

**Output:**
```
  SD initialized: Yes/No
  Card:   {type} ({bus_mode})
  FAT:    FAT{n}
  Size:   {size}
  Total:  {total}
  Used:   {used}
  Free:   {free}
```

### FLASH_STATUS_RESP

**Wire (13 bytes):** `[init:u8][total:u32LE][used:u32LE][free:u32LE]`

**Output:**
```
  Flash:   {init ? "Ready" : "Not initialized"}
  Total:   {size}
  Used:    {used}
  Free:    {free}
```

### FILE_INFO_RESP

**Wire (6 bytes):** `[exists:u8][isDir:u8][size:u32LE]`

**Output:**
```
  Exists:  Yes/No
  Type:    Directory/File
  Size:    {size} bytes
```

### USB_DEVICES_RESP

**Wire:** `[init:u8][taskRunning:u8][backendLen:u8][backend:str][count:u8]`
Per device: `[addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]`

**Device states:** 0=ATTACHED, 1=CONFIGURED, 2=IDENTIFIED, 3=READY, 4=FAILED

**Output:**
```
  USB Host:    {init ? "Initialized" : "Not initialized"}
  Task:        {running ? "Running" : "Stopped"}
  Backend:     {name}
  Devices:     {count}
    [{addr}] VID={vid:04X} PID={pid:04X} {state_name} → {slave_type_name}
```

### CODEC_STATUS_RESP

**Wire:** `[codecType:u8][init:u8][i2cOK:u8][sda:u8][scl:u8][supply:u8][muted:u8][digitalVol:u8][deviceCtrl:u8][fault:u8][nameLen:u8][name:str]`

**Codec types:** 0=None, 1=TAS5825M, 2=SimpleI2S

**Output:**
```
  ── Codec Status ──────────────
  Codec:       {name} ({type_name})
  Initialized: Yes/No
  I2C:         {ok ? "OK" : "FAIL"} (SDA={sda}, SCL={scl})
  Supply:      {supply ? "OK" : "OFF"}
  Muted:       Yes/No
  Volume:      {digitalVol} (raw)
  Device Ctrl: 0x{ctrl:02X}
  Fault:       0x{fault:02X}
```

### SLAVE_INFO_RESP

Same wire format as INIT_READY. Output identical to INIT_READY.

---

## Command Naming Conventions

Commands MUST use consistent naming across all three CLI platforms:

| Python | Go | C# | Notes |
|--------|----|----|-------|
| `gc.yaw` | `yaw <position_us>` | `yaw <position_us>` | NOT `yaw.input` |
| `gc.calibrate.cancel` | `calibrate.cancel` | `calibrate.cancel` | NOT `calib.cancel` |
| `gc.battery` | `battery <enable> <auto>` | `battery <enable> <auto>` | NOT `battery.config` |

---

## Async Packet Handling

All three platforms handle these async packet types:

| Packet Type | Description | All Platforms |
|-------------|-------------|---------------|
| `LOG_MESSAGE` (0xFD) | Diagnostic log | Rich format |
| `GEAR_SEQ_STATUS` (0x70) | Gear sequence progress | Rich format |
| `GEAR_DOOR_STATUS` (0x72) | Door position update | Rich format |
| `GEAR_CALIB_STATUS` (0x6B) | Calibration progress | Rich format |
| `LANDING_LIGHT_STATUS` (0x56) | Landing light progress | Rich format |
| `ACK` (0xF6) | Keepalive responses | Ignored |
| `NACK` (0xF7) | Async errors | Error message |

---

## Size Formatting

For storage sizes (SD, Flash, file sizes), use human-readable formatting:

| Size Range | Format | Example |
|------------|--------|---------|
| < 1024 | `{n} B` | `512 B` |
| < 1 MB | `{n/1024:.1f} KB` | `45.3 KB` |
| < 1 GB | `{n/1048576:.1f} MB` | `234.7 MB` |
| >= 1 GB | `{n/1073741824:.2f} GB` | `3.72 GB` |
