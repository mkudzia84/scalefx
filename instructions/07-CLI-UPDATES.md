# CLI Update Guide

> **ACTION DOCUMENT:** How to add commands and controllers to the interactive CLI.

---

## CLI Architecture

The CLI uses a modular handler architecture with **composition-based slave routing**. Each controller has its own handler file. The HubFX handler reuses direct handler code for slave commands by composing handler instances with a packet wrapper.

### Async Output Handling

The CLI uses a split-screen terminal UI (`output.py`) built with `prompt_toolkit`:
- **Output area** (top): Scrolling pane showing all command results, log messages, and status updates
- **Input area** (bottom): Fixed single-line prompt that is never overwritten by async output
- **Separator**: Horizontal line between output and input areas
- All `print()` calls from any thread are captured via `sys.stdout` replacement and displayed in the output area
- The output pane auto-scrolls to show the latest content
- Command history is available via up/down arrow keys
- Commands run in a background thread so the UI stays responsive

**Key dependency:** `prompt_toolkit>=3.0.0` (in `tests/requirements.txt`)

### Composition Pattern for Slave Routing

The HubFX handler composes instances of direct handlers (GunFX, LightFX, GearControl) with `_packet_wrapper` set to `HubFxCommands.slave_route`. This transparently wraps direct command packets in SLAVE_ROUTE envelopes for hub routing, eliminating code duplication.

```
Direct use:     GunFxCommandHandler._send_ack(packet)  →  conn.send_expect_ack(packet)
Hub slave use:  GunFxCommandHandler._send_ack(packet)  →  conn.send_expect_ack(slave_route(packet))
```

The `_build_slave_registry()` auto-discovers commands from composed handlers' `get_commands()`, prefixing usage strings with `slave `. Query commands that require custom response packets (not ACK-based) are excluded since SLAVE_ROUTE only forwards ACK/NACK.

```yaml
File_Structure:
  "tests/cli/output.py": "TerminalUI - split-screen terminal with prompt_toolkit Application"
  "tests/cli/base.py": "CommandInfo, OutputMixin, ControllerType, CommandHandlerBase (with _send_ack, _wrap_packet)"
  "tests/cli/parsers.py": "Response packet parsing utilities"
  "tests/cli/interactive.py": "Main CLI class (~350 lines, composes handlers + TerminalUI)"
  "tests/cli/handlers/core.py": "Core/protocol commands (connect, init, status, reboot)"
  "tests/cli/handlers/gunfx.py": "GunFX commands (trigger, servo, smoke)"
  "tests/cli/handlers/lightfx.py": "LightFX commands (led, servo, power, LED sequences)"
  "tests/cli/handlers/gearcontrol.py": "GearControl commands (gear, servo, yaw, calibration)"
  "tests/cli/handlers/hubfx.py": "HubFX hub commands + composed slave routing"
  "tests/cli/handlers/storage.py": "Reusable file operations (SD/Flash) parameterized by target"

Handler_Pattern:
  base_class: "CommandHandlerBase (from base.py)"
  registration: "Handlers registered in interactive.py constructor"
  command_routing: "Each handler returns dict of {name: (method, CommandInfo)}"
  controller_filtering: "Commands filtered by ControllerType after INIT_READY"
  packet_wrapper: "Optional callable on CommandHandlerBase that wraps packets before sending"
  send_helper: "_send_ack(packet, ok_msg, timeout=None) — unified send+wrap+error handling"

Command_Categories:
  core_commands:
    handler: "handlers/core.py"
    availability: "Always available"
    examples: ["help", "connect", "disconnect", "ports", "exit"]
  
  protocol_commands:
    handler: "handlers/core.py"
    availability: "When connected"
    examples: ["init", "shutdown", "status", "keepalive", "reboot", "bootsel"]
  
  gunfx_commands:
    handler: "handlers/gunfx.py"
    availability: "When connected AND controller detected as GunFX (direct) or HubFX (via slave)"
    examples: ["gfx.trigger", "gfx.servo", "gfx.smoke"]

  lightfx_commands:
    handler: "handlers/lightfx.py"
    availability: "When connected AND controller detected as LightFX (direct) or HubFX (via slave)"
    examples: ["lfx.led", "lfx.servo", "lfx.brightness"]

  gearcontrol_commands:
    handler: "handlers/gearcontrol.py"
    availability: "When connected AND controller detected as GearControl (direct) or HubFX (via slave)"
    examples: ["gc.deploy", "gc.retract", "gc.calibrate"]
  
  hubfx_commands:
    handler: "handlers/hubfx.py"
    availability: "When connected AND controller detected as HubFX"
    examples: ["audio.play", "engine.start", "slave gfx.trigger on 100"]

Dynamic_Detection:
  trigger: "INIT_READY response received"
  location: "handlers/core.py"
  method: "Parse device name, set ControllerType"
  result: "Controller-specific handler's commands appear in help"
```

---

## File Locations

```yaml
Main:
  "tests/cli/interactive.py": "Main CLI class, handler composition"
  "tests/cli/base.py": "Base classes, CommandInfo, OutputMixin, ControllerType, _send_ack, _wrap_packet"
  "tests/cli/parsers.py": "Response packet parsing"

Handlers:
  "tests/cli/handlers/core.py": "Core and protocol commands"
  "tests/cli/handlers/gunfx.py": "GunFX controller commands (8 commands)"
  "tests/cli/handlers/lightfx.py": "LightFX controller commands (16 commands)"
  "tests/cli/handlers/gearcontrol.py": "GearControl controller commands (16 commands)"
  "tests/cli/handlers/hubfx.py": "HubFX hub commands + slave routing via composition"
  "tests/cli/handlers/storage.py": "Reusable file operations (SD/Flash)"
```

---

## Adding Command to Existing Controller

### Step 1: Add Handler Method to Handler Class

**FILE:** `tests/cli/handlers/gunfx.py` (or `lightfx.py`, `gearcontrol.py`)

```python
def cmd_newcmd(self, args: List[str]):
    """GunFX new command handler."""
    if len(args) < 1:
        self.print_error("Usage: gfx.newcmd <param1> [param2]")
        return
    
    try:
        param1 = int(args[0])
        param2 = int(args[1]) if len(args) > 1 else 0
        
        if param1 < 0 or param1 > 100:
            self.print_error("param1 must be 0-100")
            return
        
        # _send_ack handles: packet wrapping, send, ACK/NACK response printing
        self._send_ack(
            GunFxCommands.new_command(param1, param2),
            f"Command executed: param1={param1}")
            
    except ValueError:
        self.print_error("Invalid parameter value")
```

**NOTE:** Using `_send_ack()` instead of manual `send_expect_ack()` + response handling ensures
the command works transparently both as a direct handler AND when composed for hub slave routing.
For query commands that need custom response parsing, use `conn.send_expect_ack()` directly.
```

### Step 2: Register in get_commands()

**FIND:** `get_commands()` method in the handler class

**ADD:**

```python
def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
    return {
        # ...existing commands...
        
        'gfx.newcmd': (self.cmd_newcmd, CommandInfo(
            'gfx.newcmd',                      # name
            'gfx.newcmd <param1> [param2]',    # usage
            'Description of what command does', # description
            requires_init=True)),              # needs init?
    }
```

### Step 3: Add Import (if needed)

```python
from tests.framework.commands import GunFxCommands
from tests.framework.packets import GunFxPacket, GunFxError
```

---

## Adding New Controller Type

### Step 1: Create Handler File

**CREATE:** `tests/cli/handlers/newfx.py`

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
                'newfx.cmd1', 'newfx.cmd1 <param>',
                'Execute command 1',
                requires_init=True)),
            
            'newfx.cmd2': (self.cmd_newfx_cmd2, CommandInfo(
                'newfx.cmd2', 'newfx.cmd2 <id>',
                'Execute command 2',
                requires_init=True)),
        }
    
    def cmd_newfx_cmd1(self, args: List[str]):
        """NewFX command 1."""
        if len(args) < 1:
            self.print_error("Usage: newfx.cmd1 <param>")
            return
        try:
            param = int(args[0])
            packet = NewFxCommands.command_1(param)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_success(f"Command 1 executed: {param}")
            else:
                self.print_response(response)
        except ValueError:
            self.print_error("Invalid parameter")
    
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

### Step 2: Add ControllerType

**FILE:** `tests/cli/base.py`

```python
class ControllerType:
    GUNFX = 'gunfx'
    LIGHTFX = 'lightfx'
    NOOP = 'noop'
    NEWFX = 'newfx'    # ADD THIS
```

### Step 3: Register Handler in interactive.py

**FILE:** `tests/cli/interactive.py`

```python
from tests.cli.handlers.newfx import NewFxCommandHandler

# In constructor:
self.newfx_handler = NewFxCommandHandler(self.conn)

# In get_available_commands():
if self.controller_type == ControllerType.NEWFX:
    commands.update(self.newfx_handler.get_commands())
```

### Step 4: Add Controller Detection

**FILE:** `tests/cli/handlers/core.py`

```python
# In _parse_init_ready() or controller detection logic:
name_lower = device_name.lower()
if 'newfx' in name_lower:
    self.controller_type = ControllerType.NEWFX
    self.print_info("Detected NewFX - newfx.* commands available")
```

---

## CommandInfo Structure

```python
# Defined in tests/cli/base.py
@dataclass
class CommandInfo:
    name: str           # Command name (e.g., 'gunfx.trigger')
    usage: str          # Usage string for help
    description: str    # One-line description
    requires_init: bool = False  # Must INIT before using
```

## CommandHandlerBase

```python
# Defined in tests/cli/base.py
class CommandHandlerBase(OutputMixin):
    def __init__(self):
        self.conn = None                  # Set via set_connection()
        self.controller_type = None       # Set via set_controller_type()
        self._cancel_event = None         # Set via set_cancel_event()
        self._packet_wrapper = None       # Set for hub slave routing
    
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return dict of command_name -> (handler_method, CommandInfo)."""
        raise NotImplementedError

    # --- Packet send helpers ---
    
    def _wrap_packet(self, packet: bytes) -> bytes:
        """Apply _packet_wrapper if set, else return packet unchanged."""
    
    def _send_ack(self, packet: bytes, ok_msg: str, timeout: float = None) -> bool:
        """Send packet (with wrapping), print ACK ok_msg or NACK error. Returns success."""
    
    def _print_ack_response(self, response):
        """Print NACK error code/message from a failed command response."""
```

### Packet Wrapper (Slave Routing)

The `_packet_wrapper` attribute enables transparent hub routing. When set on a handler instance,
all `_send_ack()` calls wrap the outgoing packet before sending:

```python
# In HubFx handler init:
self._gunfx = GunFxCommandHandler()
self._gunfx._packet_wrapper = HubFxCommands.slave_route  # Wraps in SLAVE_ROUTE_GUNFX

# Now calling self._gunfx.cmd_trigger(["on", "100"]) sends:
#   SLAVE_ROUTE_GUNFX envelope containing TRIGGER_ON packet
# instead of raw TRIGGER_ON packet
```

**Rules:**
1. ACK-based commands work transparently with `_send_ack()` + wrapper
2. Query commands that need custom response parsing CANNOT be routed — exclude them from slave registry
3. Commands that use `conn.send_expect_ack()` directly are NOT wrapped — only `_send_ack()` applies the wrapper

---

## Handler Method Pattern

```python
def cmd_feature(self, args: List[str]):
    """Controller feature description."""
    
    # 1. Validate argument count
    if len(args) < REQUIRED_ARGS:
        self.print_error("Usage: prefix.feature <arg1> <arg2>")
        return
    
    # 2. Handle subcommands (if any)
    if args and args[0].lower() in ('set', 'get', 'on', 'off'):
        subcmd = args[0].lower()
        args = args[1:]
    
    # 3. Parse and validate arguments
    try:
        value = int(args[0])
        if value < MIN or value > MAX:
            self.print_error(f"Value must be {MIN}-{MAX}")
            return
    except ValueError:
        self.print_error("Invalid numeric value")
        return
    except IndexError:
        self.print_error("Missing required argument")
        return
    
    # 4. Send and handle response (unified pattern)
    self._send_ack(
        XxxCommands.feature(value),
        f"Feature set to {value}")
```

**For query commands** that need custom response parsing (not ACK-based):

```python
def cmd_query(self, args: List[str]):
    """Query command that returns data (not just ACK)."""
    packet = XxxCommands.query_something()
    success, response = self.conn.send_expect_ack(packet)
    if success:
        # Parse response payload
        data = parse_query_response(response)
        self.print_info(f"Result: {data}")
    else:
        self._print_ack_response(response)
```

**NOTE:** Query commands that use `conn.send_expect_ack()` directly are NOT wrapped by
`_packet_wrapper`, so they cannot be routed through slave. Exclude them from slave registries.

---

## Output Methods

```python
# Success message (green)
self.print_success("Operation completed")

# Error message (red)
self.print_error("Something went wrong")

# Info message (cyan)
self.print_info("Informational message")

# Warning message (yellow)
self.print_warning("Warning message")

# Raw response display
self.print_response(response)
```

---

## Verification

```bash
# Syntax check (all handler files)
python -m py_compile tests/cli/output.py
python -m py_compile tests/cli/base.py
python -m py_compile tests/cli/interactive.py
python -m py_compile tests/cli/handlers/core.py
python -m py_compile tests/cli/handlers/gunfx.py
python -m py_compile tests/cli/handlers/lightfx.py
python -m py_compile tests/cli/handlers/gearcontrol.py
python -m py_compile tests/cli/handlers/hubfx.py
python -m py_compile tests/cli/handlers/storage.py

# Run CLI
python -m tests.cli.interactive

# Test commands (direct)
> connect
> init
> help                    # Verify new commands appear
> gfx.trigger on 100     # Test GunFX command (when connected to GunFX)

# Test commands (hub slave routing)
> connect                 # Connect to HubFX
> init
> slave gfx.trigger on 100   # Routes via SLAVE_ROUTE envelope
> slave gc.deploy all         # Routes to GearControl via hub
```

---

## Checklist

```yaml
Adding_Command:
  - "[ ] Handler method added to handlers/xxxfx.py using _send_ack()"
  - "[ ] CommandInfo added to get_commands() dict"
  - "[ ] Argument validation in handler"
  - "[ ] Error handling with try/except"
  - "[ ] Imports added if needed"
  - "[ ] python -m py_compile passes on handler file"
  - "[ ] Command appears in 'help'"
  - "[ ] Command executes correctly"
  - "[ ] IF ACK-based: automatically available via hub slave routing"
  - "[ ] IF query: added to EXCLUDE set in hubfx.py _build_slave_registry()"

Adding_Controller:
  - "[ ] Handler file created: tests/cli/handlers/newfx.py"
  - "[ ] CommandHandlerBase subclass with get_commands()"
  - "[ ] All ACK commands use _send_ack() for hub routing compatibility"
  - "[ ] ControllerType constant added to base.py"
  - "[ ] Handler registered in interactive.py"
  - "[ ] Controller detection added to handlers/core.py"
  - "[ ] Handler methods implemented"
  - "[ ] Imports added"
  - "[ ] python -m py_compile passes on all files"
  - "[ ] Commands appear after init with controller"
  - "[ ] IF HubFX slave: compose handler in hubfx.py __init__ with _packet_wrapper"
```
