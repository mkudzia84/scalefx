"""
ScaleFX CLI Base Classes and Utilities

Shared infrastructure for the interactive CLI:
- CommandInfo dataclass for command metadata
- Output formatting helpers with colorama
- Controller type constants
"""

from dataclasses import dataclass
from typing import Optional, Callable, List, TYPE_CHECKING

# Colorama support with fallback
try:
    from colorama import init, Fore, Style
    init()
    HAS_COLOR = True
except ImportError:
    HAS_COLOR = False
    class Fore:  # type: ignore
        RED = GREEN = YELLOW = CYAN = MAGENTA = BLUE = RESET = ""
    class Style:  # type: ignore
        BRIGHT = RESET_ALL = ""


# =============================================================================
# Controller Types
# =============================================================================

class ControllerType:
    """Controller type constants."""
    GEARCONTROL = 'gearcontrol'
    GUNFX = 'gunfx'
    LIGHTFX = 'lightfx'
    NOOP = 'noop'


# =============================================================================
# Command Metadata
# =============================================================================

@dataclass
class CommandInfo:
    """Command metadata for help display and validation."""
    name: str
    usage: str
    description: str
    requires_init: bool = False
    controller: Optional[str] = None  # None = all, 'gunfx', 'lightfx', 'noop'


# =============================================================================
# Output Formatting
# =============================================================================

class OutputMixin:
    """Mixin providing formatted output methods."""
    
    def print_ok(self, msg: str):
        """Print success message with green checkmark."""
        print(f"{Fore.GREEN}✓{Style.RESET_ALL} {msg}")
    
    def print_error(self, msg: str):
        """Print error message with red X."""
        print(f"{Fore.RED}✗{Style.RESET_ALL} {msg}")
    
    def print_info(self, msg: str):
        """Print info message with yellow icon."""
        print(f"{Fore.YELLOW}ℹ{Style.RESET_ALL} {msg}")
    
    def print_warning(self, msg: str):
        """Print warning message."""
        print(f"{Fore.YELLOW}⚠{Style.RESET_ALL} {msg}")


# =============================================================================
# Command Handler Protocol
# =============================================================================

if TYPE_CHECKING:
    from tests.framework import ScaleFXConnection

class CommandHandlerBase(OutputMixin):
    """
    Base class for command handler groups.
    
    Subclasses implement commands for specific controllers (GunFX, LightFX)
    or command categories (core, protocol).
    """
    
    def __init__(self):
        self.conn: Optional['ScaleFXConnection'] = None
        self.controller_type: Optional[str] = None
    
    def set_connection(self, conn: Optional['ScaleFXConnection']):
        """Update connection reference."""
        self.conn = conn
    
    def set_controller_type(self, ctrl_type: Optional[str]):
        """Update controller type."""
        self.controller_type = ctrl_type
    
    def _require_connection(self) -> bool:
        """Check if connected, print error if not."""
        if not self.conn or not self.conn.is_connected:
            self.print_error("Not connected. Use 'connect' first.")
            return False
        return True
    
    def _require_init(self) -> bool:
        """Check if initialized, print error if not."""
        if not self._require_connection():
            return False
        if not self.conn.is_initialized:
            self.print_error("Not initialized. Use 'init' first.")
            return False
        return True
    
    def get_commands(self) -> dict:
        """
        Return dict of command_name -> (handler_method, CommandInfo).
        Override in subclasses.
        """
        return {}


# =============================================================================
# Prompt Generator
# =============================================================================

def get_prompt(controller_type: Optional[str], is_connected: bool) -> str:
    """Generate dynamic prompt based on connection state."""
    if controller_type:
        prefix = {
            ControllerType.GEARCONTROL: f"{Fore.GREEN}gearcontrol",
            ControllerType.GUNFX: f"{Fore.RED}gunfx",
            ControllerType.LIGHTFX: f"{Fore.BLUE}lightfx",
            ControllerType.NOOP: f"{Fore.MAGENTA}noop",
        }.get(controller_type, f"{Fore.CYAN}scalefx")
        return f"{prefix}>{Style.RESET_ALL} "
    elif is_connected:
        return f"{Fore.YELLOW}connected>{Style.RESET_ALL} "
    return f"{Fore.CYAN}scalefx>{Style.RESET_ALL} "
