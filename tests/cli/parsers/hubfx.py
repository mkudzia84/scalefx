"""HubFX response parsers — status display and feature extraction."""

from typing import Optional
from tests.framework.protocol import read_u32_le
from ..base import Fore, Style


def _parse_hubfx_status(data: bytes) -> None:
    """Parse HubFX ESP32-S3 module status data (6 bytes).

    Wire format:
      [flags:u8][slaveMask:u8][loop1Count:u32LE]

    Flags byte:
      bit 0: core1Ready
      bit 1: audioInitialized
      bit 2: flashReady
      bit 3: usbHostReady
      bit 4: sdCardReady
    """
    if len(data) < 2:
        print(f"  Hub data:  {data.hex()} ({len(data)} bytes)")
        return

    flags = data[0]
    slave_mask = data[1]
    loop1_count = read_u32_le(data, 2) if len(data) >= 6 else 0

    # Decode flags
    core1_ready = bool(flags & 0x01)
    audio_init = bool(flags & 0x02)
    flash_ready = bool(flags & 0x04)
    usb_ready = bool(flags & 0x08)
    sd_ready = bool(flags & 0x10)

    print(f"\n  {Fore.CYAN}━━━ HubFX Status ━━━{Style.RESET_ALL}")

    # Core 1
    c1_color = Fore.GREEN if core1_ready else Fore.RED
    c1_text = "Ready" if core1_ready else "NOT READY"
    print(f"  Core 1:    {c1_color}{c1_text}{Style.RESET_ALL}")
    if len(data) >= 6:
        print(f"             {loop1_count} iterations")

    # Audio
    audio_color = Fore.GREEN if audio_init else Fore.YELLOW
    audio_text = "Initialized" if audio_init else "Not initialized"
    print(f"  Audio:     {audio_color}{audio_text}{Style.RESET_ALL}")

    # Flash
    flash_color = Fore.GREEN if flash_ready else Fore.YELLOW
    flash_text = "Ready" if flash_ready else "Not available"
    print(f"  Flash:     {flash_color}{flash_text}{Style.RESET_ALL}")

    # SD Card
    sd_color = Fore.GREEN if sd_ready else Fore.YELLOW
    sd_text = "Ready" if sd_ready else "Not available"
    print(f"  SD Card:   {sd_color}{sd_text}{Style.RESET_ALL}")

    # USB Host
    usb_color = Fore.GREEN if usb_ready else Fore.YELLOW
    usb_text = "Active" if usb_ready else "Not active"
    print(f"  USB Host:  {usb_color}{usb_text}{Style.RESET_ALL}")

    # Slave mask
    slave_names = {0: 'GunFX', 1: 'LightFX', 2: 'GearControl'}
    has_slaves = any(slave_mask & (1 << bit) for bit in slave_names)
    if has_slaves:
        print(f"  Slaves:")
        for bit, name in slave_names.items():
            is_ready = bool(slave_mask & (1 << bit))
            color = Fore.GREEN if is_ready else Fore.RED
            status = "connected" if is_ready else "not connected"
            print(f"    {name}: {color}{status}{Style.RESET_ALL}")
    else:
        print(f"  Slaves:    {Fore.YELLOW}None connected{Style.RESET_ALL}")


def extract_hubfx_features(payload: bytes) -> Optional[dict]:
    """Extract HubFX feature flags from a STATUS response payload.

    Parses past the core header (20 or 12 bytes) to reach the HubFX
    module data, then decodes the flags byte and slave mask.

    Returns:
        Dict mapping feature name -> bool, or None if payload too short.
        Feature names: 'audio', 'sd', 'usb', 'slave:gunfx', 'slave:lightfx',
                       'slave:gearcontrol'
    """
    # Core header: 20 bytes (extended) or 12 bytes (legacy)
    if len(payload) >= 22:  # 20-byte header + 2 min module data
        module_data = payload[20:]
    elif len(payload) >= 14:  # 12-byte header + 2 min module data
        module_data = payload[12:]
    else:
        return None

    if len(module_data) < 2:
        return None

    flags = module_data[0]
    slave_mask = module_data[1]

    return {
        'audio': bool(flags & 0x02),
        'flash': bool(flags & 0x04),
        'usb': bool(flags & 0x08),
        'sd': bool(flags & 0x10),
        'slave:gunfx': bool(slave_mask & 0x01),
        'slave:lightfx': bool(slave_mask & 0x02),
        'slave:gearcontrol': bool(slave_mask & 0x04),
    }
