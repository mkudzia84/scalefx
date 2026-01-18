"""
Packet Type Constants

Defines all packet types and error codes for ScaleFX protocol.
"""


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


class GunFxPacket:
    """GunFX packet types (0x01-0x2F)."""
    TRIGGER_ON      = 0x01
    TRIGGER_OFF     = 0x02
    SERVO_SET       = 0x10
    SERVO_SETTINGS  = 0x11
    SERVO_RECOIL    = 0x12
    SMOKE_HEAT      = 0x20
    SMOKE_SETTINGS  = 0x21


class LightFxPacket:
    """LightFX packet types (0x40-0x5F)."""
    LED_SET         = 0x40
    LED_OFF         = 0x41
    LED_SEQ_CLEAR   = 0x42
    LED_SEQ_ADD     = 0x43
    LED_SEQ_START   = 0x44
    LED_SEQ_STOP    = 0x45
    SERVO_SET       = 0x50
    SERVO_SETTINGS  = 0x51
    POWER_STATUS    = 0x58
    POWER_STATUS_RESP = 0x59


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
    HEATER_SAFETY       = 0x30
    FAN_NOT_RUNNING     = 0x31
    INVALID_FAN_SPEED   = 0x32
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
            0x30: "HEATER_SAFETY",
            0x31: "FAN_NOT_RUNNING",
            0x32: "INVALID_FAN_SPEED",
            0x40: "INVALID_RPM",
            0x41: "ALREADY_FIRING",
            0x42: "NOT_FIRING",
        }
        return names.get(code, CoreError.name(code))


class LightFxError:
    """LightFX-specific error codes (0x50-0x5F)."""
    INVALID_CHANNEL = 0x50
    SEQ_FULL        = 0x51
    INVALID_EVENT   = 0x52
    INVALID_PARAM   = 0x53
    INVALID_SERVO   = 0x54
    
    @staticmethod
    def name(code: int) -> str:
        """Get error name from code."""
        names = {
            0x50: "INVALID_CHANNEL",
            0x51: "SEQ_FULL",
            0x52: "INVALID_EVENT",
            0x53: "INVALID_PARAM",
            0x54: "INVALID_SERVO",
        }
        return names.get(code, CoreError.name(code))
