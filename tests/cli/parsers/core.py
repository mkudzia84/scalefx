"""
Core protocol parsers — packet type names, error names, status dispatcher,
INIT_READY parsing, I2C scan results, log messages.
"""

from typing import Optional
from tests.framework import (
    CorePacket, CoreError, GearControlError, GunFxError, LightFxError, HubFxError,
    GearControlPacket, GunFxPacket, LightFxPacket, HubFxPacket,
)
from tests.framework.protocol import read_u16_le, read_u32_le
from ..base import Fore, Style


# =============================================================================
# Packet Type Name Resolution
# =============================================================================

def packet_type_name(ptype: int) -> str:
    """Get human-readable name for packet type."""
    core_names = {
        CorePacket.INIT: "INIT",
        CorePacket.INIT_READY: "INIT_READY",
        CorePacket.ACK: "ACK",
        CorePacket.NACK: "NACK",
        CorePacket.STATUS: "STATUS",
        CorePacket.STATUS_REQ: "STATUS_REQ",
        CorePacket.ERROR: "ERROR",
        CorePacket.SHUTDOWN: "SHUTDOWN",
        CorePacket.REBOOT: "REBOOT",
        CorePacket.BOOTSEL: "BOOTSEL",
        CorePacket.KEEPALIVE: "KEEPALIVE",
        CorePacket.I2C_SCAN: "I2C_SCAN",
        CorePacket.I2C_SCAN_RESULT: "I2C_SCAN_RESULT",
        CorePacket.IDENTIFY: "IDENTIFY",
    }
    if ptype in core_names:
        return core_names[ptype]

    # Check GearControl packet types
    for name in dir(GearControlPacket):
        if not name.startswith('_'):
            val = getattr(GearControlPacket, name)
            if val == ptype:
                return f"GEARCONTROL.{name}"

    # Check GunFX packet types
    for name in dir(GunFxPacket):
        if not name.startswith('_'):
            val = getattr(GunFxPacket, name)
            if val == ptype:
                return f"GUNFX.{name}"

    # Check LightFX packet types
    for name in dir(LightFxPacket):
        if not name.startswith('_'):
            val = getattr(LightFxPacket, name)
            if val == ptype:
                return f"LIGHTFX.{name}"

    # Check HubFX packet types
    for name in dir(HubFxPacket):
        if not name.startswith('_'):
            val = getattr(HubFxPacket, name)
            if val == ptype:
                return f"HUBFX.{name}"

    return f"UNKNOWN (0x{ptype:02X})"


def error_name(code: int) -> str:
    """Get human-readable error name, checking all modules."""
    name = GearControlError.name(code)
    if "UNKNOWN" in name:
        name = GunFxError.name(code)
    if "UNKNOWN" in name:
        name = LightFxError.name(code)
    if "UNKNOWN" in name:
        name = HubFxError.name(code)
    if "UNKNOWN" in name:
        name = CoreError.name(code)
    return name


# =============================================================================
# Generic Payload Parsers
# =============================================================================

def parse_generic_payload(payload: bytes) -> None:
    """Parse unknown payload in most readable format possible."""
    if len(payload) == 0:
        return

    # Try to detect structure
    if len(payload) == 1:
        print(f"  Value: {payload[0]} (0x{payload[0]:02X})")
    elif len(payload) == 2:
        val = read_u16_le(payload, 0)
        print(f"  Value (u16): {val} (0x{val:04X})")
    elif len(payload) == 4:
        val = read_u32_le(payload, 0)
        print(f"  Value (u32): {val} (0x{val:08X})")
    else:
        # Check if printable ASCII
        try:
            text = payload.decode('ascii')
            if text.isprintable():
                print(f"  Text: \"{text}\"")
                return
        except:
            pass

        # Show hex dump with byte values
        hex_str = ' '.join(f'{b:02X}' for b in payload)
        print(f"  Hex ({len(payload)} bytes): {hex_str}")


def parse_error_payload(payload: bytes) -> None:
    """Parse ERROR packet payload."""
    if len(payload) == 0:
        print("  (no details)")
        return
    code = payload[0]
    name = error_name(code)
    msg = payload[1:].decode('utf-8', errors='replace') if len(payload) > 1 else ""
    print(f"  Error code: {name} (0x{code:02X})")
    if msg:
        print(f"  Message: {msg}")


# =============================================================================
# Log Message Parser
# =============================================================================

_LOG_LEVEL_NAMES = {0: 'DEBUG', 1: 'INFO', 2: 'WARN', 3: 'ERROR'}
_LOG_LEVEL_COLORS = {
    0: Style.RESET_ALL,    # DEBUG: default
    1: Fore.CYAN,          # INFO:  cyan
    2: Fore.YELLOW,        # WARN:  yellow
    3: Fore.RED,           # ERROR: red
}


def parse_log_message(payload: bytes) -> None:
    """Parse LOG_MESSAGE packet payload.

    Wire format: [level:u8][millis:u32LE][message:str]
    """
    if len(payload) < 5:
        print(f"  Log: (incomplete: {payload.hex()})")
        return

    level = payload[0]
    timestamp_ms = read_u32_le(payload, 1)
    message = payload[5:].decode('utf-8', errors='replace')

    level_name = _LOG_LEVEL_NAMES.get(level, f'L{level}')
    color = _LOG_LEVEL_COLORS.get(level, Style.RESET_ALL)

    # Format timestamp as seconds.millis
    secs = timestamp_ms // 1000
    ms = timestamp_ms % 1000

    print(f"  {color}[{secs:6d}.{ms:03d}] {level_name:5s} {message}{Style.RESET_ALL}")


# =============================================================================
# Status Payload Dispatcher
# =============================================================================

def parse_status_payload(payload: bytes, controller_type: str = None) -> None:
    """Parse STATUS packet payload with rich board-specific output.

    New format: [counter:u32][uptime:u32][freeRam:u32][lastActivity:u32][keepalives:u32][moduleData...]
    Legacy format: [counter:u32][uptime:u32][freeRam:u32][moduleData...] (12-byte header)
    """
    from .gearcontrol import _parse_gearcontrol_status
    from .gunfx import _parse_gunfx_status
    from .hubfx import _parse_hubfx_status
    from .lightfx import _parse_lightfx_status

    if len(payload) == 0:
        print("  (no payload)")
        return

    # Core header: 20 bytes (new) or 12 bytes (legacy)
    if len(payload) >= 12:
        counter = read_u32_le(payload, 0)
        uptime_ms = read_u32_le(payload, 4)
        free_ram = read_u32_le(payload, 8)

        # Extended header fields (20-byte format)
        has_extended = len(payload) >= 20
        if has_extended:
            last_activity_ms = read_u32_le(payload, 12)
            keepalive_count = read_u32_le(payload, 16)
            module_data = payload[20:]
        else:
            last_activity_ms = None
            keepalive_count = None
            module_data = payload[12:]

        # Format uptime
        uptime_sec = uptime_ms // 1000
        hours = uptime_sec // 3600
        minutes = (uptime_sec % 3600) // 60
        seconds = uptime_sec % 60
        if hours > 0:
            uptime_str = f"{hours}h {minutes}m {seconds}s"
        elif minutes > 0:
            uptime_str = f"{minutes}m {seconds}s"
        else:
            uptime_str = f"{seconds}s ({uptime_ms}ms)"

        # Format free RAM
        if free_ram >= 1024:
            ram_str = f"{free_ram} bytes ({free_ram / 1024:.1f} KB)"
        else:
            ram_str = f"{free_ram} bytes"

        print(f"  Commands:  {counter}")
        print(f"  Uptime:    {uptime_str}")
        print(f"  Free RAM:  {ram_str}")

        # Extended: last activity and keepalive count
        if has_extended:
            if last_activity_ms == 0:
                activity_str = "(first command)"
            elif last_activity_ms < 1000:
                activity_str = f"{last_activity_ms}ms ago"
            else:
                act_sec = last_activity_ms / 1000
                if act_sec >= 60:
                    activity_str = f"{act_sec / 60:.1f}m ago"
                else:
                    activity_str = f"{act_sec:.1f}s ago"
            print(f"  Last seen: {activity_str}  (keepalives: {keepalive_count})")

        # Module-specific data
        if len(module_data) > 0:
            if controller_type == 'gearcontrol':
                _parse_gearcontrol_status(module_data)
            elif controller_type == 'gunfx':
                _parse_gunfx_status(module_data)
            elif controller_type == 'hubfx':
                _parse_hubfx_status(module_data)
            elif controller_type == 'lightfx':
                _parse_lightfx_status(module_data)
            elif len(module_data) > 0:
                print(f"  Module:    {module_data.hex()} ({len(module_data)} bytes)")

    elif len(payload) >= 4:
        # Legacy 4-byte format (backward compatibility)
        counter = read_u32_le(payload, 0)
        print(f"  Commands:  {counter}")
        if len(payload) > 4:
            extra = payload[4:]
            print(f"  Extra:     {extra.hex()} ({len(extra)} bytes)")
    else:
        print(f"  Raw: {payload.hex()} ({len(payload)} bytes)")


# =============================================================================
# I2C Scan Result Parser
# =============================================================================

def parse_i2c_scan_result(payload: bytes) -> None:
    """Parse I2C_SCAN_RESULT packet payload.

    Wire format:
      [numExpected:u8]
      Per expected device x N (3 bytes each):
        [address:u8][found:u8][identified:u8]
      [numExtra:u8]
      Per extra device x M (1 byte each):
        [address:u8]
    """
    if len(payload) < 2:
        print(f"  I2C scan: (incomplete: {payload.hex()})")
        return

    idx = 0
    num_expected = payload[idx]; idx += 1

    print(f"  ── I2C Bus Scan ───────────────")
    print(f"  Expected devices: {num_expected}")

    for i in range(num_expected):
        if idx + 2 >= len(payload):
            break
        address = payload[idx]; idx += 1
        found = payload[idx] != 0; idx += 1
        identified = payload[idx] != 0; idx += 1

        if found and identified:
            status = f"{Fore.GREEN}OK{Style.RESET_ALL} (found + verified)"
        elif found:
            status = f"{Fore.YELLOW}FOUND{Style.RESET_ALL} (ACK but not verified)"
        else:
            status = f"{Fore.RED}MISSING{Style.RESET_ALL} (no ACK)"

        print(f"  0x{address:02X}: {status}")

    # Extra devices
    if idx < len(payload):
        num_extra = payload[idx]; idx += 1
        if num_extra > 0:
            extra_addrs = []
            for j in range(num_extra):
                if idx < len(payload):
                    extra_addrs.append(f"0x{payload[idx]:02X}")
                    idx += 1
            print(f"  Other devices: {', '.join(extra_addrs)}")
        else:
            print(f"  Other devices: none")

    print(f"  ────────────────────────────────")


# =============================================================================
# INIT_READY Parser
# =============================================================================

class InitReadyInfo:
    """Parsed INIT_READY response."""
    def __init__(self):
        self.name: str = ""
        self.version: str = ""
        self.platform: str = ""
        self.cpu_mhz: int = 0
        self.free_ram: int = 0
        self.build: int = 0
        self.controller_type: Optional[str] = None


def parse_init_ready(payload: bytes) -> Optional[InitReadyInfo]:
    """
    Parse INIT_READY payload.

    Returns InitReadyInfo with detected controller type, or None on parse error.
    """
    from ..base import ControllerType

    try:
        info = InitReadyInfo()
        offset = 0

        # Device name
        name_len = payload[offset]
        offset += 1
        info.name = payload[offset:offset+name_len].decode('utf-8', errors='replace')
        offset += name_len

        # Version
        ver_len = payload[offset]
        offset += 1
        info.version = payload[offset:offset+ver_len].decode('utf-8', errors='replace')
        offset += ver_len

        # Platform
        plat_len = payload[offset]
        offset += 1
        info.platform = payload[offset:offset+plat_len].decode('utf-8', errors='replace')
        offset += plat_len

        # CPU MHz (u32)
        info.cpu_mhz = read_u32_le(payload, offset)
        offset += 4

        # Free RAM (u32)
        info.free_ram = read_u32_le(payload, offset)
        offset += 4

        # Build number (u32)
        info.build = read_u32_le(payload, offset)

        # Detect controller type from name
        name_lower = info.name.lower()
        if 'gearcontrol' in name_lower or 'gear' in name_lower:
            info.controller_type = ControllerType.GEARCONTROL
        elif 'gunfx' in name_lower:
            info.controller_type = ControllerType.GUNFX
        elif 'hubfx' in name_lower:
            info.controller_type = ControllerType.HUBFX
        elif 'lightfx' in name_lower:
            info.controller_type = ControllerType.LIGHTFX
        elif 'noop' in name_lower:
            info.controller_type = ControllerType.NOOP

        return info

    except (IndexError, KeyError):
        return None


def print_init_ready_info(info: InitReadyInfo) -> None:
    """Print formatted INIT_READY information."""
    print(f"  Device:   {info.name}")
    print(f"  Version:  {info.version} (build {info.build})")
    print(f"  Platform: {info.platform} @ {info.cpu_mhz}MHz")
    print(f"  Free RAM: {info.free_ram} bytes")
