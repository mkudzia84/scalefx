"""
ScaleFX CLI Response Parsers

Per-module response packet parsing, re-exported here for backward compatibility.
Callers use ``from .. import parsers`` then ``parsers.func_name()``.
"""

from .core import (
    packet_type_name,
    error_name,
    parse_generic_payload,
    parse_error_payload,
    parse_log_message,
    parse_status_payload,
    parse_i2c_scan_result,
    InitReadyInfo,
    parse_init_ready,
    print_init_ready_info,
)
from .gearcontrol import (
    parse_gear_calib_status,
    parse_gear_seq_status,
    parse_gear_door_status,
)
from .hubfx import extract_hubfx_features
from .lightfx import (
    parse_landing_light_status,
    parse_led_seq_status,
    parse_led_seq_queue,
    parse_led_status,
)

__all__ = [
    # Core
    "packet_type_name",
    "error_name",
    "parse_generic_payload",
    "parse_error_payload",
    "parse_log_message",
    "parse_status_payload",
    "parse_i2c_scan_result",
    "InitReadyInfo",
    "parse_init_ready",
    "print_init_ready_info",
    # GearControl
    "parse_gear_calib_status",
    "parse_gear_seq_status",
    "parse_gear_door_status",
    # HubFX
    "extract_hubfx_features",
    # LightFX
    "parse_landing_light_status",
    "parse_led_seq_status",
    "parse_led_seq_queue",
    "parse_led_status",
]
