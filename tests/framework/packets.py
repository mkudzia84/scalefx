"""
Packet Type Constants

Defines all packet types and error codes for ScaleFX protocol.
"""

from enum import IntEnum

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
    LOG_MESSAGE = 0xFD  # [level:u8][millis:u32LE][message:str] (async, universal)
    IDENTIFY    = 0xFE  # Query board info without triggering INIT (same payload as INIT_READY)
    DIAG_HISTORY = 0xFF  # Request diagnostic log history (sends LOG_MESSAGE packets without draining buffer)


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
    INVALID_VALUE   = 0x13
    PARAM_TOO_LONG  = 0x14
    INTERNAL        = 0xF0
    TIMEOUT         = 0xF1
    COMM_ERROR      = 0xF2
    CRC_ERROR       = 0xF4
    
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
            0x13: "INVALID_VALUE",
            0x14: "PARAM_TOO_LONG",
            0xF0: "INTERNAL",
            0xF1: "TIMEOUT",
            0xF2: "COMM_ERROR",
            0xF4: "CRC_ERROR",
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


# =============================================================================
# Streaming Protocol Packet Types (reusable infrastructure)
# =============================================================================

class StreamPacket:
    """Streaming protocol packet types (0xA4-0xA6).

    Protocol-level infrastructure for chunked data transfer.
    Used by any controller that streams data via StreamWriter.
    Defined in core/stream.h (C++) / here (Python).

    Wire format:
      STREAM_BEGIN: [totalBytes:u32LE]                        (0 = unknown)
      STREAM_DATA:  [seqNum:u16LE][crc16:u16LE][data:0-508]   per-chunk CRC-16
      STREAM_END:   [totalSegs:u16LE][totalBytes:u32LE][crc16All:u16LE]

    CRC-16: CCITT polynomial 0x1021, init 0xFFFF.
    Max chunk data: MAX_PAYLOAD_SIZE(512) - CHUNK_HEADER(4) = 508 bytes.
    All packets in a stream share the same tag for correlation.
    """
    STREAM_BEGIN = 0xA4  # [totalBytes:u32LE] (0 = unknown)
    STREAM_DATA  = 0xA5  # [seqNum:u16LE][crc16:u16LE][data:N]
    STREAM_END   = 0xA6  # [totalSegs:u16LE][totalBytes:u32LE][crc16All:u16LE]


# =============================================================================
# HubFX Packet Types and Error Codes
# =============================================================================

class HubFxPacket:
    """HubFX packet types (0x80-0xAA)."""
    # Slave management
    SLAVE_LIST       = 0x80  # [] → SLAVE_LIST_RESP
    SLAVE_LIST_RESP  = 0x81  # [count:u8][entries...] Response
    SLAVE_INIT       = 0x82  # [slaveType:u8] Init a slave by type
    SLAVE_STATUS     = 0x83  # [] Request hub-level status

    # Audio control
    AUDIO_PLAY        = 0x84  # [ch:u8][vol:u8][output:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]
    AUDIO_STOP        = 0x85  # [ch:u8] (0xFF=all)
    AUDIO_VOLUME      = 0x86  # [ch:u8][vol:u8] (ch 0xFF=master, vol 0-100)
    AUDIO_FADE        = 0x87  # [ch:u8]
    AUDIO_QUEUE       = 0x88  # [ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]
    AUDIO_QUEUE_CLEAR = 0x89  # [ch:u8] (0xFF=all)
    AUDIO_STATUS_REQ  = 0x8A  # [] → AUDIO_STATUS_RESP
    AUDIO_STATUS_RESP = 0x8B  # v3: [masterVol:u8][flags:u8][sampleRate:u16LE][bitDepth:u8]
                              #     [maxCh:u8][codecNameLen:u8][codecName:str]
                              #     [ringFillPct:u8][ringAvailRead:u16LE][ringAvailWrite:u16LE]
                              #     [underruns:u32LE][consumeLoops:u32LE][consumeFrames:u32LE]
                              #     [activeMask:u8][per-ch: 10B + wavRate:u16LE + wavCh:u8
                              #      + wavBits:u8 + fnameLen:u8 + fname:str]

    # Engine FX
    ENGINE_START       = 0x8C  # [] → ACK
    ENGINE_STOP        = 0x8D  # [] → ACK
    ENGINE_STATUS_REQ  = 0x8E  # [] → ENGINE_STATUS_RESP
    ENGINE_STATUS_RESP = 0x8F  # [state:u8][toggleEngaged:u8][active:u8]

    # Config management
    CONFIG_RELOAD      = 0x90  # [] or [pathLen:u8][path:str] → ACK/NACK
    CONFIG_STATUS      = 0x91  # [] → CONFIG_STATUS_RESP
    CONFIG_STATUS_RESP = 0x92  # [loaded:u8][size:u16LE][validOk:u8]
    CONFIG_SAVE       = 0xAC  # [] or [pathLen:u8][path:str] → ACK/NACK

    # SD card management
    SD_INIT           = 0x93  # [speed_mhz:u8] → ACK/NACK (remounts card)
    SD_STATUS_REQ     = 0x94  # [] → SD_STATUS_RESP
    SD_STATUS_RESP    = 0x95  # [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]
                              #   extended (v0.5+): [cardType:u8][busMode:u8][usedSpace_MB:u32LE]

    # Slave routing (subcmd pattern)
    # Payload: [subcmd:u8][original_payload...]
    # subcmd is the original slave packet type, forwarded to the slave.
    SLAVE_ROUTE_GUNFX       = 0x96  # [subcmd:u8][...] → route to GunFX
    SLAVE_ROUTE_LIGHTFX     = 0x97  # [subcmd:u8][...] → route to LightFX
    SLAVE_ROUTE_GEARCONTROL = 0x98  # [subcmd:u8][...] → route to GearControl

    # Flash management
    FLASH_STATUS_REQ  = 0x99  # [] → response: [initialized:u8][totalBytes:u32LE][usedBytes:u32LE][freeBytes:u32LE]

    # LOG_MESSAGE moved to CorePacket.LOG_MESSAGE (0xFD) — universal across all boards

    # File operations — optional [target:u8] appended to path (0=SD default, 1=Flash)
    FILE_LIST          = 0x9A  # [pathLen:u8][path:str][target:u8?] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    FILE_DELETE        = 0x9B  # [pathLen:u8][path:str][target:u8?] → ACK/NACK
    FILE_MKDIR         = 0x9C  # [pathLen:u8][path:str][target:u8?] → ACK/NACK
    FILE_INFO          = 0x9D  # [pathLen:u8][path:str][target:u8?] → FILE_INFO_RESP
    FILE_INFO_RESP     = 0x9E  # [exists:u8][isDir:u8][size:u32LE]
    FILE_DOWNLOAD      = 0x9F  # [pathLen:u8][path:str][target:u8?] → STREAM_BEGIN + STREAM_DATA + STREAM_END
    FILE_UPLOAD_BEGIN  = 0xA0  # [size:u32LE][pathLen:u8][path:str][target:u8?] → ACK
    FILE_UPLOAD_DATA   = 0xA1  # [seqNum:u16LE][crc16:u16LE][data:N] → ACK/NACK(CRC_ERROR)
    FILE_UPLOAD_END    = 0xA2  # [] → ACK/NACK
    FILE_UPLOAD_CANCEL = 0xA3  # [] → ACK

    # Streaming packet types (STREAM_BEGIN/DATA/END) are defined in
    # StreamPacket class — they are protocol infrastructure reusable
    # by any controller, not HubFX-specific.

    # File tree (0xA9)
    FILE_TREE          = 0xA9  # [pathLen:u8][path:str][target:u8?] → STREAM (recursive listing)

    # USB Host Diagnostics (0xA7-0xA8, 0xAD)
    USB_DEVICES_REQ    = 0xA7  # [] → USB_DEVICES_RESP
    USB_DEVICES_RESP   = 0xA8  # [initialized:u8][taskRunning:u8][backendLen:u8][backend:str]
                               #   [deviceCount:u8] per-device: [addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]

    # USB Bus Recovery (0xAD)
    USB_RESET_BUS      = 0xAD  # [] → ACK (power-cycles root port, re-enumerates)

    # Codec Status (0xAA-0xAB)
    CODEC_STATUS_REQ   = 0xAA  # [] → CODEC_STATUS_RESP
    CODEC_STATUS_RESP  = 0xAB  # [codecType:u8][initialized:u8][i2cOk:u8][sdaPin:u8][sclPin:u8]
                               #   [supplyVoltage:u8][muted:u8][digitalVol:u8][deviceCtrl:u8][faultStatus:u8]
                               #   [codecNameLen:u8][codecName:str]

    # Slave Info Query (0xAE-0xAF) — returns cached boardInfo from BusClient
    SLAVE_INFO          = 0xAE  # [slaveType:u8] → SLAVE_INFO_RESP
    SLAVE_INFO_RESP     = 0xAF  # [slaveType:u8][ready:u8][connected:u8][nameLen:u8][name:str]
                                #   [verLen:u8][ver:str][platLen:u8][plat:str]
                                #   [cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]


class HubFxError:
    """HubFX-specific error codes (0x80-0x8F)."""
    SLAVE_NOT_FOUND      = 0x80
    SLAVE_NOT_CONNECTED  = 0x81
    SLAVE_INIT_FAILED    = 0x82
    NO_SLAVES            = 0x83
    SLAVE_COMM_ERROR     = 0x84
    AUDIO_ERROR          = 0x85
    SD_NOT_INITIALIZED   = 0x86
    ENGINE_NOT_AVAILABLE = 0x87
    CONFIG_ERROR         = 0x88
    INVALID_CHANNEL      = 0x89

    # File operation errors (0x8A-0x8F)
    FILE_NOT_FOUND       = 0x8A
    FILE_ALREADY_EXISTS  = 0x8B
    FILE_IO_ERROR        = 0x8C
    FILE_TOO_LARGE       = 0x8D
    UPLOAD_IN_PROGRESS   = 0x8E
    NO_UPLOAD_ACTIVE     = 0x8F

    @staticmethod
    def name(code: int) -> str:
        """Get error name from code."""
        names = {
            0x80: "SLAVE_NOT_FOUND",
            0x81: "SLAVE_NOT_CONNECTED",
            0x82: "SLAVE_INIT_FAILED",
            0x83: "NO_SLAVES",
            0x84: "SLAVE_COMM_ERROR",
            0x85: "AUDIO_ERROR",
            0x86: "SD_NOT_INITIALIZED",
            0x87: "ENGINE_NOT_AVAILABLE",
            0x88: "CONFIG_ERROR",
            0x89: "INVALID_CHANNEL",
            0x8A: "FILE_NOT_FOUND",
            0x8B: "FILE_ALREADY_EXISTS",
            0x8C: "FILE_IO_ERROR",
            0x8D: "FILE_TOO_LARGE",
            0x8E: "UPLOAD_IN_PROGRESS",
            0x8F: "NO_UPLOAD_ACTIVE",
        }
        return names.get(code, CoreError.name(code))


class SlaveType:
    """Slave controller type enumeration (matches C++ SlaveType enum)."""
    UNKNOWN      = 0
    GUNFX        = 1
    LIGHTFX      = 2
    GEARCONTROL  = 3

    @staticmethod
    def name(stype: int) -> str:
        """Get human-readable slave type name."""
        names = {
            0: "Unknown",
            1: "GunFX",
            2: "LightFX",
            3: "GearControl",
        }
        return names.get(stype, f"Unknown({stype})")


# ============================================================================
# Known USB Device Identification (VID/PID → friendly name)
# Mirrors C++ knownDeviceName() in sfx_usb_host.h
# ============================================================================

USB_VID_RASPBERRY_PI = 0x2E8A   # Raspberry Pi Foundation

USB_PID_GUNFX        = 0x0180   # gunfx_pico (custom tusb_config.h)
USB_PID_LIGHTFX      = 0x0181   # lightfx_pico
USB_PID_GEARCONTROL  = 0x0182   # gearcontrol_pico
USB_PID_PICO_DEFAULT = 0x000A   # Default Arduino-Pico CDC PID

_KNOWN_DEVICES = {
    (USB_VID_RASPBERRY_PI, USB_PID_GUNFX):       "GunFX",
    (USB_VID_RASPBERRY_PI, USB_PID_LIGHTFX):     "LightFX",
    (USB_VID_RASPBERRY_PI, USB_PID_GEARCONTROL): "GearControl",
    (USB_VID_RASPBERRY_PI, USB_PID_PICO_DEFAULT): "Pico (default PID)",
}

def known_device_name(vid: int, pid: int) -> str | None:
    """Look up a friendly name for a USB device by VID/PID.

    Returns the name string if the device is a known ScaleFX slave,
    or None if not recognized.
    """
    return _KNOWN_DEVICES.get((vid, pid))


class HubFxAudio:
    """HubFX audio wire format constants (matches C++ HubFxAudio namespace)."""
    OUTPUT_STEREO = 0
    OUTPUT_LEFT   = 1
    OUTPUT_RIGHT  = 2

    LOOP_NONE     = 0  # Play once
    LOOP_FINITE   = 1  # Loop N times
    LOOP_INFINITE = 2  # Loop forever

    QUEUE_FINISH_LOOP = 0  # Wait for current loop to finish
    QUEUE_STOP_NOW    = 1  # Stop current immediately

    CH_ALL        = 0xFF  # All channels / master
    MAX_CHANNELS  = 8


class HubFxStorage:
    """HubFX storage target and upload mode enums (matches C++ HubFxStorage namespace)."""

    class Target(IntEnum):
        SD    = 0  # SD card (default)
        FLASH = 1  # Onboard LittleFS flash

    class UploadMode(IntEnum):
        SYNC     = 0  # ACK per chunk, CRC retry (default)
        BURST    = 1  # No per-chunk ACK, MD5 verification at end
        # Mode 2 (windowed) removed — use STREAM instead
        STREAM   = 3  # Raw byte streaming: bypasses COBS, ring buffer + dual-core

    # Backward-compatible aliases
    TARGET_SD    = Target.SD
    TARGET_FLASH = Target.FLASH
    UPLOAD_SYNC  = UploadMode.SYNC
    UPLOAD_BURST = UploadMode.BURST
    UPLOAD_STREAM = UploadMode.STREAM


class DiagLevel:
    """Diagnostic log levels (matches C++ DiagLevel namespace)."""
    DEBUG = 0
    INFO  = 1
    WARN  = 2
    ERROR = 3

    @staticmethod
    def name(level: int) -> str:
        """Get human-readable level name."""
        names = {0: "DEBUG", 1: "INFO", 2: "WARN", 3: "ERROR"}
        return names.get(level, f"L{level}")


class EngineState:
    """Engine FX state enumeration (matches C++ EngineState enum)."""
    STOPPED  = 0
    STARTING = 1
    RUNNING  = 2
    STOPPING = 3

    @staticmethod
    def name(state: int) -> str:
        """Get human-readable state name."""
        names = {
            0: "Stopped",
            1: "Starting",
            2: "Running",
            3: "Stopping",
        }
        return names.get(state, f"Unknown({state})")
