"""
High-Level Command Builders

Provides friendly interfaces for building protocol commands.
"""

from .protocol import build_packet, u16_le, i16_le, u32_le
from .packets import CorePacket, GunFxPacket, LightFxPacket, LightFxEventType


class CommandBuilder:
    """Base command builder with core system commands."""
    
    @staticmethod
    def init() -> bytes:
        """Build INIT packet."""
        return build_packet(CorePacket.INIT)
    
    @staticmethod
    def shutdown() -> bytes:
        """Build SHUTDOWN packet."""
        return build_packet(CorePacket.SHUTDOWN)
    
    @staticmethod
    def keepalive() -> bytes:
        """Build KEEPALIVE packet."""
        return build_packet(CorePacket.KEEPALIVE)
    
    @staticmethod
    def reboot() -> bytes:
        """Build REBOOT packet."""
        return build_packet(CorePacket.REBOOT)
    
    @staticmethod
    def bootsel() -> bytes:
        """Build BOOTSEL packet."""
        return build_packet(CorePacket.BOOTSEL)
    
    @staticmethod
    def status_req() -> bytes:
        """Build STATUS_REQ packet."""
        return build_packet(CorePacket.STATUS_REQ)


class GunFxCommands(CommandBuilder):
    """GunFX-specific commands."""
    
    @staticmethod
    def trigger_on(rpm: int) -> bytes:
        """
        Start firing at specified RPM.
        
        Args:
            rpm: Firing rate (1-3000 RPM)
        """
        return build_packet(GunFxPacket.TRIGGER_ON, u16_le(rpm))
    
    @staticmethod
    def trigger_off(fan_delay_ms: int = 3000) -> bytes:
        """
        Stop firing.
        
        Args:
            fan_delay_ms: Delay before fan turns off (ms)
        """
        return build_packet(GunFxPacket.TRIGGER_OFF, u16_le(fan_delay_ms))
    
    @staticmethod
    def servo_set(servo_id: int, pulse_us: int) -> bytes:
        """
        Set servo position.
        
        Args:
            servo_id: Servo ID (1-3)
            pulse_us: Pulse width (500-2500µs), or -1 for detach
        """
        payload = bytes([servo_id]) + i16_le(pulse_us)
        return build_packet(GunFxPacket.SERVO_SET, payload)
    
    @staticmethod
    def servo_settings(servo_id: int, min_us: int, max_us: int,
                       speed: int = 4000, accel: int = 8000, decel: int = 8000) -> bytes:
        """
        Configure servo parameters.
        
        Args:
            servo_id: Servo ID (1-3)
            min_us: Minimum pulse width (µs)
            max_us: Maximum pulse width (µs)
            speed: Max speed (µs/s)
            accel: Acceleration (µs/s²)
            decel: Deceleration (µs/s²)
        """
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
            jerk_us: Base jerk amount (µs)
            variance_us: Random variance (µs)
        """
        payload = bytes([servo_id]) + u16_le(jerk_us) + u16_le(variance_us)
        return build_packet(GunFxPacket.SERVO_RECOIL, payload)
    
    @staticmethod
    def smoke_heat(on: bool) -> bytes:
        """
        Enable/disable smoke heater.
        
        Args:
            on: True to enable, False to disable
        """
        return build_packet(GunFxPacket.SMOKE_HEAT, bytes([1 if on else 0]))
    
    @staticmethod
    def smoke_settings(pulsing: bool, speed: int, pulse_high: int,
                       pulse_low: int, pulse_ms: int, spindown_ms: int) -> bytes:
        """
        Configure smoke fan behavior.
        
        Args:
            pulsing: Enable pulsing mode
            speed: Fan speed (0-255)
            pulse_high: High speed during pulse
            pulse_low: Low speed between pulses
            pulse_ms: Pulse duration (ms)
            spindown_ms: Spindown delay (ms)
        """
        payload = bytes([1 if pulsing else 0, speed, pulse_high, pulse_low])
        payload += u16_le(pulse_ms) + u16_le(spindown_ms)
        return build_packet(GunFxPacket.SMOKE_SETTINGS, payload)


class LightFxCommands(CommandBuilder):
    """LightFX-specific commands."""
    
    @staticmethod
    def led_set(channel: int, brightness: int) -> bytes:
        """
        Set LED brightness.
        
        Args:
            channel: LED channel (1-8)
            brightness: Brightness (0-255)
        """
        return build_packet(LightFxPacket.LED_SET, bytes([channel, brightness]))
    
    @staticmethod
    def led_off(channel: int = 0) -> bytes:
        """
        Turn off LED(s).
        
        Args:
            channel: LED channel (1-8), or 0 for all
        """
        return build_packet(LightFxPacket.LED_OFF, bytes([channel]))
    
    @staticmethod
    def led_seq_clear(channel: int) -> bytes:
        """
        Clear LED sequence.
        
        Args:
            channel: LED channel (1-8)
        """
        return build_packet(LightFxPacket.LED_SEQ_CLEAR, bytes([channel]))
    
    @staticmethod
    def led_seq_add(channel: int, event_type: int, param1: int = 0,
                    param2: int = 0, param3: int = 0, param4: int = 0) -> bytes:
        """
        Add event to LED sequence.
        
        Args:
            channel: LED channel (1-8)
            event_type: Event type (see LightFxEventType)
            param1-4: Event-specific parameters
        """
        payload = bytes([channel, event_type])
        payload += u16_le(param1) + u16_le(param2)
        payload += bytes([param3, param4])
        return build_packet(LightFxPacket.LED_SEQ_ADD, payload)
    
    @staticmethod
    def led_seq_add_on(channel: int, duration_ms: int, brightness: int) -> bytes:
        """Add ON event to sequence."""
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.ON,
            duration_ms, 0, brightness, 0
        )
    
    @staticmethod
    def led_seq_add_off(channel: int, duration_ms: int) -> bytes:
        """Add OFF event to sequence."""
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.OFF,
            duration_ms, 0, 0, 0
        )
    
    @staticmethod
    def led_seq_add_flash(channel: int, interval_ms: int, duration_ms: int,
                          brightness: int, duty: int = 50) -> bytes:
        """Add FLASH event to sequence."""
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.FLASH,
            interval_ms, duration_ms, brightness, duty
        )
    
    @staticmethod
    def led_seq_add_fade_in(channel: int, duration_ms: int, brightness: int) -> bytes:
        """Add FADE_IN event to sequence."""
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.FADE_IN,
            duration_ms, 0, brightness, 0
        )
    
    @staticmethod
    def led_seq_add_fade_out(channel: int, duration_ms: int, brightness: int) -> bytes:
        """Add FADE_OUT event to sequence."""
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.FADE_OUT,
            duration_ms, 0, brightness, 0
        )
    
    @staticmethod
    def led_seq_start(channel: int = 0) -> bytes:
        """
        Start LED sequence playback.
        
        Args:
            channel: LED channel (1-8), or 0 for all
        """
        return build_packet(LightFxPacket.LED_SEQ_START, bytes([channel]))
    
    @staticmethod
    def led_seq_stop(channel: int = 0) -> bytes:
        """
        Stop LED sequence playback.
        
        Args:
            channel: LED channel (1-8), or 0 for all
        """
        return build_packet(LightFxPacket.LED_SEQ_STOP, bytes([channel]))
    
    @staticmethod
    def servo_set(servo_id: int, pulse_us: int) -> bytes:
        """
        Set servo position.
        
        Args:
            servo_id: Servo ID (1-3)
            pulse_us: Pulse width (500-2500µs), or -1 for detach
        """
        payload = bytes([servo_id]) + i16_le(pulse_us)
        return build_packet(LightFxPacket.SERVO_SET, payload)
    
    @staticmethod
    def servo_settings(servo_id: int, min_us: int, max_us: int,
                       speed: int = 4000, accel: int = 8000, decel: int = 8000) -> bytes:
        """Configure servo parameters."""
        payload = (bytes([servo_id]) +
                   u16_le(min_us) + u16_le(max_us) +
                   u16_le(speed) + u16_le(accel) + u16_le(decel))
        return build_packet(LightFxPacket.SERVO_SETTINGS, payload)
    
    @staticmethod
    def power_status() -> bytes:
        """Request power status from INA226."""
        return build_packet(LightFxPacket.POWER_STATUS)
