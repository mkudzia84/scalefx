"""LightFX response parsers — status, landing lights, LED sequences."""

from typing import Optional
from tests.framework.protocol import read_u16_le
from ..base import Fore, Style


def _parse_lightfx_status(data: bytes) -> None:
    """Parse LightFX module status data (24 bytes).

    Wire format:
      [ledBrightness:u8x8][ledSeqFlags:u8]
      [servo0:u16][servo1:u16][servo2:u16]
      [landingLightStates:u8x3]
      [masterBrightness_pct:u8]
      [ledEnabledFlags:u8]
      [batteryV_mV:u16LE][cellCount:u8][batteryPct:u8]
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

    # Landing light states (optional, backward compat)
    ll_states = []
    if len(data) >= 18:
        ll_phase_names = {0: 'RET', 1: 'DEPLOYING', 2: 'DEP', 3: 'RETRACTING'}
        for i in range(3):
            phase = data[15 + i]
            ll_states.append(ll_phase_names.get(phase, f'?({phase})'))

    # Master brightness (optional, backward compat)
    master_brightness = data[18] if len(data) >= 19 else 100

    # Enabled flags (optional, backward compat)
    enabled_flags = data[19] if len(data) >= 20 else 0xFF  # default all enabled

    print(f"  ── LightFX ────────────────────")

    # LED status (compact format with enabled/disabled indicators)
    led_parts = []
    for i in range(8):
        ch = i + 1
        bri = led_brightness[i]
        seq = bool(seq_flags & (1 << i))
        enabled = bool(enabled_flags & (1 << i))
        if not enabled:
            led_parts.append(f"ch{ch}={bri}[DIS]")
        elif bri > 0 or seq:
            seq_mark = "▶" if seq else ""
            led_parts.append(f"ch{ch}={bri}{seq_mark}")
    if led_parts:
        print(f"  LEDs:      {', '.join(led_parts)}")
    else:
        print(f"  LEDs:      all off")

    # Master brightness (only show if not 100%)
    if master_brightness < 100:
        print(f"  Master:    {master_brightness}%")

    # Servos
    print(f"  Servos:    [{servo0}µs, {servo1}µs, {servo2}µs]")

    # Landing lights
    if ll_states:
        ll_parts = [f"slot{i+1}={s}" for i, s in enumerate(ll_states)]
        print(f"  Lights:    {', '.join(ll_parts)}")

    # Battery (optional, backward compat — bytes 20-23)
    if len(data) >= 24:
        bat_mv = read_u16_le(data, 20)
        cell_count = data[22]
        bat_pct = data[23]
        bat_v = bat_mv / 1000.0
        print(f"  Battery:   {bat_v:.2f}V ({bat_pct}%, {cell_count}S)")


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
        'brightness': payload[8] if len(payload) >= 9 else 0,
    }


def parse_led_seq_queue(payload: bytes) -> Optional[dict]:
    """Parse LED_SEQ_QUEUE_RESP payload."""
    if len(payload) < 5:
        return None

    result = {
        'channel': payload[0],
        'count': payload[1],
        'current_index': payload[2],
        'playing': payload[3] != 0,
        'brightness': payload[4],
        'events': [],
    }

    event_names = ['ON', 'OFF', 'FLASH', 'FADE_IN', 'FADE_OUT', 'FADING', 'BEACON']

    for i in range(result['count']):
        offset = 5 + (i * 4)
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
