# ScaleFX CLI

Compiled interactive command-line client for ScaleFX controllers. Written in Go, produces a single static binary with zero runtime dependencies.

## Building

```bash
cd tools/cli
go build -o scalefx-cli.exe .
```

**Requirements:** Go 1.21+, single external dependency (`go.bug.st/serial`).

## Usage

```bash
# Connect to a specific port
scalefx-cli -p COM5

# Verbose protocol logging
scalefx-cli -p COM5 -v

# List available serial ports
scalefx-cli --list
```

If no port is specified, the CLI will list available ports and prompt for selection on `connect`.

## Architecture

```
tools/cli/
├── main.go              # Entry point, flag parsing
├── cli.go               # Interactive loop, command dispatch, help, listener
├── connection.go        # Serial connection, tag-correlated send/receive
├── protocol.go          # COBS encode/decode, CRC-8/CRC-16, packet build/parse
├── packets.go           # Packet type constants, error codes (mirrors C++ headers)
├── commands.go          # Command builders (mirrors tests/framework/commands.py)
├── parsers.go           # Response payload parsers (status, I2C, init_ready, etc.)
├── output.go            # ANSI colored output, help rendering
├── helpers.go           # Shared utilities (arg parsing, guards, servo patterns)
├── format_storage.go    # Storage-related output formatting
├── handler_core.go      # Core commands (connect, init, status, reboot, etc.)
├── handler_gunfx.go     # GunFX commands (trigger, servo, smoke)
├── handler_lightfx.go   # LightFX commands (LED, sequences, servo, landing lights)
├── handler_gearcontrol.go # GearControl commands (gear, servo, yaw, calibration)
└── handler_hubfx.go     # HubFX commands (slaves, audio, engine, storage, USB)
```

### Key Design Patterns

- **Tag-correlated send/receive**: Single persistent reader goroutine dispatches responses via channels keyed by correlation tag. Async/unsolicited packets go to a callback.
- **Command groups**: Commands organized by controller type. Only commands matching the detected controller (+ universal core commands) are shown in help.
- **Strategy pattern**: Shared patterns (servo set/config) factored into generic helpers with controller-specific builder functions as strategies.
- **Flat command namespace**: No prefixes — `trigger on 500` not `gfx.trigger on 500`.

### Protocol Layer

Implements the full ScaleFX binary COBS protocol:
- Packet format: `[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]`
- CRC-8 polynomial: 0x07
- COBS framing with 0x00 delimiter
- 6 Mbps baud rate
- Little-endian for all multi-byte values

### Sync Points

These Go files must stay in sync with their C++, Python, and C# counterparts:

| Go File | C++ Source | Python Source | C# Source |
|---------|-----------|---------------|------------|
| `packets.go` | `sfx_serial/serial/core/core.h`, `*/xxxfx.h` | `tests/framework/packets.py` | `ScaleFXSerial/PacketTypes.cs`, `ErrorCodes.cs` |
| `commands.go` | Client methods in `*/xxxfx.h` | `tests/framework/commands.py` | `ScaleFXSerial/Commands/*.cs` |
| `parsers.go` | Server response payloads in `*/xxxfx.h` | `tests/cli/parsers.py` | (Studio app layer) |

## Command Reference

### Core Commands (all controllers)

| Command | Description |
|---------|-------------|
| `connect [port] [baud]` | Connect to serial port |
| `disconnect` | Disconnect from port |
| `reconnect` | Disconnect and reconnect to same port |
| `ports` | List available serial ports |
| `init` | Send INIT to controller |
| `identify` | Identify controller (no state change) |
| `shutdown` | Send SHUTDOWN to controller |
| `status` | Request controller status |
| `reboot` | Reboot controller |
| `bootsel` | Enter BOOTSEL/DFU mode |
| `i2c.scan` | Scan I2C bus for devices |
| `diag [count]` | Request diagnostic log history |
| `keepalive` | Send keepalive ping |
| `verbose [on\|off]` | Toggle verbose mode |

### GunFX Commands

| Command | Description |
|---------|-------------|
| `trigger on <rpm>` | Start firing at RPM |
| `trigger off [delay_ms]` | Stop firing |
| `servo set <id> <pulse_us>` | Move servo |
| `servo.config <id> <min> <max> [spd] [acc] [dec]` | Configure servo |
| `servo.recoil <id> <jerk_us> <variance>` | Set recoil jerk |
| `smoke on\|off` | Toggle smoke heater |
| `smoke.config <pulsing> <speed> <high> <low> <pulse_ms> <spindown_ms>` | Configure smoke fan |
| `smoke.reset` | Reset smoke system |
| `smoke.limit <target> <limit_mA>` | Set current limit |

### LightFX Commands

| Command | Description |
|---------|-------------|
| `led <ch> <brightness>` | Set LED brightness (0-100) |
| `led.off <ch>` | Turn LED off |
| `led.status` | Query all LED channel status |
| `seq.add <ch> <type> [params...]` | Add sequence event (on/off/flash/fadein/fadeout/fading/beacon) |
| `seq.clear <ch>` | Clear sequence |
| `seq.start <ch> [loops]` | Start sequence |
| `seq.stop <ch>` | Stop sequence |
| `seq.restart <ch>` | Restart sequence |
| `seq.status <ch>` | Query sequence status |
| `seq.queue <ch>` | Query sequence event queue |
| `brightness <0-100>` | Master brightness |
| `servo set <id> <pulse_us>` | Move servo |
| `servo.config <id> <min> <max> [spd] [acc] [dec]` | Configure servo |
| `landing.bind <slot> <servo> <led> <deploy_us> <retract_us> <brightness>` | Bind landing light |
| `landing.unbind <slot>` | Unbind landing light |
| `landing.deploy <slot>` | Deploy landing light |
| `landing.retract <slot>` | Retract landing light |
| `reset <ch>` | Reset LED channel |
| `enable <ch>` | Enable LED channel |
| `disable <ch>` | Disable LED channel |

### GearControl Commands

| Command | Description |
|---------|-------------|
| `deploy <gear_id>` | Deploy gear |
| `retract <gear_id>` | Retract gear |
| `stop <gear_id>` | Stop gear |
| `all deploy\|retract` | All gears |
| `servo set <id> <pulse_us>` | Move servo |
| `servo.config <id> <min> <max> [spd] [acc] [dec]` | Configure servo |
| `gear.config <id> <flags> <stall_mA> <timeout_ms>` | Configure gear |
| `door.config <id> <open0> <close0> <open1> <close1>` | Configure door |
| `door.mode <id> <pre> <post> <delay_ms>` | Door animation mode |
| `yaw.config <id> <neutral> <min> <max>` | Configure yaw |
| `yaw.input <position_us>` | Set yaw input |
| `calibrate <gear_id> [timeout_s]` | Start calibration |
| `calib.cancel <gear_id>` | Cancel calibration |
| `battery.config <enable> <auto_deploy>` | Battery monitor |
| `reset <gear_id>` | Reset gear |
| `enable <gear_id>` | Enable gear |
| `disable <gear_id>` | Disable gear |

### HubFX Commands

| Command | Description |
|---------|-------------|
| `slaves` | List connected slaves |
| `slave.init <type>` | Init slave (gunfx\|lightfx\|gearcontrol) |
| `slave.info <type>` | Query slave info |
| `audio.play <ch> <path> [vol] [ch1\|ch2] [loop [N\|inf]]` | Play audio file |
| `audio.stop [ch\|all]` | Stop audio (default: all) |
| `audio.volume <ch\|master> <vol>` | Set volume (0-100) |
| `audio.fade <ch>` | Fade out audio |
| `audio.queue <ch> <path> [vol] [loop N]` | Queue sound after current |
| `audio.clear [ch\|all]` | Clear audio queue |
| `audio.status` | Audio mixer status |
| `codec.status` | DAC codec status |
| `engine.start` | Start engine effect |
| `engine.stop` | Stop engine effect |
| `engine.status` | Engine status |
| `config.reload [path]` | Reload config from SD |
| `config.status` | Config status |
| `config.save [path]` | Save config to SD |
| `sd.init` | Initialize SD card |
| `sd.status` | SD card status |
| `flash.status` | Flash status |
| `file.list <sd\|flash> [path]` | List files |
| `file.delete <sd\|flash> <path>` | Delete file |
| `file.mkdir <sd\|flash> <path>` | Create directory |
| `file.info <sd\|flash> <path>` | File info |
| `file.tree <sd\|flash> [path]` | Tree view |
| `usb.devices` | List USB devices |
| `usb.reset` | Reset USB bus |

## Comparison with Python CLI

| | Go CLI | Python CLI |
|-|--------|-----------|
| **Startup** | Instant (compiled binary) | ~2s (Python + prompt_toolkit) |
| **Distribution** | Single .exe, zero deps | Requires Python 3.10+, venv, pip install |
| **Size** | ~6 MB static binary | ~50 MB (Python + packages) |
| **Protocol** | Full COBS/CRC, tag correlation | Full COBS/CRC, tag correlation |
| **Commands** | Flat namespace, same coverage | Prefix-based (via handler composition) |
| **UI** | Simple readline, ANSI colors | prompt_toolkit split-screen |
| **Async display** | Callback-based, inline | Split pane output buffer |
