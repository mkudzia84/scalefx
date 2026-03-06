"""
High-Level Command Builders

Provides friendly interfaces for building protocol commands.

All commands include parameter validation warnings when values exceed
hardware specifications.
"""

import warnings
from .protocol import build_packet, u16_le, i16_le, u32_le
from .packets import CorePacket, GunFxPacket, LightFxPacket, LightFxEventType, GearControlPacket


# =============================================================================
# Specification Constants
# =============================================================================

# Servo specs (standard hobby servo)
SERVO_PULSE_MIN = 500      # µs - absolute minimum
SERVO_PULSE_MAX = 2500     # µs - absolute maximum
SERVO_ID_MIN = 1
SERVO_ID_MAX = 3

# LED specs
LED_CHANNEL_MIN = 1
LED_CHANNEL_MAX = 8
LED_BRIGHTNESS_MAX = 100

# GunFX trigger
TRIGGER_RPM_MIN = 1
TRIGGER_RPM_MAX = 3000     # RPM

# u16 limits
U16_MAX = 65535


def _warn_range(name: str, value: int, min_val: int, max_val: int, unit: str = "") -> None:
    """Emit warning if value is outside expected range."""
    if value < min_val or value > max_val:
        unit_str = f" {unit}" if unit else ""
        warnings.warn(
            f"{name}={value}{unit_str} is outside spec range [{min_val}-{max_val}]",
            UserWarning,
            stacklevel=3
        )


def _warn_u16(name: str, value: int) -> None:
    """Warn if value exceeds u16 max."""
    if value < 0 or value > U16_MAX:
        warnings.warn(
            f"{name}={value} exceeds u16 range [0-{U16_MAX}]",
            UserWarning,
            stacklevel=3
        )


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

    @staticmethod
    def i2c_scan() -> bytes:
        """Request I2C bus scan.
        
        Scans the I2C bus for expected devices registered with the controller
        and reports any additional devices found. Returns an I2C_SCAN_RESULT
        packet instead of ACK.
        
        Requires that the controller has I2C scan enabled via PicoServer.
        If not supported, returns NACK NOT_SUPPORTED.
        
        Response wire format:
          [numExpected:u8]
          Per expected device × N (3 bytes each):
            [address:u8][found:u8][identified:u8]
          [numExtra:u8]
          Per extra device × M (1 byte each):
            [address:u8]
        """
        return build_packet(CorePacket.I2C_SCAN)


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
    def smoke_settings(pulsing: bool, speed: int, pulse_high: int,
                       pulse_low: int, pulse_ms: int, spindown_ms: int) -> bytes:
        """
        Configure smoke fan behavior.
        
        Args:
            pulsing: Enable pulsing mode (True) or constant speed (False)
            speed: Fan PWM speed (0-255, where 255 = 100%)
            pulse_high: High speed during pulse (0-255)
            pulse_low: Low speed between pulses (0-255)
            pulse_ms: Pulse duration in milliseconds
            spindown_ms: Spindown delay in milliseconds after trigger off
            
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


class LightFxCommands(CommandBuilder):
    """LightFX-specific commands."""
    
    @staticmethod
    def led_set(channel: int, brightness: int) -> bytes:
        """
        Set LED brightness.
        
        Args:
            channel: LED channel (1-8)
            brightness: Brightness (0-100%, where 0=off, 100=full)
            
        Warnings:
            Emits UserWarning if channel not in [1-8].
            Emits UserWarning if brightness not in [0-100].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_range("brightness", brightness, 0, LED_BRIGHTNESS_MAX)
        return build_packet(LightFxPacket.LED_SET, bytes([channel, brightness]))
    
    @staticmethod
    def led_off(channel: int = 0) -> bytes:
        """
        Turn off LED(s).
        
        Args:
            channel: LED channel (1-8), or 0 for all channels
            
        Warnings:
            Emits UserWarning if channel not in [0-8].
        """
        _warn_range("channel", channel, 0, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_OFF, bytes([channel]))
    
    @staticmethod
    def led_seq_clear(channel: int) -> bytes:
        """
        Clear LED sequence (removes all queued events).
        
        Args:
            channel: LED channel (1-8)
            
        Warnings:
            Emits UserWarning if channel not in [1-8].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_SEQ_CLEAR, bytes([channel]))
    
    @staticmethod
    def led_seq_add(channel: int, event_type: int, param1: int = 0,
                    param2: int = 0, param3: int = 0, param4: int = 0) -> bytes:
        """
        Add event to LED sequence (low-level).
        
        Args:
            channel: LED channel (1-8)
            event_type: Event type constant (see LightFxEventType)
            param1: First parameter (u16, typically duration_ms or interval_ms)
            param2: Second parameter (u16, typically duration_ms)
            param3: Third parameter (u8, typically brightness 0-100)
            param4: Fourth parameter (u8, typically duty cycle 0-100)
            
        Note:
            Prefer using led_seq_add_on(), led_seq_add_flash(), etc. for
            type-safe event creation with proper documentation.
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("param1", param1)
        _warn_u16("param2", param2)
        _warn_range("param3", param3, 0, 100)
        _warn_range("param4", param4, 0, 100)
        payload = bytes([channel, event_type])
        payload += u16_le(param1) + u16_le(param2)
        payload += bytes([param3, param4])
        return build_packet(LightFxPacket.LED_SEQ_ADD, payload)
    
    @staticmethod
    def led_seq_add_on(channel: int, duration_ms: int, brightness: int) -> bytes:
        """
        Add ON event to sequence.
        
        Args:
            channel: LED channel (1-8)
            duration_ms: Duration in milliseconds (0 = infinite, max 65535ms ≈ 65s)
            brightness: LED brightness (0-100%)
            
        Warnings:
            Emits UserWarning if brightness not in [0-100].
            Emits UserWarning if duration_ms exceeds 65535.
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("duration_ms", duration_ms)
        _warn_range("brightness", brightness, 0, LED_BRIGHTNESS_MAX)
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.ON,
            duration_ms, 0, brightness, 0
        )
    
    @staticmethod
    def led_seq_add_off(channel: int, duration_ms: int) -> bytes:
        """
        Add OFF event to sequence.
        
        Args:
            channel: LED channel (1-8)
            duration_ms: Duration in milliseconds (0 = infinite, max 65535ms ≈ 65s)
            
        Warnings:
            Emits UserWarning if duration_ms exceeds 65535.
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("duration_ms", duration_ms)
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.OFF,
            duration_ms, 0, 0, 0
        )
    
    @staticmethod
    def led_seq_add_flash(channel: int, interval_ms: int, duration_ms: int,
                          brightness: int, duty: int = 50) -> bytes:
        """
        Add FLASH event to sequence.
        
        Args:
            channel: LED channel (1-8)
            interval_ms: On/off cycle interval in milliseconds (full cycle = 2×interval)
            duration_ms: Total flash duration in milliseconds (0 = infinite)
            brightness: LED brightness when on (0-100%)
            duty: Duty cycle percentage (0-100, default 50 = equal on/off)
            
        Warnings:
            Emits UserWarning if brightness not in [0-100].
            Emits UserWarning if duty not in [0-100].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("interval_ms", interval_ms)
        _warn_u16("duration_ms", duration_ms)
        _warn_range("brightness", brightness, 0, LED_BRIGHTNESS_MAX)
        _warn_range("duty", duty, 0, 100, "%")
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.FLASH,
            interval_ms, duration_ms, brightness, duty
        )
    
    @staticmethod
    def led_seq_add_fade_in(channel: int, duration_ms: int, brightness: int) -> bytes:
        """
        Add FADE_IN event to sequence (fade from off to target brightness).
        
        Args:
            channel: LED channel (1-8)
            duration_ms: Fade duration in milliseconds
            brightness: Target brightness (0-100%)
            
        Warnings:
            Emits UserWarning if brightness not in [0-100].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("duration_ms", duration_ms)
        _warn_range("brightness", brightness, 0, LED_BRIGHTNESS_MAX)
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.FADE_IN,
            duration_ms, 0, brightness, 0
        )
    
    @staticmethod
    def led_seq_add_fade_out(channel: int, duration_ms: int, brightness: int) -> bytes:
        """
        Add FADE_OUT event to sequence (fade from start brightness to off).
        
        Args:
            channel: LED channel (1-8)
            duration_ms: Fade duration in milliseconds
            brightness: Starting brightness (0-100%)
            
        Warnings:
            Emits UserWarning if brightness not in [0-100].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("duration_ms", duration_ms)
        _warn_range("brightness", brightness, 0, LED_BRIGHTNESS_MAX)
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.FADE_OUT,
            duration_ms, 0, brightness, 0
        )
    
    @staticmethod
    def led_seq_start(channel: int = 0) -> bytes:
        """
        Start LED sequence playback.
        
        Args:
            channel: LED channel (1-8), or 0 to start all sequences
            
        Warnings:
            Emits UserWarning if channel not in [0-8].
        """
        _warn_range("channel", channel, 0, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_SEQ_START, bytes([channel]))
    
    @staticmethod
    def led_seq_stop(channel: int = 0) -> bytes:
        """
        Stop LED sequence playback.
        
        Args:
            channel: LED channel (1-8), or 0 to stop all sequences
            
        Warnings:
            Emits UserWarning if channel not in [0-8].
        """
        _warn_range("channel", channel, 0, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_SEQ_STOP, bytes([channel]))
    
    @staticmethod
    def led_seq_restart(channel: int) -> bytes:
        """
        Restart LED sequence from beginning.
        
        Args:
            channel: LED channel (1-8)
            
        Warnings:
            Emits UserWarning if channel not in [1-8].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_SEQ_RESTART, bytes([channel]))
    
    @staticmethod
    def led_seq_status(channel: int) -> bytes:
        """
        Request LED sequence status.
        
        Args:
            channel: LED channel (1-8)
            
        Returns:
            Response packet containing: playing (bool), eventCount (u8),
            currentIndex (u8), loopCount (u32)
            
        Warnings:
            Emits UserWarning if channel not in [1-8].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_SEQ_STATUS, bytes([channel]))
    
    @staticmethod
    def led_seq_queue(channel: int) -> bytes:
        """
        Request LED sequence queue details.
        
        Args:
            channel: LED channel (1-8)
            
        Returns:
            Response packet containing event queue listing with
            type, duration_ms, and param1 for each event (up to 24 events).
            
        Warnings:
            Emits UserWarning if channel not in [1-8].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_SEQ_QUEUE, bytes([channel]))
    
    @staticmethod
    def led_status() -> bytes:
        """
        Request status of all LED channels.
        
        Returns:
            Response packet with per-channel status:
            - brightness (0-100%)
            - seqPlaying (bool)
            - eventCount (u8)
        """
        return build_packet(LightFxPacket.LED_STATUS)
    
    @staticmethod
    def led_master_brightness(pct: int) -> bytes:
        """
        Set master brightness for all LED channels.
        
        Scales all LED output by the given percentage. Does not
        change individual channel brightness values, only the
        final physical output.
        
        Args:
            pct: Brightness percentage (0-100, where 0=off, 100=full)
            
        Warnings:
            Emits UserWarning if pct not in [0-100].
        """
        _warn_range("pct", pct, 0, 100, "%")
        return build_packet(LightFxPacket.LED_MASTER_BRIGHTNESS, bytes([pct]))
    
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
        return build_packet(LightFxPacket.SERVO_SET, payload)
    
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
        return build_packet(LightFxPacket.SERVO_SETTINGS, payload)
    
    # =========================================================================
    # Landing Light Control
    # =========================================================================
    
    @staticmethod
    def landing_light_bind(slot: int, servo_id: int, led_channel: int,
                           deploy_us: int, retract_us: int,
                           brightness: int = 100) -> bytes:
        """
        Bind a servo and LED channel as a landing light pair.
        
        Args:
            slot: Landing light slot (1-3)
            servo_id: Servo ID (1-3)
            led_channel: LED channel (1-8)
            deploy_us: Servo position when deployed (µs)
            retract_us: Servo position when retracted (µs)
            brightness: LED brightness when light is on (0-100%, default 100)
            
        Warnings:
            Emits UserWarning if slot not in [1-3].
            Emits UserWarning if servo_id not in [1-3].
            Emits UserWarning if led_channel not in [1-8].
        """
        _warn_range("slot", slot, 1, 3)
        _warn_range("servo_id", servo_id, SERVO_ID_MIN, SERVO_ID_MAX)
        _warn_range("led_channel", led_channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_range("deploy_us", deploy_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("retract_us", retract_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("brightness", brightness, 0, LED_BRIGHTNESS_MAX)
        payload = (bytes([slot, servo_id, led_channel]) +
                   u16_le(deploy_us) + u16_le(retract_us) +
                   bytes([brightness]))
        return build_packet(LightFxPacket.LANDING_LIGHT_BIND, payload)
    
    @staticmethod
    def landing_light_unbind(slot: int = 0) -> bytes:
        """
        Unbind a landing light slot.
        
        Args:
            slot: Landing light slot (1-3), or 0 to unbind all
        """
        _warn_range("slot", slot, 0, 3)
        return build_packet(LightFxPacket.LANDING_LIGHT_UNBIND, bytes([slot]))
    
    @staticmethod
    def landing_light_deploy(slot: int = 0) -> bytes:
        """
        Deploy landing light (servo moves to deploy position, light on when arrived).
        
        Args:
            slot: Landing light slot (1-3), or 0 for all configured slots
        """
        _warn_range("slot", slot, 0, 3)
        return build_packet(LightFxPacket.LANDING_LIGHT_DEPLOY, bytes([slot]))
    
    @staticmethod
    def landing_light_retract(slot: int = 0) -> bytes:
        """
        Retract landing light (light off immediately, then servo moves to retract position).
        
        Args:
            slot: Landing light slot (1-3), or 0 for all configured slots
        """
        _warn_range("slot", slot, 0, 3)
        return build_packet(LightFxPacket.LANDING_LIGHT_RETRACT, bytes([slot]))


# =============================================================================
# GearControl Specification Constants
# =============================================================================

GEAR_ID_MIN = 0
GEAR_ID_MAX = 2
GEAR_SERVO_ID_MIN = 0
GEAR_SERVO_ID_MAX = 7

# Gear-all action values
GEAR_ACTION_RETRACT = 0
GEAR_ACTION_DEPLOY  = 1
GEAR_ACTION_STOP    = 2


class GearControlCommands(CommandBuilder):
    """GearControl-specific commands."""

    @staticmethod
    def gear_deploy(gear_id: int) -> bytes:
        """
        Deploy landing gear (open doors, extend, optionally close doors).
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        return build_packet(GearControlPacket.GEAR_DEPLOY, bytes([gear_id]))

    @staticmethod
    def gear_retract(gear_id: int) -> bytes:
        """
        Retract landing gear (open doors, retract, close doors).
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        return build_packet(GearControlPacket.GEAR_RETRACT, bytes([gear_id]))

    @staticmethod
    def gear_stop(gear_id: int) -> bytes:
        """
        Emergency stop motor for specified gear.
        
        Args:
            gear_id: Gear index (0-2)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        return build_packet(GearControlPacket.GEAR_STOP, bytes([gear_id]))

    @staticmethod
    def gear_all(action: int) -> bytes:
        """
        Deploy, retract, or stop all gears simultaneously.
        
        Args:
            action: 0=retract, 1=deploy, 2=stop
        """
        _warn_range("action", action, GEAR_ACTION_RETRACT, GEAR_ACTION_STOP)
        return build_packet(GearControlPacket.GEAR_ALL, bytes([action]))

    @staticmethod
    def servo_set(servo_id: int, pulse_us: int) -> bytes:
        """
        Set servo position.
        
        Args:
            servo_id: Servo ID (0-7)
            pulse_us: Pulse width in microseconds (500-2500µs)
        """
        _warn_range("servo_id", servo_id, GEAR_SERVO_ID_MIN, GEAR_SERVO_ID_MAX)
        _warn_range("pulse_us", pulse_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        payload = bytes([servo_id]) + u16_le(pulse_us)
        return build_packet(GearControlPacket.SERVO_SET, payload)

    @staticmethod
    def servo_settings(servo_id: int, min_us: int, max_us: int,
                       speed: int = 4000, accel: int = 8000,
                       decel: int = 8000) -> bytes:
        """
        Configure servo parameters (matches GunFX/LightFX SRV_SETTINGS pattern).
        
        Args:
            servo_id: Servo ID (0-7)
            min_us: Minimum pulse width in µs (500-2500)
            max_us: Maximum pulse width in µs (500-2500)
            speed: Maximum speed in µs/second (default 4000)
            accel: Acceleration in µs/second² (default 8000)
            decel: Deceleration in µs/second² (default 8000)
        """
        _warn_range("servo_id", servo_id, GEAR_SERVO_ID_MIN, GEAR_SERVO_ID_MAX)
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
        return build_packet(GearControlPacket.SRV_SETTINGS, payload)

    @staticmethod
    def gear_config(gear_id: int, flags: int, stall_current_mA: int,
                    timeout_ms: int) -> bytes:
        """
        Configure gear behavior.
        
        Args:
            gear_id: Gear index (0-2)
            flags: Config flags (bit 0: close doors on retract,
                   bit 1: close doors on deploy, bit 2: has yaw)
            stall_current_mA: Motor stall current threshold in milliamps
            timeout_ms: Maximum motor run time in milliseconds
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        _warn_u16("stall_current_mA", stall_current_mA)
        _warn_u16("timeout_ms", timeout_ms)
        payload = (bytes([gear_id, flags]) +
                   u16_le(stall_current_mA) + u16_le(timeout_ms))
        return build_packet(GearControlPacket.GEAR_CONFIG, payload)

    @staticmethod
    def door_config(gear_id: int, open0_us: int, close0_us: int,
                    open1_us: int, close1_us: int) -> bytes:
        """
        Configure door servo positions for a gear.
        
        Args:
            gear_id: Gear index (0-2)
            open0_us: Door servo 0 open position in µs
            close0_us: Door servo 0 closed position in µs
            open1_us: Door servo 1 open position in µs
            close1_us: Door servo 1 closed position in µs
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        _warn_range("open0_us", open0_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("close0_us", close0_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("open1_us", open1_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("close1_us", close1_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        payload = (bytes([gear_id]) +
                   u16_le(open0_us) + u16_le(close0_us) +
                   u16_le(open1_us) + u16_le(close1_us))
        return build_packet(GearControlPacket.DOOR_CONFIG, payload)

    @staticmethod
    def yaw_config(gear_id: int, neutral_us: int, min_us: int,
                   max_us: int) -> bytes:
        """
        Configure yaw servo.
        
        Args:
            gear_id: Associated gear index (yaw active when this gear deployed)
            neutral_us: Neutral/center position in µs
            min_us: Minimum yaw position in µs
            max_us: Maximum yaw position in µs
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        _warn_range("neutral_us", neutral_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("min_us", min_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        _warn_range("max_us", max_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        payload = (bytes([gear_id]) +
                   u16_le(neutral_us) + u16_le(min_us) + u16_le(max_us))
        return build_packet(GearControlPacket.YAW_CONFIG, payload)

    @staticmethod
    def yaw_input(position_us: int) -> bytes:
        """
        Set yaw position (only effective when associated gear is deployed).
        
        Args:
            position_us: Yaw servo position in µs (clamped to configured range)
        """
        _warn_range("position_us", position_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX, "µs")
        return build_packet(GearControlPacket.YAW_INPUT, u16_le(position_us))

    @staticmethod
    def gear_calibrate(gear_id: int) -> bytes:
        """
        Start stall current calibration for a gear.
        
        Runs the motor in each direction to detect stall current.
        The calibrated value is saved and reported in STATUS responses.
        Progress updates are sent as GEAR_CALIB_STATUS packets.
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        return build_packet(GearControlPacket.GEAR_CALIBRATE, bytes([gear_id]))

    @staticmethod
    def gear_calib_cancel(gear_id: int) -> bytes:
        """
        Cancel an in-progress stall current calibration.
        
        Stops the motor and returns the gear to UNKNOWN state.
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        return build_packet(GearControlPacket.GEAR_CALIB_CANCEL, bytes([gear_id]))

    @staticmethod
    def battery_config(enabled: bool, auto_deploy: bool = False) -> bytes:
        """
        Configure battery monitoring and low-voltage behavior.
        
        Battery monitoring is disabled by default. The host must explicitly
        enable it via this command when a battery is physically connected
        (hardware jumper). When disabled, STATUS reports 0mV and auto-deploy
        is inactive regardless of the auto_deploy flag.
        
        When enabled with auto_deploy, all landing gears will automatically
        deploy when the battery voltage drops below the low warning threshold
        (3.2V/cell for LiPo). This is a safety feature for RC aircraft.
        
        Args:
            enabled: True to enable battery voltage monitoring, False to disable
            auto_deploy: True to enable auto-deploy on low voltage (only effective when enabled)
        """
        return build_packet(GearControlPacket.BATTERY_CONFIG,
                            bytes([1 if enabled else 0, 1 if auto_deploy else 0]))

    @staticmethod
    def door_mode(gear_id: int, mode: int, delay_ms: int = 500) -> bytes:
        """
        Configure door activation mode for a gear.
        
        Controls how door servos are used during deploy/retract sequences:
          0 = NONE        No door servos (motor only)
          1 = SINGLE      One door servo (servo 0 only)
          2 = DUAL_SYNC   Two doors, simultaneous (default)
          3 = DUAL_DELAY  Two doors, door 1 starts after delay_ms
          4 = DUAL_SEQ    Two doors, door 1 starts after door 0 completes
        
        For DUAL_DELAY/DUAL_SEQ: doors open 0→1, close 1→0 (like real aircraft).
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
            mode: DoorMode value (0-4)
            delay_ms: Delay between doors in ms (DUAL_DELAY only, default 500)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        _warn_range("mode", mode, 0, 4)
        _warn_u16("delay_ms", delay_ms)
        payload = bytes([gear_id, mode]) + u16_le(delay_ms)
        return build_packet(GearControlPacket.DOOR_MODE, payload)


