"""LightFX command builders."""

from ._common import (
    warnings, build_packet, u16_le, i16_le,
    _warn_range, _warn_u16,
    LightFxPacket, LightFxEventType,
    SERVO_ID_MIN, SERVO_ID_MAX, SERVO_PULSE_MIN, SERVO_PULSE_MAX,
    LED_CHANNEL_MIN, LED_CHANNEL_MAX, LED_BRIGHTNESS_MAX,
)
from .core import CommandBuilder


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
    def led_seq_add_fading(channel: int, cycle_ms: int, duration_ms: int,
                           min_brightness: int = 0, max_brightness: int = 100) -> bytes:
        """
        Add FADING event to sequence (sinusoidal breathing/beacon effect).
        
        Args:
            channel: LED channel (1-8)
            cycle_ms: Duration of one full fade cycle in milliseconds
            duration_ms: Total event duration in milliseconds (0 = infinite)
            min_brightness: Minimum brightness (0-100, default 0)
            max_brightness: Maximum brightness (0-100, default 100)
            
        Warnings:
            Emits UserWarning if brightness values not in [0-100].
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("cycle_ms", cycle_ms)
        _warn_u16("duration_ms", duration_ms)
        _warn_range("min_brightness", min_brightness, 0, LED_BRIGHTNESS_MAX)
        _warn_range("max_brightness", max_brightness, 0, LED_BRIGHTNESS_MAX)
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.FADING,
            cycle_ms, duration_ms, min_brightness, max_brightness
        )
    
    @staticmethod
    def led_seq_add_beacon(channel: int, cycle_ms: int, duration_ms: int,
                           flash_percent: int = 15, max_brightness: int = 100) -> bytes:
        """
        Add BEACON event to sequence (rotating beacon with brief flash).
        
        Args:
            channel: LED channel (1-8)
            cycle_ms: Duration of one full rotation in milliseconds
            duration_ms: Total event duration in milliseconds (0 = infinite)
            flash_percent: Percentage of cycle occupied by flash (1-50, default 15)
            max_brightness: Peak brightness (0-100, default 100)
            
        Warnings:
            Emits UserWarning if values out of range.
        """
        _warn_range("channel", channel, LED_CHANNEL_MIN, LED_CHANNEL_MAX)
        _warn_u16("cycle_ms", cycle_ms)
        _warn_u16("duration_ms", duration_ms)
        _warn_range("flash_percent", flash_percent, 1, 50)
        _warn_range("max_brightness", max_brightness, 0, LED_BRIGHTNESS_MAX)
        return LightFxCommands.led_seq_add(
            channel, LightFxEventType.BEACON,
            cycle_ms, duration_ms, flash_percent, max_brightness
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
    
    # =========================================================================
    # Channel Management
    # =========================================================================
    
    @staticmethod
    def led_reset(channel: int = 0) -> bytes:
        """
        Reset LED channel(s) to defaults.
        
        Stops sequence, clears sequence, turns off LED, re-enables channel.
        If channel=0, also resets master brightness to 100%.
        
        Args:
            channel: LED channel (1-8), or 0 for all channels
            
        Warnings:
            Emits UserWarning if channel not in [0-8].
        """
        _warn_range("channel", channel, 0, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_RESET, bytes([channel]))
    
    @staticmethod
    def led_enable(channel: int, enabled: bool = True) -> bytes:
        """
        Enable or disable an LED channel.
        
        When disabled, LED operations on the channel return CHANNEL_DISABLED.
        Disabling stops any active sequence and turns off the LED.
        
        Args:
            channel: LED channel (1-8), or 0 for all channels
            enabled: True to enable, False to disable
            
        Warnings:
            Emits UserWarning if channel not in [0-8].
        """
        _warn_range("channel", channel, 0, LED_CHANNEL_MAX)
        return build_packet(LightFxPacket.LED_ENABLE, bytes([channel, 1 if enabled else 0]))
