"""GunFX command builders."""

from ._common import (
    warnings, build_packet, u16_le, i16_le,
    _warn_range, _warn_u16,
    GunFxPacket,
    SERVO_ID_MIN, SERVO_ID_MAX, SERVO_PULSE_MIN, SERVO_PULSE_MAX,
    TRIGGER_RPM_MIN, TRIGGER_RPM_MAX,
)
from .core import CommandBuilder


class GunFxCommands(CommandBuilder):
    """GunFX-specific commands."""

    @staticmethod
    def trigger_on(rpm: int) -> bytes:
        """
        Start firing at specified RPM.
        
        Args:
            rpm: Firing rate in rounds per minute (1-3000 RPM)
            
        Warnings:
            Emits UserWarning if rpm is outside [1-3000] range.
        """
        _warn_range("rpm", rpm, TRIGGER_RPM_MIN, TRIGGER_RPM_MAX, "RPM")
        _warn_u16("rpm", rpm)
        return build_packet(GunFxPacket.TRIGGER_ON, u16_le(rpm))
    
    @staticmethod
    def trigger_off(fan_delay_ms: int = 3000) -> bytes:
        """
        Stop firing.
        
        Args:
            fan_delay_ms: Delay before smoke fan turns off, in milliseconds (default 3000ms)
            
        Warnings:
            Emits UserWarning if delay exceeds u16 max (65535ms ≈ 65s).
        """
        _warn_u16("fan_delay_ms", fan_delay_ms)
        return build_packet(GunFxPacket.TRIGGER_OFF, u16_le(fan_delay_ms))
    
    @staticmethod
    def servo_set(servo_id: int, pulse_us: int) -> bytes:
        """
        Set servo position.
        
        Args:
            servo_id: Servo ID (1-3)
            pulse_us: Pulse width in microseconds (500-2500µs), or -1 to detach
            
        Warnings:
            Emits UserWarning if servo_id not in [1-3].
            Emits UserWarning if pulse_us outside [500-2500] (except -1 for detach).
        """
        _warn_range("servo_id", servo_id, SERVO_ID_MIN, SERVO_ID_MAX)
        if pulse_us != -1:
            _warn_range("pulse_us", pulse_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        payload = bytes([servo_id]) + i16_le(pulse_us)
        return build_packet(GunFxPacket.SERVO_SET, payload)
    
    @staticmethod
    def servo_settings(servo_id: int, min_us: int, max_us: int,
                       speed: int = 4000, accel: int = 8000, decel: int = 8000) -> bytes:
        """
        Configure servo parameters.
        
        Args:
            servo_id: Servo ID (1-3)
            min_us: Minimum pulse width in microseconds (typ. 500-1000µs)
            max_us: Maximum pulse width in microseconds (typ. 2000-2500µs)
            speed: Maximum speed in µs/second (default 4000)
            accel: Acceleration in µs/second² (default 8000)
            decel: Deceleration in µs/second² (default 8000)
            
        Warnings:
            Emits UserWarning if servo_id not in [1-3].
            Emits UserWarning if min_us >= max_us.
            Emits UserWarning if pulse limits outside [500-2500].
        """
        _warn_range("servo_id", servo_id, SERVO_ID_MIN, SERVO_ID_MAX)
        _warn_range("min_us", min_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("max_us", max_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        if min_us >= max_us:
            warnings.warn(f"min_us={min_us} should be less than max_us={max_us}", UserWarning, stacklevel=2)
        _warn_u16("speed", speed)
        _warn_u16("accel", accel)
        _warn_u16("decel", decel)
        payload = (bytes([servo_id]) +
                   u16_le(min_us) + u16_le(max_us) +
                   u16_le(speed) + u16_le(accel) + u16_le(decel))
        return build_packet(GunFxPacket.SERVO_SETTINGS, payload)
    
    @staticmethod
    def servo_recoil(servo_id: int, jerk_us: int, variance_us: int) -> bytes:
        """
        Configure recoil jerk effect.
        
        Args:
            servo_id: Servo ID (1-3)
            jerk_us: Base jerk amount in microseconds
            variance_us: Random variance in microseconds
            
        Warnings:
            Emits UserWarning if servo_id not in [1-3].
        """
        _warn_range("servo_id", servo_id, SERVO_ID_MIN, SERVO_ID_MAX)
        _warn_u16("jerk_us", jerk_us)
        _warn_u16("variance_us", variance_us)
        payload = bytes([servo_id]) + u16_le(jerk_us) + u16_le(variance_us)
        return build_packet(GunFxPacket.SERVO_RECOIL, payload)
    
    @staticmethod
    def smoke_heat(on: bool) -> bytes:
        """
        Enable/disable smoke heater.
        
        Args:
            on: True to enable heater, False to disable
            
        Warning:
            Heater draws significant current. Ensure adequate power supply.
        """
        return build_packet(GunFxPacket.SMOKE_HEAT, bytes([1 if on else 0]))
    
    @staticmethod
    def smoke_settings(pulsing: bool = False, speed: int = 255, pulse_high: int = 255,
                       pulse_low: int = 80, pulse_ms: int = 0, spindown_ms: int = 5000) -> bytes:
        """
        Configure smoke fan behavior.
        
        Args:
            pulsing: Enable pulsing mode (True) or constant speed (False)
            speed: Fan PWM speed (0-255, where 255 = 100%)
            pulse_high: High speed during pulse (0-255)
            pulse_low: Low speed between pulses (0-255)
            pulse_ms: Pulse duration in milliseconds (0 = auto-calculate from RPM)
            spindown_ms: Spindown delay in milliseconds after trigger off
            
        Notes:
            When pulse_ms=0 (default), the firmware auto-calculates pulse
            duration as 50% of the shot interval based on the current RPM,
            clamped to 20-250ms. Set pulse_ms > 0 to override with a fixed value.
            
        Warnings:
            Emits UserWarning if speed/pulse values exceed 255.
        """
        _warn_range("speed", speed, 0, 255)
        _warn_range("pulse_high", pulse_high, 0, 255)
        _warn_range("pulse_low", pulse_low, 0, 255)
        _warn_u16("pulse_ms", pulse_ms)
        _warn_u16("spindown_ms", spindown_ms)
        payload = bytes([1 if pulsing else 0, speed, pulse_high, pulse_low])
        payload += u16_le(pulse_ms) + u16_le(spindown_ms)
        return build_packet(GunFxPacket.SMOKE_SETTINGS, payload)
    
    @staticmethod
    def smoke_reset() -> bytes:
        """Clear smoke error states (heater/fan disconnect, overcurrent)."""
        return build_packet(GunFxPacket.SMOKE_RESET)

    @staticmethod
    def smoke_current_limit(channel: int, limit_mA: int) -> bytes:
        """
        Set overcurrent protection limit for a smoke channel.

        When current exceeds the limit, PWM is automatically stepped down.
        If throttling fails to bring current under the limit, the channel
        is shut off and an overcurrent error is set.

        Args:
            channel: 0=heater, 1=fan
            limit_mA: Current limit in milliamps (0=disable protection)

        Warnings:
            Emits UserWarning if channel not in [0-1].
            Emits UserWarning if limit_mA exceeds u16 max.
        """
        _warn_range("channel", channel, 0, 1)
        _warn_u16("limit_mA", limit_mA)
        payload = bytes([channel]) + u16_le(limit_mA)
        return build_packet(GunFxPacket.SMOKE_CURRENT_LIMIT, payload)
