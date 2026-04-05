"""GearControl command builders."""

from ._common import (
    warnings, build_packet, u16_le,
    _warn_range, _warn_u16,
    GearControlPacket,
    SERVO_PULSE_MIN, SERVO_PULSE_MAX,
    GEAR_ID_MIN, GEAR_ID_MAX, GEAR_SERVO_ID_MIN, GEAR_SERVO_ID_MAX,
    GEAR_ACTION_RETRACT, GEAR_ACTION_STOP,
)
from .core import CommandBuilder


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
    def gear_calibrate(gear_id: int, timeout_s: int = 0) -> bytes:
        """
        Start stall current calibration for a gear.
        
        Runs the motor in each direction to detect stall current.
        The calibrated value is saved and reported in STATUS responses.
        Progress updates are sent as GEAR_CALIB_STATUS packets.
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
            timeout_s: Overall timeout in seconds (0=no timeout, 1-255)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        if timeout_s > 0:
            _warn_range("timeout_s", timeout_s, 1, 255, "s")
            return build_packet(GearControlPacket.GEAR_CALIBRATE, bytes([gear_id, timeout_s]))
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
    def door_mode(gear_id: int, pre_deploy: int, post_deploy: int = 0, delay_ms: int = 500) -> bytes:
        """
        Configure door activation modes for a gear (two-mode system).
        
        doorPreDeploy (pre_deploy): Doors opened before deploy motor, closed after retract motor.
        doorPostDeploy (post_deploy): Doors closed after deploy motor, opened before retract motor.
        Retract is the reverse of the deploy operation.
        
        Mode values:
          0 = NONE        No door servos (skip this phase)
          1 = SINGLE      One door servo (servo 0 only)
          2 = DUAL_SYNC   Two doors, simultaneous (default)
          3 = DUAL_DELAY  Two doors, door 1 starts after delay_ms
          4 = DUAL_SEQ    Two doors, door 1 starts after door 0 completes
        
        Setting post_deploy=NONE skips post-deploy close and pre-retract open phases.
        For DUAL_DELAY/DUAL_SEQ: doors open 0→1, close 1→0 (like real aircraft).
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
            pre_deploy: doorPreDeploy mode value (0-4)
            post_deploy: doorPostDeploy mode value (0-4, default 0=NONE=skip)
            delay_ms: Delay between doors in ms (DUAL_DELAY only, default 500)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        _warn_range("pre_deploy", pre_deploy, 0, 4)
        _warn_range("post_deploy", post_deploy, 0, 4)
        _warn_u16("delay_ms", delay_ms)
        payload = bytes([gear_id, pre_deploy, post_deploy]) + u16_le(delay_ms)
        return build_packet(GearControlPacket.DOOR_MODE, payload)

    @staticmethod
    def gear_reset(gear_id: int) -> bytes:
        """
        Clear error state for a gear (ERROR → UNKNOWN).
        
        Clears the error state and error reason. Safe to call on
        non-errored gears (no-op on server side).
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        return build_packet(GearControlPacket.GEAR_RESET, bytes([gear_id]))

    @staticmethod
    def gear_enable(gear_id: int, enabled: bool) -> bytes:
        """
        Enable or disable a gear channel.
        
        When disabled, deploy/retract/calibrate commands are rejected.
        stop() always works (safety). Active sequences are stopped.
        
        Args:
            gear_id: Gear index (0=nose, 1=left main, 2=right main)
            enabled: True to enable, False to disable
        """
        _warn_range("gear_id", gear_id, GEAR_ID_MIN, GEAR_ID_MAX)
        return build_packet(GearControlPacket.GEAR_ENABLE,
                            bytes([gear_id, 1 if enabled else 0]))
