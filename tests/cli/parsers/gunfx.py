"""GunFX response parsers — status display."""

from tests.framework.protocol import read_u16_le, read_u32_le
from ..base import Fore, Style


def _parse_gunfx_status(data: bytes) -> None:
    """Parse GunFX module status data (28 bytes).

    Wire format:
      [flags:u8][fanSpeed:u8][fanOffMs:u16]
      [servo0:u16][servo1:u16][servo2:u16]
      [rpm:u16][shots:u32][heaterMs:u32]
      [heaterError:u8][fanError:u8]
      [heaterDuty:u8][fanDuty:u8]
      [batteryV_mV:u16][cellCount:u8][batteryPct:u8]
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

    # ── Muzzle Flash ──
    if firing:
        print(f"  Fire rate: {rpm} RPM")
    print(f"  Shots:     {shots}")

    # ── Fan ──
    if fan_on or fan_spindown:
        fan_info = f"speed={fan_speed}"
        if fan_spindown and fan_off_ms > 0:
            fan_info += f", off in {fan_off_ms}ms"
        print(f"  Fan:       {fan_info}")

    # ── Heater ──
    if heater_ms > 0:
        heater_sec = heater_ms / 1000
        print(f"  Heater:    {heater_sec:.1f}s total")

    # ── Servos ──
    print(f"  Servos:    [{servo0}µs, {servo1}µs, {servo2}µs]")

    # ── Smoke Error Reasons (bytes 20-21) ──
    if len(data) >= 22:
        from tests.framework.packets import SmokeErrorReason
        htr_err = data[20]
        fan_err = data[21]
        if htr_err != SmokeErrorReason.NONE or fan_err != SmokeErrorReason.NONE:
            print(f"  ── Smoke Errors ──────────────")
            if htr_err != SmokeErrorReason.NONE:
                print(f"  Heater:    {Fore.RED}{SmokeErrorReason.name(htr_err)}{Style.RESET_ALL}")
            if fan_err != SmokeErrorReason.NONE:
                print(f"  Fan:       {Fore.RED}{SmokeErrorReason.name(fan_err)}{Style.RESET_ALL}")

    # ── Overcurrent Throttle State (bytes 22-23) ──
    if len(data) >= 24:
        htr_duty = data[22]
        fan_duty = data[23]
        if htr_duty < 255 or fan_duty < 255:
            print(f"  ── Overcurrent Throttle ──────")
            if htr_duty < 255:
                pct = round(htr_duty / 255 * 100)
                print(f"  Heater:    {Fore.YELLOW}throttled to {pct}% (duty {htr_duty}/255){Style.RESET_ALL}")
            if fan_duty < 255:
                pct = round(fan_duty / 255 * 100)
                print(f"  Fan:       {Fore.YELLOW}throttled to {pct}% (duty {fan_duty}/255){Style.RESET_ALL}")

    # ── Battery (bytes 24-27) ──
    if len(data) >= 28:
        battery_mV  = read_u16_le(data, 24)
        cell_count  = data[26]
        battery_pct = data[27]

        battery_V = battery_mV / 1000.0
        if battery_mV > 0:
            batt_parts = [f"{battery_V:.2f}V ({battery_mV}mV)"]
            if cell_count > 0:
                batt_parts.append(f"{cell_count}S")
            if battery_pct > 0:
                pct_color = Fore.GREEN if battery_pct > 30 else (Fore.YELLOW if battery_pct > 10 else Fore.RED)
                batt_parts.append(f"{pct_color}{battery_pct}%{Style.RESET_ALL}")
            print(f"  Battery:   {', '.join(batt_parts)}")
        else:
            print(f"  Battery:   {Fore.YELLOW}not detected{Style.RESET_ALL}")
