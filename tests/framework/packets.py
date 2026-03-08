"""
Packet Type Constants

Defines all packet types and error codes for ScaleFX protocol.
"""

# Tag value for async/unsolicited server-initiated messages
TAG_ASYNC = 0x00


class CorePacket:
    """Core system packet types (0xF0-0xFF)."""
    INIT        = 0xF0
    SHUTDOWN    = 0xF1
    KEEPALIVE   = 0xF2
    INIT_READY  = 0xF3
    STATUS      = 0xF4
    ERROR       = 0xF5
    ACK         = 0xF6
    NACK        = 0xF7
    REBOOT      = 0xF8
    BOOTSEL     = 0xF9
    STATUS_REQ  = 0xFA
    I2C_SCAN    = 0xFB
    I2C_SCAN_RESULT = 0xFC


class GunFxPacket:
    """GunFX packet types (0x01-0x2F)."""
    TRIGGER_ON      = 0x01
    TRIGGER_OFF     = 0x02
    SERVO_SET       = 0x10
    SERVO_SETTINGS  = 0x11
    SERVO_RECOIL    = 0x12
    SMOKE_HEAT          = 0x20
    SMOKE_SETTINGS      = 0x21
    SMOKE_RESET         = 0x22
    SMOKE_CURRENT_LIMIT = 0x23


class LightFxPacket:
    """LightFX packet types (0x40-0x5F)."""
    LED_SET           = 0x40
    LED_OFF           = 0x41
    LED_SEQ_CLEAR     = 0x42
    LED_SEQ_ADD       = 0x43
    LED_SEQ_START     = 0x44
    LED_SEQ_STOP      = 0x45
    LED_SEQ_RESTART   = 0x46
    LED_SEQ_STATUS    = 0x47
    LED_STATUS        = 0x48
    LED_SEQ_QUEUE     = 0x49
    LED_MASTER_BRIGHTNESS = 0x4A
    LED_RESET             = 0x4B  # [ch:u8] (0=all) Reset channel to defaults, re-enable
    LED_ENABLE            = 0x4C  # [ch:u8][enabled:u8] Enable/disable LED channel
    SERVO_SET         = 0x50
    SERVO_SETTINGS    = 0x51
    # Landing light control
    LANDING_LIGHT_BIND    = 0x52
    LANDING_LIGHT_UNBIND  = 0x53
    LANDING_LIGHT_DEPLOY  = 0x54
    LANDING_LIGHT_RETRACT = 0x55
    LANDING_LIGHT_STATUS  = 0x56  # [slot:u8][phase:u8][finished:u8] Async progress
    # Response packet types
    LED_STATUS_RESP     = 0x5A
    LED_SEQ_STATUS_RESP = 0x5B
    LED_SEQ_QUEUE_RESP  = 0x5D


class LightFxEventType:
    """LightFX LED event types."""
    ON        = 0x00
    OFF       = 0x01
    FLASH     = 0x02
    FADE_IN   = 0x03
    FADE_OUT  = 0x04
    FADING    = 0x05


class CoreError:
    """Core error codes (0x00-0x0F, 0xF0-0xFF)."""
    OK              = 0x00
    UNKNOWN         = 0x01
    NOT_INITIALIZED = 0x02
    INVALID_COMMAND = 0x03
    MISSING_PARAM   = 0x04
    BUSY            = 0x05
    NOT_SUPPORTED   = 0x06
    INVALID_PARAM   = 0x10
    PARAM_RANGE     = 0x11
    INVALID_ID      = 0x12
    INTERNAL        = 0xF0
    TIMEOUT         = 0xF1
    COMM_ERROR      = 0xF2
    
    @staticmethod
    def name(code: int) -> str:
        """Get error name from code."""
        names = {
            0x00: "OK",
            0x01: "UNKNOWN",
            0x02: "NOT_INITIALIZED",
            0x03: "INVALID_COMMAND",
            0x04: "MISSING_PARAM",
            0x05: "BUSY",
            0x06: "NOT_SUPPORTED",
            0x10: "INVALID_PARAM",
            0x11: "PARAM_RANGE",
            0x12: "INVALID_ID",
            0xF0: "INTERNAL",
            0xF1: "TIMEOUT",
            0xF2: "COMM_ERROR",
        }
        return names.get(code, f"UNKNOWN(0x{code:02X})")


class GunFxError:
    """GunFX-specific error codes (0x20-0x4F)."""
    SERVO_INVALID_ID    = 0x20
    SERVO_PULSE_RANGE   = 0x21
    SERVO_MIN_MAX       = 0x22
    SERVO_NOT_CONFIGURED = 0x23
    INVALID_FAN_SPEED   = 0x30
    HEATER_DISCONNECTED = 0x31
    FAN_DISCONNECTED    = 0x32
    HEATER_OVERCURRENT  = 0x33
    FAN_OVERCURRENT     = 0x34
    INVALID_RPM         = 0x40
    ALREADY_FIRING      = 0x41
    NOT_FIRING          = 0x42
    
    @staticmethod
    def name(code: int) -> str:
        """Get error name from code."""
        names = {
            0x20: "SERVO_INVALID_ID",
            0x21: "SERVO_PULSE_RANGE",
            0x22: "SERVO_MIN_MAX",
            0x23: "SERVO_NOT_CONFIGURED",
            0x30: "INVALID_FAN_SPEED",
            0x31: "HEATER_DISCONNECTED",
            0x32: "FAN_DISCONNECTED",
            0x33: "HEATER_OVERCURRENT",
            0x34: "FAN_OVERCURRENT",
            0x40: "INVALID_RPM",
            0x41: "ALREADY_FIRING",
            0x42: "NOT_FIRING",
        }
        return names.get(code, CoreError.name(code))


class SmokeErrorReason:
    """Per-channel smoke error reason codes (STATUS diagnostic)."""
    NONE                = 0x00  # No error
    HEATER_DISCONNECTED = 0x01  # Heater drawing 0 current while ON
    FAN_DISCONNECTED    = 0x02  # Fan drawing 0 current while ON
    HEATER_OVERCURRENT  = 0x03  # Heater current exceeded limit
    FAN_OVERCURRENT     = 0x04  # Fan current exceeded limit

    @staticmethod
    def name(reason: int) -> str:
        """Get human-readable error reason."""
        names = {
            0x00: "none",
            0x01: "heater disconnected",
            0x02: "fan disconnected",
            0x03: "heater overcurrent",
            0x04: "fan overcurrent",
        }
        return names.get(reason, f"unknown(0x{reason:02X})")


class LightFxError:
    """LightFX-specific error codes (0x50-0x5F)."""
    INVALID_CHANNEL = 0x50
    SEQ_FULL        = 0x51
    INVALID_EVENT   = 0x52
    INVALID_PARAM   = 0x53
    INVALID_SERVO   = 0x54
    INVALID_SLOT    = 0x55
    CHANNEL_DISABLED = 0x56
    
    @staticmethod
    def name(code: int) -> str:
        """Get error name from code."""
        names = {
            0x50: "INVALID_CHANNEL",
            0x51: "SEQ_FULL",
            0x52: "INVALID_EVENT",
            0x53: "INVALID_PARAM",
            0x54: "INVALID_SERVO",
            0x55: "INVALID_SLOT",
            0x56: "CHANNEL_DISABLED",
        }
        return names.get(code, CoreError.name(code))


class GearControlPacket:
    """GearControl packet types (0x60-0x7F)."""
    GEAR_DEPLOY    = 0x60
    GEAR_RETRACT   = 0x61
    GEAR_STOP      = 0x62
    GEAR_ALL       = 0x63
    SERVO_SET      = 0x64
    SRV_SETTINGS   = 0x65
    GEAR_CONFIG    = 0x66
    DOOR_CONFIG    = 0x67
    YAW_CONFIG     = 0x68
    YAW_INPUT      = 0x69
    GEAR_CALIBRATE    = 0x6A
    GEAR_CALIB_STATUS = 0x6B
    GEAR_CALIB_CANCEL = 0x6C
    BATTERY_CONFIG   = 0x6D
    DOOR_MODE        = 0x6E
    GEAR_RESET       = 0x6F  # [gear_id:u8] Clear error state (ERROR → UNKNOWN)
    GEAR_SEQ_STATUS  = 0x70  # [gear_id:u8][phase:u8][deploying:u8][finished:u8] Sequence progress
    GEAR_ENABLE      = 0x71  # [gear_id:u8][enabled:u8] Enable/disable gear channel
    GEAR_DOOR_STATUS = 0x72  # [gear_id:u8][state:u8][door0_pos_us:u16LE][door1_pos_us:u16LE] Door state transition


class GearControlError:
    """GearControl-specific error codes (0x60-0x6F)."""
    INVALID_GEAR_ID    = 0x60
    INVALID_SERVO_ID   = 0x61
    GEAR_BUSY          = 0x62
    MOTOR_STALL        = 0x63
    MOTOR_TIMEOUT      = 0x64
    SERVO_OUT_OF_RANGE = 0x65
    INA226_ERROR       = 0x66
    YAW_NOT_AVAILABLE  = 0x67
    INVALID_ACTION     = 0x68
    NO_CURRENT_MONITOR = 0x69
    NOT_CALIBRATING    = 0x6A
    GEAR_DISABLED      = 0x6B
    
    @staticmethod
    def name(code: int) -> str:
        """Get error name from code."""
        names = {
            0x60: "INVALID_GEAR_ID",
            0x61: "INVALID_SERVO_ID",
            0x62: "GEAR_BUSY",
            0x63: "MOTOR_STALL",
            0x64: "MOTOR_TIMEOUT",
            0x65: "SERVO_OUT_OF_RANGE",
            0x66: "INA226_ERROR",
            0x67: "YAW_NOT_AVAILABLE",
            0x68: "INVALID_ACTION",
            0x69: "NO_CURRENT_MONITOR",
            0x6A: "NOT_CALIBRATING",
            0x6B: "GEAR_DISABLED",
        }
        return names.get(code, CoreError.name(code))


class DoorMode:
    """Door activation modes for landing gear sequencing."""
    NONE       = 0  # No door servos (motor only)
    SINGLE     = 1  # One door servo (servo 0 only)
    DUAL_SYNC  = 2  # Two doors, simultaneous (default)
    DUAL_DELAY = 3  # Two doors, door 1 starts after delay
    DUAL_SEQ   = 4  # Two doors, door 1 starts after door 0 completes

    @staticmethod
    def name(mode: int) -> str:
        """Get mode name from value."""
        names = {
            0: "NONE",
            1: "SINGLE",
            2: "DUAL_SYNC",
            3: "DUAL_DELAY",
            4: "DUAL_SEQ",
        }
        return names.get(mode, f"UNKNOWN({mode})")


class GearErrorReason:
    """Per-gear error reason codes (STATUS diagnostic)."""
    NONE           = 0x00  # No error
    MONITOR_FAULT  = 0x01  # INA226 I2C init failed
    MOTOR_STALL        = 0x02  # Motor stall detected during operation
    MOTOR_TIMEOUT      = 0x03  # Motor operation exceeded timeout
    SEQUENCE_ERROR     = 0x04  # Unexpected state during sequencing
    MOTOR_DISCONNECTED = 0x05  # Motor current dropped to 0 (disconnected)
    CALIB_TIMEOUT      = 0x06  # Calibration exceeded overall time limit
    NO_STALL_DETECTED  = 0x07  # No stall current detected in either direction

    @staticmethod
    def name(reason: int) -> str:
        """Get human-readable error reason."""
        names = {
            0x00: "none",
            0x01: "INA226 init failed",
            0x02: "motor stall",
            0x03: "motor timeout",
            0x04: "sequence error",
            0x05: "motor disconnected",
            0x06: "calibration timeout",
            0x07: "no stall detected",
        }
        return names.get(reason, f"unknown(0x{reason:02X})")


class GearSeqPhase:
    """Gear deploy/retract sequence phases (wire format)."""
    IDLE           = 0
    OPENING_DOORS  = 1
    RUNNING_MOTOR  = 2
    CLOSING_DOORS  = 3
    SEQ_ERROR      = 4
    SYNC_WAIT      = 5

    @staticmethod
    def name(phase: int) -> str:
        """Get human-readable phase name."""
        names = {
            0: "idle",
            1: "opening doors",
            2: "running motor",
            3: "closing doors",
            4: "error",
            5: "sync wait",
        }
        return names.get(phase, f"unknown({phase})")


class DoorState:
    """Door state values (wire format for STATUS and GEAR_DOOR_STATUS)."""
    UNKNOWN  = 0  # State not determined
    CLOSED   = 1  # Doors at close position
    OPEN     = 2  # Doors at open position
    OPENING  = 3  # Doors moving to open position
    CLOSING  = 4  # Doors moving to close position

    @staticmethod
    def name(state: int) -> str:
        """Get human-readable door state name."""
        names = {
            0: "unknown",
            1: "closed",
            2: "open",
            3: "opening",
            4: "closing",
        }
        return names.get(state, f"unknown({state})")


class LandingLightPhase:
    """Landing light deploy/retract phases (wire format)."""
    RETRACTED  = 0
    DEPLOYING  = 1
    DEPLOYED   = 2
    RETRACTING = 3

    @staticmethod
    def name(phase: int) -> str:
        """Get human-readable phase name."""
        names = {
            0: "retracted",
            1: "deploying",
            2: "deployed",
            3: "retracting",
        }
        return names.get(phase, f"unknown({phase})")
