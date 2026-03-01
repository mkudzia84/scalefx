"""
ScaleFX Test Framework

Common connectivity and protocol handling for testing ScaleFX controllers.
"""

from .connection import ScaleFXConnection, find_ports, find_scalefx_ports
from .protocol import cobs_encode, cobs_decode, crc8
from .packets import CorePacket, GunFxPacket, LightFxPacket, CoreError, GunFxError, LightFxError
from .commands import CommandBuilder, GunFxCommands, LightFxCommands

__all__ = [
    'ScaleFXConnection',
    'find_ports', 'find_scalefx_ports',
    'cobs_encode', 'cobs_decode', 'crc8',
    'CorePacket', 'GunFxPacket', 'LightFxPacket',
    'CoreError', 'GunFxError', 'LightFxError',
    'CommandBuilder', 'GunFxCommands', 'LightFxCommands',
]
