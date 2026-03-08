"""
ScaleFX Test Framework

Common connectivity and protocol handling for testing ScaleFX controllers.
"""

from .connection import ScaleFXConnection, find_ports, find_scalefx_ports
from .protocol import cobs_encode, cobs_decode, crc8, crc16_ccitt
from .packets import CorePacket, GunFxPacket, LightFxPacket, GearControlPacket, HubFxPacket, StreamPacket, CoreError, GunFxError, LightFxError, GearControlError, HubFxError, DoorMode, DoorState, GearErrorReason, GearSeqPhase, LandingLightPhase, SmokeErrorReason, SlaveType, HubFxAudio, EngineState, DiagLevel, TAG_ASYNC
from .commands import CommandBuilder, GunFxCommands, LightFxCommands, GearControlCommands, HubFxCommands

__all__ = [
    'ScaleFXConnection',
    'find_ports', 'find_scalefx_ports',
    'cobs_encode', 'cobs_decode', 'crc8', 'crc16_ccitt',
    'CorePacket', 'GunFxPacket', 'LightFxPacket', 'GearControlPacket', 'HubFxPacket', 'StreamPacket',
    'CoreError', 'GunFxError', 'LightFxError', 'GearControlError', 'HubFxError',
    'DoorMode', 'DoorState', 'GearErrorReason',
    'GearSeqPhase', 'LandingLightPhase', 'SmokeErrorReason',
    'SlaveType', 'HubFxAudio', 'EngineState', 'DiagLevel', 'TAG_ASYNC',
    'CommandBuilder', 'GunFxCommands', 'LightFxCommands', 'GearControlCommands', 'HubFxCommands',
]
