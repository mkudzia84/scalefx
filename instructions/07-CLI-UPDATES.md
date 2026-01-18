# CLI Update Guide

> **ACTION DOCUMENT:** How to add commands and controllers to the interactive CLI.

---

## CLI Architecture

```yaml
Command_Registries:
  core_commands:
    availability: "Always"
    examples: ["help", "connect", "disconnect", "ports", "exit"]
  
  protocol_commands:
    availability: "When connected"
    examples: ["init", "shutdown", "status", "keepalive", "reboot", "bootsel"]
  
  gunfx_commands:
    availability: "When connected AND controller_type == CTRL_GUNFX"
    examples: ["gunfx.trigger", "gunfx.servo", "gunfx.smoke"]
  
  lightfx_commands:
    availability: "When connected AND controller_type == CTRL_LIGHTFX"
    examples: ["lightfx.led", "lightfx.servo", "lightfx.power"]

Dynamic_Detection:
  trigger: "INIT_READY response received"
  method: "Parse device name, set controller_type"
  result: "Controller-specific commands appear in help"
```

---

## File Location

```
tests/cli/interactive.py
```

---

## Adding Command to Existing Controller

### Step 1: Add to Command Registry

**FIND:** `_build_command_registry()` method

**ADD to appropriate registry:**

```python
# For GunFX command
self.gunfx_commands: Dict[str, Tuple[Callable, CommandInfo]] = {
    # ...existing commands...
    
    'gunfx.newcmd': (self.cmd_gunfx_newcmd, CommandInfo(
        'gunfx.newcmd',                    # name (matches dict key)
        'gunfx.newcmd <param1> [param2]',  # usage (shown in help)
        'Description of what command does', # description
        requires_init=True,                 # needs init first?
        controller=self.CTRL_GUNFX         # controller type
    )),
}
```

### Step 2: Add Handler Method

**ADD method to class:**

```python
def cmd_gunfx_newcmd(self, args: List[str]):
    """GunFX new command handler."""
    # Validate arguments
    if len(args) < 1:
        self.print_error("Usage: gunfx.newcmd <param1> [param2]")
        return
    
    try:
        # Parse arguments
        param1 = int(args[0])
        param2 = int(args[1]) if len(args) > 1 else 0  # optional
        
        # Validate ranges
        if param1 < 0 or param1 > 100:
            self.print_error("param1 must be 0-100")
            return
        
        # Build and send packet
        packet = GunFxCommands.new_command(param1, param2)
        success, response = self.conn.send_expect_ack(packet)
        
        # Report result
        if success:
            self.print_success(f"Command executed: param1={param1}")
        else:
            self.print_response(response)
            
    except ValueError:
        self.print_error("Invalid parameter value")
```

### Step 3: Add Import (if needed)

**FIND:** Import section at top of file

**ADD if not present:**

```python
from tests.framework.commands import GunFxCommands  # if not imported
from tests.framework.packets import GunFxPacket, GunFxError  # if needed
```

---

## Adding New Controller Type

### Step 1: Add Controller Constant

**FIND:**
```python
class InteractiveCLI:
    CTRL_GUNFX = 'gunfx'
    CTRL_LIGHTFX = 'lightfx'
    CTRL_NOOP = 'noop'
```

**ADD:**
```python
    CTRL_NEWFX = 'newfx'
```

### Step 2: Create Command Registry

**FIND:** `_build_command_registry()` method

**ADD after existing registries:**

```python
# NewFX-specific commands
self.newfx_commands: Dict[str, Tuple[Callable, CommandInfo]] = {
    'newfx.cmd1': (self.cmd_newfx_cmd1, CommandInfo(
        'newfx.cmd1', 'newfx.cmd1 <param>',
        'Execute command 1',
        requires_init=True, controller=self.CTRL_NEWFX)),
    
    'newfx.cmd2': (self.cmd_newfx_cmd2, CommandInfo(
        'newfx.cmd2', 'newfx.cmd2 <id>',
        'Execute command 2',
        requires_init=True, controller=self.CTRL_NEWFX)),
}
```

### Step 3: Update get_available_commands()

**FIND:**
```python
def get_available_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
    # ...
    if self.controller_type == self.CTRL_GUNFX:
        commands.update(self.gunfx_commands)
    elif self.controller_type == self.CTRL_LIGHTFX:
        commands.update(self.lightfx_commands)
```

**ADD:**
```python
    elif self.controller_type == self.CTRL_NEWFX:
        commands.update(self.newfx_commands)
```

### Step 4: Update Controller Detection

**FIND:** `_parse_init_ready()` method

**ADD detection logic:**
```python
name_lower = device_name.lower()
if 'gunfx' in name_lower or 'gun' in name_lower:
    self.controller_type = self.CTRL_GUNFX
    self.print_info("Detected GunFX - gunfx.* commands available")
elif 'lightfx' in name_lower or 'light' in name_lower:
    self.controller_type = self.CTRL_LIGHTFX
    self.print_info("Detected LightFX - lightfx.* commands available")
elif 'newfx' in name_lower:  # ADD THIS
    self.controller_type = self.CTRL_NEWFX
    self.print_info("Detected NewFX - newfx.* commands available")
elif 'noop' in name_lower:
    self.controller_type = self.CTRL_NOOP
```

### Step 5: Update Prompt (Optional)

**FIND:** `prompt` property

**ADD color for new controller:**
```python
@property
def prompt(self) -> str:
    if self.controller_type:
        prefix = {
            self.CTRL_GUNFX: f"{Fore.RED}gunfx",
            self.CTRL_LIGHTFX: f"{Fore.BLUE}lightfx",
            self.CTRL_NOOP: f"{Fore.MAGENTA}noop",
            self.CTRL_NEWFX: f"{Fore.GREEN}newfx",  # ADD
        }.get(self.controller_type, f"{Fore.CYAN}scalefx")
```

### Step 6: Add Handler Methods

```python
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
            self.print_success(f"Command 2 executed: id={id_val}")
        else:
            self.print_response(response)
    except ValueError:
        self.print_error("Invalid id")
```

### Step 7: Add Imports

```python
from tests.framework.commands import NewFxCommands
from tests.framework.packets import NewFxPacket, NewFxError
```

---

## CommandInfo Structure

```python
@dataclass
class CommandInfo:
    name: str           # Command name (e.g., 'gunfx.trigger')
    usage: str          # Usage string for help
    description: str    # One-line description
    requires_init: bool = False      # Must INIT before using
    controller: str = None           # Controller type filter
```

---

## Handler Method Pattern

```python
def cmd_controller_feature(self, args: List[str]):
    """Controller feature description."""
    
    # 1. Validate argument count
    if len(args) < REQUIRED_ARGS:
        self.print_error("Usage: controller.feature <arg1> <arg2>")
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
    
    # 4. Build packet
    packet = XxxCommands.feature(value)
    
    # 5. Send and handle response
    success, response = self.conn.send_expect_ack(packet)
    
    if success:
        self.print_success(f"Feature set to {value}")
    else:
        self.print_response(response)
```

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
# Syntax check
python -m py_compile tests/cli/interactive.py

# Run CLI
python -m tests.cli.interactive

# Test commands
> connect
> init
> help                    # Verify new commands appear
> newfx.cmd1 100         # Test new command
```

---

## Checklist

```yaml
Adding_Command:
  - "[ ] CommandInfo added to correct registry dict"
  - "[ ] Handler method added with cmd_ prefix"
  - "[ ] Argument validation in handler"
  - "[ ] Error handling with try/except"
  - "[ ] Imports added if needed"
  - "[ ] python -m py_compile passes"
  - "[ ] Command appears in 'help'"
  - "[ ] Command executes correctly"

Adding_Controller:
  - "[ ] CTRL_NEWFX constant added"
  - "[ ] Command registry created"
  - "[ ] get_available_commands() updated"
  - "[ ] _parse_init_ready() detection added"
  - "[ ] Prompt color added (optional)"
  - "[ ] Handler methods added"
  - "[ ] Imports added"
  - "[ ] python -m py_compile passes"
  - "[ ] Commands appear after init with controller"
```
