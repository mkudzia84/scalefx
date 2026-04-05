"""GearControl response parsers — status, calibration, sequence, and door status."""

from tests.framework import GearErrorReason
from tests.framework.protocol import read_u16_le, read_i16_le
from ..base import Fore, Style


def _parse_gearcontrol_status(data: bytes) -> None:
    """Parse GearControl module status data (53 bytes).

    Wire format:
      Per gear x 3 (11 bytes each = 33 bytes):
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
      [gear0_door_modes:u8]       # Packed: low nibble = doorPreDeploy, high nibble = doorPostDeploy
      [gear1_door_modes:u8]
      [gear2_door_modes:u8]
      [gear0_config_flags:u8]     # GearConfigFlags bitmask per gear
      [gear1_config_flags:u8]
      [gear2_config_flags:u8]
    """
    from tests.framework.packets import DoorMode as DoorModeClass
    from tests.framework.packets import DoorState as DoorStateClass
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

    # Packed door modes per gear (bytes 44-46)
    door_modes = [0, 0, 0]
    post_deploy_modes = [0, 0, 0]
    if len(data) >= 47:
        for i in range(3):
            packed = data[44 + i]
            door_modes[i] = packed & 0x0F
            post_deploy_modes[i] = (packed >> 4) & 0x0F

    # Config flags per gear (bytes 47-49)
    config_flags = [0, 0, 0]
    if len(data) >= 50:
        config_flags = [data[47], data[48], data[49]]

    # Door state per gear (bytes 50-52) — DoorState values
    door_states = [0, 0, 0]  # Default UNKNOWN
    if len(data) >= 53:
        door_states = [data[50], data[51], data[52]]

    # LED flags and battery (global)
    yaw = read_u16_le(data, 33)
    led_flags = data[35]
    battery_mV = read_u16_le(data, 36)
    battery_flags = data[38]

    for i in range(3):
        offset = i * 11
        state = data[offset]
        current_mA = read_u16_le(data, offset + 1)
        door0 = read_u16_le(data, offset + 3)
        door1 = read_u16_le(data, offset + 5)
        stall_mA = read_u16_le(data, offset + 7)
        shunt_10uV = read_i16_le(data, offset + 9)  # signed, in 10uV units
        shunt_mV = shunt_10uV * 10 / 1000.0  # convert to millivolts

        sname = state_names.get(state, f'?({state})')
        scolor = state_colors.get(state, '')

        # Config flags
        cflags = config_flags[i]
        enabled = bool(cflags & 0x80)
        has_yaw = bool(cflags & 0x01)

        # Build status tags
        tags = []
        if not enabled:
            tags.append(f"{Fore.YELLOW}DISABLED{Style.RESET_ALL}")
        if state == 5 and error_reasons[i] != 0:  # GearState::ERROR
            tags.append(f"{Fore.RED}{GearErrorReason.name(error_reasons[i])}{Style.RESET_ALL}")
        tag_str = f"  [{', '.join(tags)}]" if tags else ""

        # Stall calibration
        stall_str = f"stall={stall_mA}mA" if stall_mA > 0 else f"{Fore.YELLOW}uncalibrated{Style.RESET_ALL}"

        # Door modes
        dmode = door_modes[i]
        dmode_name = DoorModeClass.name(dmode).lower()
        pdmode = post_deploy_modes[i]
        pdmode_name = DoorModeClass.name(pdmode).lower() if pdmode != 0 else "skip"

        # Line 1: state + motor + stall
        print(f"  {gear_names[i]:>10}: {scolor}{sname}{Style.RESET_ALL}{tag_str}")

        # Line 2: current readings + calibration
        print(f"             motor={current_mA}mA  shunt={shunt_mV:.1f}mV  {stall_str}")

        # Line 3: doors + config
        dstate = door_states[i]
        dstate_name = DoorStateClass.name(dstate)
        dstate_colors = {
            0: Fore.YELLOW,  # unknown
            1: Fore.CYAN,    # closed
            2: Fore.GREEN,   # open
            3: Fore.YELLOW,  # opening
            4: Fore.YELLOW,  # closing
        }
        dstate_color = dstate_colors.get(dstate, '')
        if dmode != 0:
            print(f"             doors=[{door0}µs, {door1}µs]  {dstate_color}{dstate_name}{Style.RESET_ALL}"
                  f"  pre={dmode_name}  post={pdmode_name}"
                  f"{'  yaw' if has_yaw else ''}")
        else:
            print(f"             doors=none{'  yaw' if has_yaw else ''}")

    # ── Global ──
    print(f"  ── Global ─────────────────────")
    print(f"  Yaw:       {yaw}µs")

    # Shunt resistance config
    if shunt_mohm > 0:
        shunt_ohm = shunt_mohm / 1000.0
        max_current = 81.92 / shunt_ohm  # INA226 max shunt voltage = +/-81.92mV
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

    # Status LEDs: per-gear deploy/retract indicators
    led_labels_full = [
        ('Nose Deploy', 'Nose Retract'),
        ('Left Deploy', 'Left Retract'),
        ('Right Deploy', 'Right Retract'),
    ]
    led_parts = []
    for gi in range(3):
        dep_bit = gi * 2
        ret_bit = gi * 2 + 1
        dep_on = bool(led_flags & (1 << dep_bit))
        ret_on = bool(led_flags & (1 << ret_bit))
        abbr = gear_names[gi][0]  # N, L, R
        if dep_on and ret_on:
            led_parts.append(f"{Fore.YELLOW}{abbr}:both{Style.RESET_ALL}")
        elif dep_on:
            led_parts.append(f"{Fore.GREEN}{abbr}:dep{Style.RESET_ALL}")
        elif ret_on:
            led_parts.append(f"{Fore.CYAN}{abbr}:ret{Style.RESET_ALL}")
        else:
            led_parts.append(f"{abbr}:off")

    # Indicator LEDs (bits 6-7)
    conn_led = bool(led_flags & (1 << 6))
    err_led = bool(led_flags & (1 << 7))
    led_parts.append(f"{Fore.GREEN}CONN{Style.RESET_ALL}" if conn_led else "conn")
    led_parts.append(f"{Fore.RED}ERR{Style.RESET_ALL}" if err_led else "err")
    print(f"  LEDs:      [{', '.join(led_parts)}]")


# =============================================================================
# GearControl Calibration Status Parser
# =============================================================================

def parse_gear_calib_status(payload: bytes) -> None:
    """Parse GEAR_CALIB_STATUS packet payload.

    Wire format (10 bytes):
      [gear_id:u8][phase:u8][current_mA:u16LE][peak_mA:u16LE][calibratedStall_mA:u16LE][finished:u8][errorReason:u8]
      errorReason is optional (backward-compatible).
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
    error_reason = payload[9] if len(payload) >= 10 else 0

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
    # Show error reason on ERROR phase
    if phase == 7 and error_reason > 0:
        reason_name = GearErrorReason.name(error_reason)
        parts.append(f"{Fore.RED}reason={reason_name}{Style.RESET_ALL}")

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
# GearControl Door Status Parser
# =============================================================================

def parse_gear_door_status(payload: bytes) -> None:
    """Parse GEAR_DOOR_STATUS packet payload.

    Wire format (6 bytes):
      [gear_id:u8][state:u8][door0_pos_us:u16LE][door1_pos_us:u16LE]
    """
    from tests.framework.packets import DoorState

    if len(payload) < 2:
        print(f"  DoorStatus: (incomplete: {payload.hex()})")
        return

    gear_id = payload[0]
    state = payload[1]
    door0 = read_u16_le(payload, 2) if len(payload) >= 4 else 0
    door1 = read_u16_le(payload, 4) if len(payload) >= 6 else 0

    gear_names = {0: 'Nose', 1: 'Left Main', 2: 'Right Main'}
    gear_name = gear_names.get(gear_id, f'Gear {gear_id}')
    state_name = DoorState.name(state)

    # Color based on door state
    state_colors = {
        DoorState.UNKNOWN: Fore.YELLOW,
        DoorState.CLOSED:  Fore.CYAN,
        DoorState.OPEN:    Fore.GREEN,
        DoorState.OPENING: Fore.YELLOW,
        DoorState.CLOSING: Fore.YELLOW,
    }
    state_color = state_colors.get(state, '')

    parts = [f"{state_color}{state_name}{Style.RESET_ALL}"]
    if len(payload) >= 4:
        parts.append(f"d0={door0}µs")
    if len(payload) >= 6:
        parts.append(f"d1={door1}µs")

    print(f"  {Fore.MAGENTA}◇{Style.RESET_ALL} {gear_name} doors: {', '.join(parts)}")
