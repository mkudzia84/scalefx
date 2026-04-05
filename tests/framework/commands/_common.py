"""Shared imports, constants, and helpers for all command builders."""

import struct
import warnings

from ..protocol import build_packet, parse_packet, u16_le, u32_le, i16_le

from ..packets import (
    CorePacket,
    GunFxPacket,
    LightFxPacket, LightFxEventType,
    GearControlPacket,
    HubFxPacket, HubFxAudio, HubFxStorage,
)

# =============================================================================
# Specification Constants (shared across builders)
# =============================================================================

# Servo range (GunFX, LightFX share these; GearControl has own range)
SERVO_ID_MIN = 1
SERVO_ID_MAX = 3
SERVO_PULSE_MIN = 500
SERVO_PULSE_MAX = 2500

# LED range (LightFX)
LED_CHANNEL_MIN = 1
LED_CHANNEL_MAX = 8
LED_BRIGHTNESS_MAX = 100

# Trigger (GunFX)
TRIGGER_RPM_MIN = 1
TRIGGER_RPM_MAX = 3000

# Generic u16 limit
U16_MAX = 65535

# GearControl specification constants
GEAR_ID_MIN = 0
GEAR_ID_MAX = 2
GEAR_SERVO_ID_MIN = 0
GEAR_SERVO_ID_MAX = 7

# Gear-all action values
GEAR_ACTION_RETRACT = 0
GEAR_ACTION_DEPLOY  = 1
GEAR_ACTION_STOP    = 2


def _warn_range(name: str, value: int, lo: int, hi: int, unit: str = "") -> None:
    """Emit UserWarning if value outside [lo, hi]."""
    if value < lo or value > hi:
        suffix = f" {unit}" if unit else ""
        warnings.warn(
            f"{name}={value} outside [{lo}-{hi}]{suffix}",
            UserWarning,
            stacklevel=3
        )


def _warn_u16(name: str, value: int) -> None:
    """Emit UserWarning if value exceeds unsigned 16-bit range."""
    if value > U16_MAX:
        warnings.warn(
            f"{name}={value} exceeds u16 max ({U16_MAX})",
            UserWarning,
            stacklevel=3
        )
