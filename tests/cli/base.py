"""
ScaleFX CLI Base Classes and Utilities

Shared infrastructure for the interactive CLI:
- CommandInfo dataclass for command metadata
- Output formatting helpers with colorama
- Controller type constants
"""

import sys
import threading
import time
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
        RED = GREEN = YELLOW = CYAN = MAGENTA = BLUE = LIGHTBLACK_EX = RESET = ""
    class Style:  # type: ignore
        BRIGHT = DIM = RESET_ALL = ""


# =============================================================================
# Controller Types
# =============================================================================

class ControllerType:
    """Controller type constants."""
    GEARCONTROL = 'gearcontrol'
    GUNFX = 'gunfx'
    HUBFX = 'hubfx'
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
    group: Optional[str] = None  # Sub-group for help display (e.g., 'Audio', 'GunFX Slave')


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


class Spinner:
    """
    Context manager showing an animated spinner during long-running operations.

    Usage:
        with Spinner("Remounting SD card..."):
            result = some_blocking_call()

        with Spinner("Initializing...") as s:
            # Can update the message mid-operation
            s.update("Still working...")
    """
    FRAMES = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'

    def __init__(self, message: str = "Working..."):
        self._message = message
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def update(self, message: str):
        """Update the spinner message while running."""
        self._message = message

    def __enter__(self):
        self._running = True
        self._thread = threading.Thread(target=self._spin, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *args):
        self._running = False
        if self._thread:
            self._thread.join(timeout=1.0)
        # Clear the spinner line
        print(f"\r{' ' * 80}\r", end='', flush=True)

    def _spin(self):
        i = 0
        while self._running:
            frame = self.FRAMES[i % len(self.FRAMES)]
            print(f"\r    {frame} {self._message}", end='', flush=True)
            time.sleep(0.08)
            i += 1


def format_progress_bar(current: int, total: int, width: int = 30,
                        start_time: float = 0) -> str:
    """
    Build a progress bar string for display.

    Args:
        current:    Bytes (or units) completed so far
        total:      Total bytes (or units) expected
        width:      Character width of the bar
        start_time: time.time() of the operation start (for speed/ETA)

    Returns:
        Formatted progress bar string (no trailing newline)
    """
    if total <= 0:
        return ""
    pct = min(100, (current * 100) // total)
    filled = (current * width) // total
    bar = '█' * filled + '░' * (width - filled)
    size_str = f"{current}/{total}"

    speed_str = "-- KB/s"
    eta_str = ""
    if start_time > 0:
        elapsed = time.time() - start_time
        if elapsed > 0 and current > 0:
            speed = current / elapsed
            speed_str = f"{speed / 1024:.1f} KB/s"
            remaining = total - current
            if speed > 0:
                eta = remaining / speed
                eta_str = f"ETA {eta:.0f}s"

    return f"    [{bar}] {pct:3d}% {size_str} {speed_str} {eta_str}"


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
    
    Packet wrapper support: Set ``_packet_wrapper`` to a callable that
    transforms outgoing packets (e.g., ``HubFxCommands.slave_route``) to
    enable transparent hub routing of direct handler commands.
    """
    
    def __init__(self):
        self.conn: Optional['ScaleFXConnection'] = None
        self.controller_type: Optional[str] = None
        self._cancel_event: Optional[threading.Event] = None
        self._packet_wrapper: Optional[Callable] = None
    
    def set_connection(self, conn: Optional['ScaleFXConnection']):
        """Update connection reference."""
        self.conn = conn
    
    def set_controller_type(self, ctrl_type: Optional[str]):
        """Update controller type."""
        self.controller_type = ctrl_type
    
    def set_cancel_event(self, event: threading.Event):
        """Set the cancel event (shared with UI for Ctrl+C signalling)."""
        self._cancel_event = event

    @property
    def cancel_requested(self) -> bool:
        """Check if the user has requested cancellation via Ctrl+C."""
        return self._cancel_event is not None and self._cancel_event.is_set()
    
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

    # =========================================================================
    # Packet send helpers
    # =========================================================================

    def _wrap_packet(self, packet: bytes) -> bytes:
        """Apply packet wrapper if set (e.g., hub slave routing)."""
        if self._packet_wrapper:
            return self._packet_wrapper(packet)
        return packet

    def _send_ack(self, packet: bytes, ok_msg: str, timeout: float = None) -> bool:
        """Send a command packet, print ACK/NACK result.

        Applies ``_packet_wrapper`` before sending (no-op when unset).
        Returns True if the command was acknowledged.
        """
        wrapped = self._wrap_packet(packet)
        if timeout is not None:
            success, response = self.conn.send_expect_ack(wrapped, timeout=timeout)
        else:
            success, response = self.conn.send_expect_ack(wrapped)
        if success:
            self.print_ok(ok_msg)
        else:
            self._print_ack_response(response)
        return success

    def _print_ack_response(self, response):
        """Print NACK or timeout error for a failed command."""
        from . import parsers
        if response is None:
            self.print_error("No response (timeout)")
        elif response.is_nack:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))


# =============================================================================
# Prompt Generator
# =============================================================================

def get_prompt(controller_type: Optional[str], is_connected: bool) -> str:
    """Generate dynamic prompt based on connection state."""
    if controller_type:
        prefix = {
            ControllerType.GEARCONTROL: f"{Fore.GREEN}gearcontrol",
            ControllerType.GUNFX: f"{Fore.RED}gunfx",
            ControllerType.HUBFX: f"{Fore.CYAN}hubfx",
            ControllerType.LIGHTFX: f"{Fore.BLUE}lightfx",
            ControllerType.NOOP: f"{Fore.MAGENTA}noop",
        }.get(controller_type, f"{Fore.CYAN}scalefx")
        return f"{prefix}>{Style.RESET_ALL} "
    elif is_connected:
        return f"{Fore.YELLOW}connected>{Style.RESET_ALL} "
    return f"{Fore.CYAN}scalefx>{Style.RESET_ALL} "
