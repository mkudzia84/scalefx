"""
ScaleFX CLI Response Parsers

Centralized parsing of protocol response packets into human-readable format.
Eliminates duplicated parsing logic across command handlers.
"""

from typing import Optional
from tests.framework import (
    CorePacket, CoreError, GearControlError, GunFxError, LightFxError,
    GearControlPacket, GunFxPacket, LightFxPacket, GearErrorReason
)
from tests.framework.protocol import read_u16_le, read_u32_le, read_i16_le
from .base import Fore, Style


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
    
    return f"UNKNOWN (0x{ptype:02X})"


def error_name(code: int) -> str:
    """Get human-readable error name, checking all modules."""
    name = GearControlError.name(code)
    if "UNKNOWN" in name:
        name = GunFxError.name(code)
    if "UNKNOWN" in name:
        name = LightFxError.name(code)
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


def parse_status_payload(payload: bytes, controller_type: str = None) -> None:
    """Parse STATUS packet payload with rich board-specific output.
    
    New format: [counter:u32][uptime:u32][freeRam:u32][moduleData...]
    Legacy format: [counter:u32] (4 bytes only)
    """
    if len(payload) == 0:
        print("  (no payload)")
        return
    
    # Core header (12 bytes): counter + uptime + freeRam
    if len(payload) >= 12:
        counter = read_u32_le(payload, 0)
        uptime_ms = read_u32_le(payload, 4)
        free_ram = read_u32_le(payload, 8)
        
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
        
        # Module-specific data
        module_data = payload[12:]
        if len(module_data) > 0:
            if controller_type == 'gearcontrol':
                _parse_gearcontrol_status(module_data)
            elif controller_type == 'gunfx':
                _parse_gunfx_status(module_data)
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


def _parse_gearcontrol_status(data: bytes) -> None:
    """Parse GearControl module status data (50 bytes).
    
    Wire format:
      Per gear × 3 (11 bytes each = 33 bytes):
        [state:u8][motor_current_mA:u16][door0_pos_us:u16][door1_pos_us:u16]
        [calibratedStall_mA:u16][shuntVoltage_10uV:i16]
      [yaw_pos_us:u16]
      [led_flags:u8]              # bits 0-5 status, 6-7 indicators
      [battery_voltage_mV:u16]
      [battery_config_flags:u8]   # bit 0: auto-deploy, bit 1: low voltage, bit 2: battery enabled
      [gear0_error_reason:u8]     # Per-gear error reason (GearErrorReason)
      [gear1_error_reason:u8]
      [gear2_error_reason:u8]
      [shuntResistance_mohm:u16]  # Configured shunt resistance in milliohms
      [gear0_door_mode:u8]        # DoorMode enum per gear
      [gear1_door_mode:u8]
      [gear2_door_mode:u8]
      [gear0_config_flags:u8]     # GearConfigFlags bitmask per gear
      [gear1_config_flags:u8]
      [gear2_config_flags:u8]
    """
    from tests.framework.packets import DoorMode as DoorModeClass
    if len(data) < 39:
        print(f"  GearControl: (incomplete: {data.hex()})")
        return
    
    gear_names = ['Nose', 'Left Main', 'Right Main']
    state_names = {
        0: 'UNKNOWN', 1: 'DEPLOYED', 2: 'RETRACTED',
        3: 'DEPLOYING', 4: 'RETRACTING', 5: 'ERROR',
        6: 'CALIBRATING',
    }
    state_colors = {
        0: '', 1: Fore.GREEN, 2: Fore.CYAN,
        3: Fore.YELLOW, 4: Fore.YELLOW, 5: Fore.RED,
        6: Fore.MAGENTA,
    }
    
    print(f"  ── GearControl ────────────────")
    
    # Parse per-gear error reasons (bytes 39-41)
    error_reasons = [0, 0, 0]
    if len(data) >= 42:
        error_reasons = [data[39], data[40], data[41]]
    
    # Shunt resistance (bytes 42-43) — configured value in milliohms
    shunt_mohm = read_u16_le(data, 42) if len(data) >= 44 else 0

    # Door mode per gear (bytes 44-46)
    door_modes = [0, 0, 0]
    if len(data) >= 47:
        door_modes = [data[44], data[45], data[46]]

    # Config flags per gear (bytes 47-49)
    config_flags = [0, 0, 0]
    if len(data) >= 50:
        config_flags = [data[47], data[48], data[49]]
    
    for i in range(3):
        offset = i * 11
        state = data[offset]
        current_mA = read_u16_le(data, offset + 1)
        door0 = read_u16_le(data, offset + 3)
        door1 = read_u16_le(data, offset + 5)
        stall_mA = read_u16_le(data, offset + 7)
        shunt_10uV = read_i16_le(data, offset + 9)  # signed, in 10µV units
        shunt_mV = shunt_10uV * 10 / 1000.0  # convert to millivolts
        
        sname = state_names.get(state, f'?({state})')
        scolor = state_colors.get(state, '')
        
        stall_str = f"stall={stall_mA}mA" if stall_mA > 0 else "stall=uncal"
        
        # Show error reason when in ERROR state
        reason_str = ""
        if state == 5 and error_reasons[i] != 0:  # GearState::ERROR
            reason_str = f"  {Fore.RED}({GearErrorReason.name(error_reasons[i])}){Style.RESET_ALL}"

        # Door mode display
        dmode = door_modes[i]
        dmode_name = DoorModeClass.name(dmode).lower()

        # Config flags display
        cflags = config_flags[i]
        cflag_parts = []
        if cflags & 0x01: cflag_parts.append("close-retract")
        if cflags & 0x02: cflag_parts.append("close-deploy")
        if cflags & 0x04: cflag_parts.append("yaw")
        cflag_str = ', '.join(cflag_parts) if cflag_parts else "none"

        # Door position annotation
        door_str = f"doors=[{door0}µs, {door1}µs]" if dmode != 0 else "doors=n/a"
        
        print(f"  {gear_names[i]:>10}: {scolor}{sname}{Style.RESET_ALL}{reason_str}  "
              f"motor={current_mA}mA  shunt={shunt_mV:.1f}mV  "
              f"{door_str}  {stall_str}")
        print(f"             door-mode={dmode_name}  flags=[{cflag_str}]")
    
    yaw = read_u16_le(data, 33)
    led_flags = data[35]
    battery_mV = read_u16_le(data, 36)
    battery_flags = data[38]
    
    print(f"  Yaw:       {yaw}µs")
    
    # Shunt resistance config
    if shunt_mohm > 0:
        shunt_ohm = shunt_mohm / 1000.0
        max_current = 81.92 / shunt_ohm  # INA226 max shunt voltage = ±81.92mV
        print(f"  Shunt:     {shunt_mohm}mΩ ({shunt_ohm}Ω)  max={max_current:.0f}mA")
    
    # Battery voltage and config
    battery_enabled = bool(battery_flags & 0x04)
    auto_deploy = bool(battery_flags & 0x01)
    low_voltage = bool(battery_flags & 0x02)

    if not battery_enabled:
        print(f"  Battery:   {Fore.YELLOW}disabled{Style.RESET_ALL}")
    else:
        battery_V = battery_mV / 1000.0
        battery_parts = [f"{battery_V:.1f}V ({battery_mV}mV)"]
        if auto_deploy:
            battery_parts.append(f"{Fore.CYAN}auto-deploy{Style.RESET_ALL}")
        if low_voltage:
            battery_parts.append(f"{Fore.RED}LOW VOLTAGE{Style.RESET_ALL}")
        print(f"  Battery:   {', '.join(battery_parts)}")
    
    # Status LED flags display
    led_parts = []
    led_labels = ['ND', 'NR', 'LD', 'LR', 'RD', 'RR']
    for i in range(6):
        if led_flags & (1 << i):
            led_parts.append(f"{Fore.GREEN}{led_labels[i]}{Style.RESET_ALL}")
        else:
            led_parts.append(f"{led_labels[i]}")
    print(f"  Status:    [{', '.join(led_parts)}]")
    
    # Indicator LEDs (bits 6-7)
    conn_led = bool(led_flags & (1 << 6))
    err_led = bool(led_flags & (1 << 7))
    ind_parts = []
    ind_parts.append(f"{Fore.GREEN}CONN{Style.RESET_ALL}" if conn_led else "CONN")
    ind_parts.append(f"{Fore.RED}ERR{Style.RESET_ALL}" if err_led else "ERR")
    print(f"  Indicators:[{', '.join(ind_parts)}]")


def _parse_gunfx_status(data: bytes) -> None:
    """Parse GunFX module status data (20 bytes).
    
    Wire format:
      [flags:u8][fanSpeed:u8][fanOffMs:u16]
      [servo0:u16][servo1:u16][servo2:u16]
      [rpm:u16][shots:u32][heaterMs:u32]
    """
    if len(data) < 20:
        print(f"  GunFX:     (incomplete: {data.hex()})")
        return
    
    flags = data[0]
    firing       = bool(flags & 0x01)
    flash_active = bool(flags & 0x02)
    flash_fading = bool(flags & 0x04)
    heater_on    = bool(flags & 0x08)
    fan_on       = bool(flags & 0x10)
    fan_spindown = bool(flags & 0x20)
    
    fan_speed = data[1]
    fan_off_ms = read_u16_le(data, 2)
    servo0 = read_u16_le(data, 4)
    servo1 = read_u16_le(data, 6)
    servo2 = read_u16_le(data, 8)
    rpm = read_u16_le(data, 10)
    shots = read_u32_le(data, 12)
    heater_ms = read_u32_le(data, 16)
    
    # Build state flags string
    state_parts = []
    if firing:       state_parts.append(f"{Fore.RED}FIRING{Style.RESET_ALL}")
    if flash_active: state_parts.append("FLASH")
    if flash_fading: state_parts.append("FADING")
    if heater_on:    state_parts.append(f"{Fore.YELLOW}HEATER{Style.RESET_ALL}")
    if fan_on:       state_parts.append("FAN")
    if fan_spindown: state_parts.append("SPINDOWN")
    state_str = ', '.join(state_parts) if state_parts else "IDLE"
    
    print(f"  ── GunFX ──────────────────────")
    print(f"  State:     {state_str}")
    if fan_on or fan_spindown:
        fan_info = f"speed={fan_speed}"
        if fan_spindown and fan_off_ms > 0:
            fan_info += f", off in {fan_off_ms}ms"
        print(f"  Fan:       {fan_info}")
    print(f"  Servos:    [{servo0}µs, {servo1}µs, {servo2}µs]")
    if firing:
        print(f"  Fire rate: {rpm} RPM")
    print(f"  Shots:     {shots}")
    if heater_ms > 0:
        heater_sec = heater_ms / 1000
        print(f"  Heater:    {heater_sec:.1f}s total")


def _parse_lightfx_status(data: bytes) -> None:
    """Parse LightFX module status data (15 bytes).
    
    Wire format:
      [ledBrightness:u8×8][ledSeqFlags:u8]
      [servo0:u16][servo1:u16][servo2:u16]
    """
    if len(data) < 15:
        print(f"  LightFX:   (incomplete: {data.hex()})")
        return
    
    # LED channels
    led_brightness = [data[i] for i in range(8)]
    seq_flags = data[8]
    
    # Servos
    servo0 = read_u16_le(data, 9)
    servo1 = read_u16_le(data, 11)
    servo2 = read_u16_le(data, 13)
    
    print(f"  ── LightFX ────────────────────")
    
    # LED status (compact format)
    led_parts = []
    for i in range(8):
        ch = i + 1
        bri = led_brightness[i]
        seq = bool(seq_flags & (1 << i))
        if bri > 0 or seq:
            seq_mark = "▶" if seq else ""
            led_parts.append(f"ch{ch}={bri}{seq_mark}")
    if led_parts:
        print(f"  LEDs:      {', '.join(led_parts)}")
    else:
        print(f"  LEDs:      all off")
    
    # Servos
    print(f"  Servos:    [{servo0}µs, {servo1}µs, {servo2}µs]")


# =============================================================================
# GearControl Calibration Status Parser
# =============================================================================

def parse_gear_calib_status(payload: bytes) -> None:
    """Parse GEAR_CALIB_STATUS packet payload.
    
    Wire format (9 bytes):
      [gear_id:u8][phase:u8][current_mA:u16LE][peak_mA:u16LE][calibratedStall_mA:u16LE][finished:u8]
    """
    if len(payload) < 9:
        print(f"  CalibStatus: (incomplete: {payload.hex()})")
        return
    
    gear_id = payload[0]
    phase = payload[1]
    current_mA = read_u16_le(payload, 2)
    peak_mA = read_u16_le(payload, 4)
    stall_mA = read_u16_le(payload, 6)
    finished = payload[8] != 0
    
    gear_names = {0: 'Nose', 1: 'Left Main', 2: 'Right Main'}
    gear_name = gear_names.get(gear_id, f'Gear {gear_id}')
    
    phase_names = {
        0: 'IDLE', 1: 'CLEAR_RUN', 2: 'CLEAR_SETTLE',
        3: 'DEPLOY_RUN', 4: 'MID_SETTLE', 5: 'RETRACT_RUN',
        6: 'COMPLETE', 7: 'ERROR', 8: 'CANCELLED',
        9: 'OPENING_DOORS', 10: 'CLOSING_DOORS',
    }
    phase_name = phase_names.get(phase, f'?({phase})')
    
    # Color based on phase
    if phase == 6:  # COMPLETE
        phase_color = Fore.GREEN
    elif phase == 7:  # ERROR
        phase_color = Fore.RED
    elif phase == 8:  # CANCELLED
        phase_color = Fore.YELLOW
    elif phase in (1, 3, 5):  # Motor running phases
        phase_color = Fore.CYAN
    else:
        phase_color = ''
    
    parts = [f"{phase_color}{phase_name}{Style.RESET_ALL}"]
    parts.append(f"current={current_mA}mA")
    if peak_mA > 0:
        parts.append(f"peak={peak_mA}mA")
    if stall_mA > 0:
        parts.append(f"stall={stall_mA}mA")
    if finished:
        parts.append(f"{Fore.WHITE}[FINISHED]{Style.RESET_ALL}")
    
    print(f"  {Fore.MAGENTA}◆{Style.RESET_ALL} {gear_name} calib: {', '.join(parts)}")


# =============================================================================
# GearControl Sequence Status Parser
# =============================================================================

def parse_gear_seq_status(payload: bytes) -> None:
    """Parse GEAR_SEQ_STATUS packet payload.
    
    Wire format (8 bytes):
      [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]
    """
    from tests.framework.packets import GearSeqPhase

    if len(payload) < 8:
        print(f"  SeqStatus: (incomplete: {payload.hex()})")
        return
    
    gear_id = payload[0]
    phase = payload[1]
    deploying = payload[2] != 0
    finished = payload[3] != 0
    elapsed_ms = int.from_bytes(payload[4:8], 'little')
    
    gear_names = {0: 'Nose', 1: 'Left Main', 2: 'Right Main'}
    gear_name = gear_names.get(gear_id, f'Gear {gear_id}')
    phase_name = GearSeqPhase.name(phase)
    action = "deploy" if deploying else "retract"
    
    # Color based on state
    if finished and phase != GearSeqPhase.SEQ_ERROR:
        phase_color = Fore.GREEN
    elif phase == GearSeqPhase.SEQ_ERROR:
        phase_color = Fore.RED
    elif phase == GearSeqPhase.RUNNING_MOTOR:
        phase_color = Fore.CYAN
    elif phase == GearSeqPhase.SYNC_WAIT:
        phase_color = Fore.MAGENTA
    else:
        phase_color = Fore.YELLOW
    
    # Format elapsed time
    elapsed_sec = elapsed_ms / 1000.0
    
    parts = [f"{phase_color}{phase_name}{Style.RESET_ALL}"]
    parts.append(action)
    parts.append(f"{elapsed_sec:.1f}s")
    if finished:
        parts.append(f"{Fore.WHITE}[FINISHED in {elapsed_sec:.1f}s]{Style.RESET_ALL}")
    
    print(f"  {Fore.MAGENTA}▸{Style.RESET_ALL} {gear_name} seq: {', '.join(parts)}")


# =============================================================================
# Landing Light Status Parser
# =============================================================================

def parse_landing_light_status(payload: bytes) -> None:
    """Parse LANDING_LIGHT_STATUS packet payload.
    
    Wire format (3 bytes):
      [slot:u8][phase:u8][finished:u8]
    """
    from tests.framework.packets import LandingLightPhase

    if len(payload) < 3:
        print(f"  LandingLightStatus: (incomplete: {payload.hex()})")
        return
    
    slot = payload[0]
    phase = payload[1]
    finished = payload[2] != 0
    
    phase_name = LandingLightPhase.name(phase)
    
    # Color based on state
    if phase == LandingLightPhase.DEPLOYED:
        phase_color = Fore.GREEN
    elif phase == LandingLightPhase.RETRACTED:
        phase_color = Fore.YELLOW
    else:
        phase_color = Fore.CYAN
    
    parts = [f"{phase_color}{phase_name}{Style.RESET_ALL}"]
    if finished:
        parts.append(f"{Fore.WHITE}[FINISHED]{Style.RESET_ALL}")
    
    print(f"  {Fore.BLUE}▸{Style.RESET_ALL} Landing light {slot}: {', '.join(parts)}")


# =============================================================================
# I2C Scan Result Parser
# =============================================================================

def parse_i2c_scan_result(payload: bytes) -> None:
    """Parse I2C_SCAN_RESULT packet payload.
    
    Wire format:
      [numExpected:u8]
      Per expected device × N (3 bytes each):
        [address:u8][found:u8][identified:u8]
      [numExtra:u8]
      Per extra device × M (1 byte each):
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
    from .base import ControllerType
    
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
        elif 'gunfx' in name_lower or 'gun' in name_lower:
            info.controller_type = ControllerType.GUNFX
        elif 'lightfx' in name_lower or 'light' in name_lower:
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


# =============================================================================
# LightFX Response Parsers
# =============================================================================

def parse_led_seq_status(payload: bytes) -> Optional[dict]:
    """Parse LED_SEQ_STATUS_RESP payload."""
    if len(payload) < 8:
        return None
    
    return {
        'channel': payload[0],
        'playing': payload[1] != 0,
        'event_count': payload[2],
        'current_index': payload[3],
        'loop_count': int.from_bytes(payload[4:8], 'little'),
    }


def parse_led_seq_queue(payload: bytes) -> Optional[dict]:
    """Parse LED_SEQ_QUEUE_RESP payload."""
    if len(payload) < 4:
        return None
    
    result = {
        'channel': payload[0],
        'count': payload[1],
        'current_index': payload[2],
        'playing': payload[3] != 0,
        'events': [],
    }
    
    event_names = ['ON', 'OFF', 'FLASH', 'FADE_IN', 'FADE_OUT', 'FADING']
    
    for i in range(result['count']):
        offset = 4 + (i * 4)
        if offset + 4 <= len(payload):
            etype = payload[offset]
            duration = int.from_bytes(payload[offset+1:offset+3], 'little')
            param1 = payload[offset+3]
            
            ename = event_names[etype] if etype < len(event_names) else f"UNKNOWN(0x{etype:02X})"
            result['events'].append({
                'index': i,
                'type': etype,
                'type_name': ename,
                'duration': duration,
                'param1': param1,
            })
    
    return result


def parse_led_status(payload: bytes) -> list:
    """Parse LED_STATUS_RESP payload."""
    channels = []
    for i in range(0, len(payload), 4):
        if i + 4 <= len(payload):
            channels.append({
                'channel': payload[i],
                'brightness': payload[i+1],
                'seq_playing': payload[i+2] != 0,
                'seq_count': payload[i+3],
            })
    return channels



