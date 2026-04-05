"""
Command builder package — split by controller module.

Re-exports all public classes and constants for backward compatibility.
External code can continue to use:
    from tests.framework.commands import CommandBuilder, GunFxCommands, ...
"""

from .core import CommandBuilder
from .gunfx import GunFxCommands
from .lightfx import LightFxCommands
from .gearcontrol import GearControlCommands
from .hubfx import HubFxCommands

# Re-export specification constants used by external code
from ._common import (
    SERVO_ID_MIN, SERVO_ID_MAX, SERVO_PULSE_MIN, SERVO_PULSE_MAX,
    LED_CHANNEL_MIN, LED_CHANNEL_MAX, LED_BRIGHTNESS_MAX,
    TRIGGER_RPM_MIN, TRIGGER_RPM_MAX, U16_MAX,
    GEAR_ID_MIN, GEAR_ID_MAX, GEAR_SERVO_ID_MIN, GEAR_SERVO_ID_MAX,
    GEAR_ACTION_RETRACT, GEAR_ACTION_DEPLOY, GEAR_ACTION_STOP,
)

__all__ = [
    'CommandBuilder',
    'GunFxCommands',
    'LightFxCommands',
    'GearControlCommands',
    'HubFxCommands',
    # Constants
    'SERVO_ID_MIN', 'SERVO_ID_MAX', 'SERVO_PULSE_MIN', 'SERVO_PULSE_MAX',
    'LED_CHANNEL_MIN', 'LED_CHANNEL_MAX', 'LED_BRIGHTNESS_MAX',
    'TRIGGER_RPM_MIN', 'TRIGGER_RPM_MAX', 'U16_MAX',
    'GEAR_ID_MIN', 'GEAR_ID_MAX', 'GEAR_SERVO_ID_MIN', 'GEAR_SERVO_ID_MAX',
    'GEAR_ACTION_RETRACT', 'GEAR_ACTION_DEPLOY', 'GEAR_ACTION_STOP',
]
