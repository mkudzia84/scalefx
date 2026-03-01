"""
High-Level Command Builders

Provides friendly interfaces for building protocol commands.

All commands include parameter validation warnings when values exceed
hardware specifications.
"""

import warnings
from .protocol import build_packet, u16_le, i16_le, u32_le
from .packets import CorePacket, GunFxPacket, LightFxPacket, LightFxEventType


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
LED_BRIGHTNESS_MAX = 255

# GunFX trigger
TRIGGER_RPM_MIN = 1
TRIGGER_RPM_MAX = 3000     # RPM

# INA226 specs
INA226_BUS_VOLTAGE_MAX = 36000    # mV (36V max)
INA226_SHUNT_VOLTAGE_MAX = 81920  # µV (±81.92mV max shunt voltage)
INA226_SHUNT_MOHM_MIN = 1         # mΩ - practical minimum
INA226_SHUNT_MOHM_MAX = 10000     # mΩ - practical maximum (10Ω)

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
            brightness: PWM brightness (0-255, where 0=off, 255=full)
            
        Warnings:
            Emits UserWarning if channel not in [1-8].
            Emits UserWarning if brightness not in [0-255].
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
            param3: Third parameter (u8, typically brightness 0-255)
            param4: Fourth parameter (u8, typically duty cycle 0-100)
            
        Note:
            Prefer using led_seq_add_on(), led_seq_add_flash(), etc. for
            type-safe event creation with proper documentation.
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("param1", param1)
        _warn_u16("param2", param2)
        _warn_range("param3", param3, 0, 255)
        _warn_range("param4", param4, 0, 255)
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
            brightness: LED brightness (0-255)
            
        Warnings:
            Emits UserWarning if brightness not in [0-255].
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
            brightness: LED brightness when on (0-255)
            duty: Duty cycle percentage (0-100, default 50 = equal on/off)
            
        Warnings:
            Emits UserWarning if brightness not in [0-255].
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
            brightness: Target brightness (0-255)
            
        Warnings:
            Emits UserWarning if brightness not in [0-255].
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
            brightness: Starting brightness (0-255)
            
        Warnings:
            Emits UserWarning if brightness not in [0-255].
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
            - brightness (0-255)
            - seqPlaying (bool)
            - eventCount (u8)
        """
        return build_packet(LightFxPacket.LED_STATUS)
    
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
    
    @staticmethod
    def power_status() -> bytes:
        """
        Request power status from INA226.
        
        Returns:
            Response packet containing:
            - voltage_mv (u16): Bus voltage in millivolts (0-36000mV)
            - current_ma (i16): Current in milliamps (signed)
            - power_mw (u16): Power in milliwatts
            - available (u8): 1 if INA226 detected, 0 otherwise
            - shunt_mohm (u16): Configured shunt resistance in milliohms
            - max_current_ma (u16): Configured max current in milliamps
        """
        return build_packet(LightFxPacket.POWER_STATUS)
    
    @staticmethod
    def power_config(shunt_mohm: int, max_current_ma: int) -> bytes:
        """
        Configure INA226 power monitor calibration.
        
        The INA226 calculates current from shunt voltage using:
            Current = Shunt_Voltage / Shunt_Resistance
        
        Max measurable current is limited by shunt voltage range (±81.92mV):
            Max_Current = 81.92mV / Shunt_Resistance
        
        Args:
            shunt_mohm: Shunt resistor value in milliohms (1-10000mΩ)
                       e.g., 100 for 0.1Ω, 10 for 0.01Ω
            max_current_ma: Maximum expected current in milliamps (1-65535mA)
                           e.g., 3200 for 3.2A
        
        Examples:
            power_config(100, 3200)   # 0.1Ω shunt, 3.2A max
            power_config(10, 8190)    # 0.01Ω shunt, 8.19A max (81.92mV/0.01Ω)
            power_config(50, 1640)    # 0.05Ω shunt, 1.64A max
            
        Warnings:
            Emits UserWarning if shunt_mohm is outside [1-10000].
            Emits UserWarning if max_current exceeds shunt's capability.
            
        Note:
            INA226 bus voltage range: 0-36V
            INA226 shunt voltage range: ±81.92mV
        """
        _warn_range("shunt_mohm", shunt_mohm, INA226_SHUNT_MOHM_MIN, INA226_SHUNT_MOHM_MAX, "mΩ")
        _warn_u16("max_current_ma", max_current_ma)
        
        # Check if max_current exceeds what the shunt can measure
        # Max detectable current = 81.92mV / (shunt_mohm / 1000) = 81920 / shunt_mohm mA
        if shunt_mohm > 0:
            max_measurable_ma = INA226_SHUNT_VOLTAGE_MAX // shunt_mohm
            if max_current_ma > max_measurable_ma:
                warnings.warn(
                    f"max_current_ma={max_current_ma} exceeds shunt capability "
                    f"(max ~{max_measurable_ma}mA with {shunt_mohm}mΩ shunt)",
                    UserWarning,
                    stacklevel=2
                )
        
        payload = u16_le(shunt_mohm) + u16_le(max_current_ma)
        return build_packet(LightFxPacket.POWER_CONFIG, payload)
