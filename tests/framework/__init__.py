"""
ScaleFX Test Framework

Common connectivity and protocol handling for testing ScaleFX controllers.
"""

from .connection import ScaleFXConnection, find_ports, find_scalefx_ports
from .protocol import cobs_encode, cobs_decode, crc8
from .packets import CorePacket, GunFxPacket, LightFxPacket, GearControlPacket, CoreError, GunFxError, LightFxError, GearControlError, DoorMode, DoorState, GearErrorReason, GearSeqPhase, LandingLightPhase, SmokeErrorReason, TAG_ASYNC
from .commands import CommandBuilder, GunFxCommands, LightFxCommands, GearControlCommands

__all__ = [
    'ScaleFXConnection',
    'find_ports', 'find_scalefx_ports',
    'cobs_encode', 'cobs_decode', 'crc8',
    'CorePacket', 'GunFxPacket', 'LightFxPacket', 'GearControlPacket',
    'CoreError', 'GunFxError', 'LightFxError', 'GearControlError', 'DoorMode', 'DoorState', 'GearErrorReason',
    'GearSeqPhase', 'LandingLightPhase', 'SmokeErrorReason', 'TAG_ASYNC',
    'CommandBuilder', 'GunFxCommands', 'LightFxCommands', 'GearControlCommands',
]
