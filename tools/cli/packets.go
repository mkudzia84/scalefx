package main

import (
	"fmt"
	"strings"
)

// ScaleFX CLI - Packet Type Constants and Error Codes
// Mirrors tests/framework/packets.py (MUST stay in sync with C++ headers).

// ─── Core Packet Types (0xF0-0xFF) ───

const (
	CoreINIT         = 0xF0
	CoreSHUTDOWN     = 0xF1
	CoreKEEPALIVE    = 0xF2
	CoreINIT_READY   = 0xF3
	CoreSTATUS       = 0xF4
	CoreERROR        = 0xF5
	CoreACK          = 0xF6
	CoreNACK         = 0xF7
	CoreREBOOT       = 0xF8
	CoreBOOTSEL      = 0xF9
	CoreSTATUS_REQ   = 0xFA
	CoreI2C_SCAN     = 0xFB
	CoreI2C_SCAN_RES = 0xFC
	CoreLOG_MESSAGE  = 0xFD
	CoreIDENTIFY     = 0xFE
	CoreDIAG_HISTORY = 0xFF
)

// ─── Core Error Codes ───

const (
	ErrOK              = 0x00
	ErrUNKNOWN         = 0x01
	ErrNOT_INITIALIZED = 0x02
	ErrINVALID_COMMAND = 0x03
	ErrMISSING_PARAM   = 0x04
	ErrBUSY            = 0x05
	ErrNOT_SUPPORTED   = 0x06
	ErrINVALID_PARAM   = 0x10
	ErrPARAM_RANGE     = 0x11
	ErrINVALID_ID      = 0x12
	ErrINVALID_VALUE   = 0x13
	ErrPARAM_TOO_LONG  = 0x14
	ErrINTERNAL        = 0xF0
	ErrTIMEOUT         = 0xF1
	ErrCOMM_ERROR      = 0xF2
	ErrCRC_ERROR       = 0xF4
)

// ─── GunFX Packet Types (0x01-0x2F) ───

const (
	GfxTRIGGER_ON          = 0x01
	GfxTRIGGER_OFF         = 0x02
	GfxSERVO_SET           = 0x10
	GfxSERVO_SETTINGS      = 0x11
	GfxSERVO_RECOIL        = 0x12
	GfxSMOKE_HEAT          = 0x20
	GfxSMOKE_SETTINGS      = 0x21
	GfxSMOKE_RESET         = 0x22
	GfxSMOKE_CURRENT_LIMIT = 0x23
)

// ─── GunFX Error Codes (0x20-0x4F) ───

const (
	GfxErrSERVO_INVALID_ID    = 0x20
	GfxErrSERVO_PULSE_RANGE   = 0x21
	GfxErrSERVO_MIN_MAX       = 0x22
	GfxErrSERVO_NOT_CONFIGURED = 0x23
	GfxErrINVALID_FAN_SPEED   = 0x30
	GfxErrHEATER_DISCONNECTED = 0x31
	GfxErrFAN_DISCONNECTED    = 0x32
	GfxErrHEATER_OVERCURRENT  = 0x33
	GfxErrFAN_OVERCURRENT     = 0x34
	GfxErrINVALID_RPM         = 0x40
	GfxErrALREADY_FIRING      = 0x41
	GfxErrNOT_FIRING          = 0x42
)

// ─── LightFX Packet Types (0x40-0x5F) ───

const (
	LfxLED_SET              = 0x40
	LfxLED_OFF              = 0x41
	LfxLED_SEQ_CLEAR        = 0x42
	LfxLED_SEQ_ADD          = 0x43
	LfxLED_SEQ_START        = 0x44
	LfxLED_SEQ_STOP         = 0x45
	LfxLED_SEQ_RESTART      = 0x46
	LfxLED_SEQ_STATUS       = 0x47
	LfxLED_STATUS           = 0x48
	LfxLED_SEQ_QUEUE        = 0x49
	LfxLED_MASTER_BRIGHTNESS = 0x4A
	LfxLED_RESET            = 0x4B
	LfxLED_ENABLE           = 0x4C
	LfxSERVO_SET            = 0x50
	LfxSERVO_SETTINGS       = 0x51
	LfxLANDING_LIGHT_BIND    = 0x52
	LfxLANDING_LIGHT_UNBIND  = 0x53
	LfxLANDING_LIGHT_DEPLOY  = 0x54
	LfxLANDING_LIGHT_RETRACT = 0x55
	LfxLANDING_LIGHT_STATUS  = 0x56
	LfxLED_SEQ_STATUS_RESP   = 0x5A
	LfxLED_STATUS_RESP       = 0x5B
	LfxLED_SEQ_QUEUE_RESP    = 0x5D
)

// ─── LightFX LED Event Types ───

const (
	LfxEvtON       = 0x00
	LfxEvtOFF      = 0x01
	LfxEvtFLASH    = 0x02
	LfxEvtFADE_IN  = 0x03
	LfxEvtFADE_OUT = 0x04
	LfxEvtFADING   = 0x05
	LfxEvtBEACON   = 0x06
)

// ─── LightFX Error Codes (0x50-0x5F) ───

const (
	LfxErrINVALID_CHANNEL  = 0x50
	LfxErrSEQ_FULL         = 0x51
	LfxErrINVALID_EVENT    = 0x52
	LfxErrINVALID_PARAM    = 0x53
	LfxErrINVALID_SERVO    = 0x54
	LfxErrINVALID_SLOT     = 0x55
	LfxErrCHANNEL_DISABLED = 0x56
)

// ─── GearControl Packet Types (0x60-0x7F) ───

const (
	GcGEAR_DEPLOY      = 0x60
	GcGEAR_RETRACT     = 0x61
	GcGEAR_STOP        = 0x62
	GcGEAR_ALL         = 0x63
	GcSERVO_SET        = 0x64
	GcSRV_SETTINGS     = 0x65
	GcGEAR_CONFIG      = 0x66
	GcDOOR_CONFIG      = 0x67
	GcYAW_CONFIG       = 0x68
	GcYAW_INPUT        = 0x69
	GcGEAR_CALIBRATE   = 0x6A
	GcGEAR_CALIB_STATUS = 0x6B
	GcGEAR_CALIB_CANCEL = 0x6C
	GcBATTERY_CONFIG   = 0x6D
	GcDOOR_MODE        = 0x6E
	GcGEAR_RESET       = 0x6F
	GcGEAR_SEQ_STATUS  = 0x70
	GcGEAR_ENABLE      = 0x71
	GcGEAR_DOOR_STATUS = 0x72
)

// ─── GearControl Error Codes (0x60-0x6F) ───

const (
	GcErrINVALID_GEAR_ID    = 0x60
	GcErrINVALID_SERVO_ID   = 0x61
	GcErrGEAR_BUSY          = 0x62
	GcErrMOTOR_STALL        = 0x63
	GcErrMOTOR_TIMEOUT      = 0x64
	GcErrSERVO_OUT_OF_RANGE = 0x65
	GcErrINA226_ERROR       = 0x66
	GcErrYAW_NOT_AVAILABLE  = 0x67
	GcErrINVALID_ACTION     = 0x68
	GcErrNO_CURRENT_MONITOR = 0x69
	GcErrNOT_CALIBRATING    = 0x6A
	GcErrGEAR_DISABLED      = 0x6B
)

// ─── HubFX Packet Types (0x80-0xAF) ───

const (
	HubSLAVE_LIST          = 0x80
	HubSLAVE_LIST_RESP     = 0x81
	HubSLAVE_INIT          = 0x82
	HubSLAVE_STATUS        = 0x83
	HubAUDIO_PLAY          = 0x84
	HubAUDIO_STOP          = 0x85
	HubAUDIO_VOLUME        = 0x86
	HubAUDIO_FADE          = 0x87
	HubAUDIO_QUEUE         = 0x88
	HubAUDIO_QUEUE_CLEAR   = 0x89
	HubAUDIO_STATUS_REQ    = 0x8A
	HubAUDIO_STATUS_RESP   = 0x8B
	HubENGINE_START        = 0x8C
	HubENGINE_STOP         = 0x8D
	HubENGINE_STATUS_REQ   = 0x8E
	HubENGINE_STATUS_RESP  = 0x8F
	HubCONFIG_RELOAD       = 0x90
	HubCONFIG_STATUS       = 0x91
	HubCONFIG_STATUS_RESP  = 0x92
	HubCONFIG_SAVE         = 0xAC
	HubSD_INIT             = 0x93
	HubSD_STATUS_REQ       = 0x94
	HubSD_STATUS_RESP      = 0x95
	HubSLAVE_ROUTE_GUNFX       = 0x96
	HubSLAVE_ROUTE_LIGHTFX     = 0x97
	HubSLAVE_ROUTE_GEARCONTROL = 0x98
	HubFLASH_STATUS_REQ   = 0x99
	HubFILE_LIST           = 0x9A
	HubFILE_DELETE         = 0x9B
	HubFILE_MKDIR          = 0x9C
	HubFILE_INFO           = 0x9D
	HubFILE_INFO_RESP      = 0x9E
	HubFILE_DOWNLOAD       = 0x9F
	HubFILE_UPLOAD_BEGIN   = 0xA0
	HubFILE_UPLOAD_DATA    = 0xA1
	HubFILE_UPLOAD_END     = 0xA2
	HubFILE_UPLOAD_CANCEL  = 0xA3
	HubFILE_TREE           = 0xA9
	HubUSB_DEVICES_REQ     = 0xA7
	HubUSB_DEVICES_RESP    = 0xA8
	HubUSB_RESET_BUS       = 0xAD
	HubCODEC_STATUS_REQ    = 0xAA
	HubCODEC_STATUS_RESP   = 0xAB
	HubSLAVE_INFO          = 0xAE
	HubSLAVE_INFO_RESP     = 0xAF
)

// Stream protocol
const (
	StreamBEGIN = 0xA4
	StreamDATA  = 0xA5
	StreamEND   = 0xA6
)

// ─── HubFX Error Codes (0x80-0x8F) ───

const (
	HubErrSLAVE_NOT_FOUND     = 0x80
	HubErrSLAVE_NOT_CONNECTED = 0x81
	HubErrSLAVE_INIT_FAILED   = 0x82
	HubErrNO_SLAVES            = 0x83
	HubErrSLAVE_COMM_ERROR    = 0x84
	HubErrAUDIO_ERROR          = 0x85
	HubErrSD_NOT_INITIALIZED   = 0x86
	HubErrENGINE_NOT_AVAILABLE = 0x87
	HubErrCONFIG_ERROR         = 0x88
	HubErrINVALID_CHANNEL      = 0x89
	HubErrFILE_NOT_FOUND       = 0x8A
	HubErrFILE_ALREADY_EXISTS  = 0x8B
	HubErrFILE_IO_ERROR        = 0x8C
	HubErrFILE_TOO_LARGE       = 0x8D
	HubErrUPLOAD_IN_PROGRESS   = 0x8E
	HubErrNO_UPLOAD_ACTIVE     = 0x8F
)

// ─── HubFX Audio Constants ───

const (
	AudioOutputCH1 = 0x01
	AudioOutputCH2 = 0x02
	AudioOutputALL = 0x03

	AudioLoopNone     = 0
	AudioLoopFinite   = 1
	AudioLoopInfinite = 2

	AudioQueueFinishLoop = 0
	AudioQueueStopNow    = 1

	AudioChAll     = 0xFF
	AudioMaxChans  = 8
)

// ─── HubFX Storage Constants ───

const (
	StorageTargetSD    = 0
	StorageTargetFlash = 1
)

// ─── Slave Types ───

const (
	SlaveUnknown     = 0
	SlaveGunFX       = 1
	SlaveLightFX     = 2
	SlaveGearControl = 3
)

// ─── Controller Types ───

const (
	CtrlGearControl = "gearcontrol"
	CtrlGunFX       = "gunfx"
	CtrlHubFX       = "hubfx"
	CtrlLightFX     = "lightfx"
	CtrlNoOp        = "noop"
)

// ─── Name Lookup Tables ───

// ErrorName returns a human-readable name for an error code.
func ErrorName(code byte) string {
	names := map[byte]string{
		// Core
		0x00: "OK", 0x01: "UNKNOWN", 0x02: "NOT_INITIALIZED",
		0x03: "INVALID_COMMAND", 0x04: "MISSING_PARAM", 0x05: "BUSY",
		0x06: "NOT_SUPPORTED", 0x10: "INVALID_PARAM", 0x11: "PARAM_RANGE",
		0x12: "INVALID_ID", 0x13: "INVALID_VALUE", 0x14: "PARAM_TOO_LONG",
		0xF0: "INTERNAL", 0xF1: "TIMEOUT", 0xF2: "COMM_ERROR", 0xF4: "CRC_ERROR",
		// GunFX
		0x20: "SERVO_INVALID_ID", 0x21: "SERVO_PULSE_RANGE",
		0x22: "SERVO_MIN_MAX", 0x23: "SERVO_NOT_CONFIGURED",
		0x30: "INVALID_FAN_SPEED", 0x31: "HEATER_DISCONNECTED",
		0x32: "FAN_DISCONNECTED", 0x33: "HEATER_OVERCURRENT",
		0x34: "FAN_OVERCURRENT", 0x40: "INVALID_RPM",
		0x41: "ALREADY_FIRING", 0x42: "NOT_FIRING",
		// LightFX
		0x50: "INVALID_CHANNEL", 0x51: "SEQ_FULL",
		0x52: "INVALID_EVENT", 0x53: "LFX_INVALID_PARAM",
		0x54: "INVALID_SERVO", 0x55: "INVALID_SLOT", 0x56: "CHANNEL_DISABLED",
		// GearControl
		0x60: "INVALID_GEAR_ID", 0x61: "GC_INVALID_SERVO_ID",
		0x62: "GEAR_BUSY", 0x63: "MOTOR_STALL", 0x64: "MOTOR_TIMEOUT",
		0x65: "SERVO_OUT_OF_RANGE", 0x66: "INA226_ERROR",
		0x67: "YAW_NOT_AVAILABLE", 0x68: "INVALID_ACTION",
		0x69: "NO_CURRENT_MONITOR", 0x6A: "NOT_CALIBRATING", 0x6B: "GEAR_DISABLED",
		// HubFX
		0x80: "SLAVE_NOT_FOUND", 0x81: "SLAVE_NOT_CONNECTED",
		0x82: "SLAVE_INIT_FAILED", 0x83: "NO_SLAVES",
		0x84: "SLAVE_COMM_ERROR", 0x85: "AUDIO_ERROR",
		0x86: "SD_NOT_INITIALIZED", 0x87: "ENGINE_NOT_AVAILABLE",
		0x88: "CONFIG_ERROR", 0x89: "HUB_INVALID_CHANNEL",
		0x8A: "FILE_NOT_FOUND", 0x8B: "FILE_ALREADY_EXISTS",
		0x8C: "FILE_IO_ERROR", 0x8D: "FILE_TOO_LARGE",
		0x8E: "UPLOAD_IN_PROGRESS", 0x8F: "NO_UPLOAD_ACTIVE",
	}
	if name, ok := names[code]; ok {
		return name
	}
	return fmt.Sprintf("UNKNOWN(0x%02X)", code)
}

// PacketTypeName returns a human-readable name for a packet type.
func PacketTypeName(ptype byte) string {
	names := map[byte]string{
		// Core
		CoreINIT: "INIT", CoreINIT_READY: "INIT_READY", CoreACK: "ACK",
		CoreNACK: "NACK", CoreSTATUS: "STATUS", CoreSTATUS_REQ: "STATUS_REQ",
		CoreERROR: "ERROR", CoreSHUTDOWN: "SHUTDOWN", CoreREBOOT: "REBOOT",
		CoreBOOTSEL: "BOOTSEL", CoreKEEPALIVE: "KEEPALIVE",
		CoreI2C_SCAN: "I2C_SCAN", CoreI2C_SCAN_RES: "I2C_SCAN_RESULT",
		CoreIDENTIFY: "IDENTIFY", CoreLOG_MESSAGE: "LOG_MESSAGE",
		CoreDIAG_HISTORY: "DIAG_HISTORY",
		// GunFX
		GfxTRIGGER_ON: "GFX.TRIGGER_ON", GfxTRIGGER_OFF: "GFX.TRIGGER_OFF",
		GfxSERVO_SET: "GFX.SERVO_SET", GfxSERVO_SETTINGS: "GFX.SERVO_SETTINGS",
		GfxSERVO_RECOIL: "GFX.SERVO_RECOIL", GfxSMOKE_HEAT: "GFX.SMOKE_HEAT",
		GfxSMOKE_SETTINGS: "GFX.SMOKE_SETTINGS", GfxSMOKE_RESET: "GFX.SMOKE_RESET",
		GfxSMOKE_CURRENT_LIMIT: "GFX.SMOKE_CURRENT_LIMIT",
		// LightFX
		LfxLED_SET: "LFX.LED_SET", LfxLED_OFF: "LFX.LED_OFF",
		LfxLED_SEQ_CLEAR: "LFX.LED_SEQ_CLEAR", LfxLED_SEQ_ADD: "LFX.LED_SEQ_ADD",
		LfxLED_SEQ_START: "LFX.LED_SEQ_START", LfxLED_SEQ_STOP: "LFX.LED_SEQ_STOP",
		LfxSERVO_SET: "LFX.SERVO_SET", LfxSERVO_SETTINGS: "LFX.SERVO_SETTINGS",
		LfxLED_MASTER_BRIGHTNESS: "LFX.LED_MASTER_BRIGHTNESS",
		LfxLED_RESET: "LFX.LED_RESET", LfxLED_ENABLE: "LFX.LED_ENABLE",
		LfxLANDING_LIGHT_BIND: "LFX.LANDING_LIGHT_BIND",
		LfxLANDING_LIGHT_DEPLOY: "LFX.LANDING_LIGHT_DEPLOY",
		LfxLANDING_LIGHT_RETRACT: "LFX.LANDING_LIGHT_RETRACT",
		// GearControl
		GcGEAR_DEPLOY: "GC.GEAR_DEPLOY", GcGEAR_RETRACT: "GC.GEAR_RETRACT",
		GcGEAR_STOP: "GC.GEAR_STOP", GcGEAR_ALL: "GC.GEAR_ALL",
		GcSERVO_SET: "GC.SERVO_SET", GcSRV_SETTINGS: "GC.SRV_SETTINGS",
		GcGEAR_CONFIG: "GC.GEAR_CONFIG", GcDOOR_CONFIG: "GC.DOOR_CONFIG",
		GcYAW_CONFIG: "GC.YAW_CONFIG", GcYAW_INPUT: "GC.YAW_INPUT",
		GcGEAR_CALIBRATE: "GC.GEAR_CALIBRATE", GcGEAR_CALIB_STATUS: "GC.CALIB_STATUS",
		GcGEAR_CALIB_CANCEL: "GC.CALIB_CANCEL", GcBATTERY_CONFIG: "GC.BATTERY_CONFIG",
		GcDOOR_MODE: "GC.DOOR_MODE", GcGEAR_RESET: "GC.GEAR_RESET",
		GcGEAR_SEQ_STATUS: "GC.GC_SEQ_STATUS", GcGEAR_ENABLE: "GC.GEAR_ENABLE",
		GcGEAR_DOOR_STATUS: "GC.DOOR_STATUS",
		// HubFX
		HubSLAVE_LIST: "HUB.SLAVE_LIST", HubSLAVE_LIST_RESP: "HUB.SLAVE_LIST_RESP",
		HubSLAVE_INIT: "HUB.SLAVE_INIT", HubSLAVE_STATUS: "HUB.SLAVE_STATUS",
		HubAUDIO_PLAY: "HUB.AUDIO_PLAY", HubAUDIO_STOP: "HUB.AUDIO_STOP",
		HubAUDIO_VOLUME: "HUB.AUDIO_VOLUME", HubAUDIO_FADE: "HUB.AUDIO_FADE",
		HubAUDIO_QUEUE: "HUB.AUDIO_QUEUE", HubAUDIO_QUEUE_CLEAR: "HUB.AUDIO_QUEUE_CLEAR",
		HubAUDIO_STATUS_REQ: "HUB.AUDIO_STATUS_REQ", HubAUDIO_STATUS_RESP: "HUB.AUDIO_STATUS_RESP",
		HubENGINE_START: "HUB.ENGINE_START", HubENGINE_STOP: "HUB.ENGINE_STOP",
		HubENGINE_STATUS_REQ: "HUB.ENGINE_STATUS_REQ", HubENGINE_STATUS_RESP: "HUB.ENGINE_STATUS_RESP",
		HubCONFIG_RELOAD: "HUB.CONFIG_RELOAD", HubCONFIG_STATUS: "HUB.CONFIG_STATUS",
		HubCONFIG_STATUS_RESP: "HUB.CONFIG_STATUS_RESP", HubCONFIG_SAVE: "HUB.CONFIG_SAVE",
		HubSD_INIT: "HUB.SD_INIT", HubSD_STATUS_REQ: "HUB.SD_STATUS_REQ",
		HubSD_STATUS_RESP: "HUB.SD_STATUS_RESP",
		HubSLAVE_ROUTE_GUNFX: "HUB.SLAVE_ROUTE_GUNFX",
		HubSLAVE_ROUTE_LIGHTFX: "HUB.SLAVE_ROUTE_LIGHTFX",
		HubSLAVE_ROUTE_GEARCONTROL: "HUB.SLAVE_ROUTE_GEARCONTROL",
		HubFLASH_STATUS_REQ: "HUB.FLASH_STATUS_REQ",
		HubFILE_LIST: "HUB.FILE_LIST", HubFILE_DELETE: "HUB.FILE_DELETE",
		HubFILE_MKDIR: "HUB.FILE_MKDIR", HubFILE_INFO: "HUB.FILE_INFO",
		HubFILE_INFO_RESP: "HUB.FILE_INFO_RESP",
		HubFILE_DOWNLOAD: "HUB.FILE_DOWNLOAD",
		HubFILE_UPLOAD_BEGIN: "HUB.FILE_UPLOAD_BEGIN",
		HubFILE_UPLOAD_DATA: "HUB.FILE_UPLOAD_DATA",
		HubFILE_UPLOAD_END: "HUB.FILE_UPLOAD_END",
		HubFILE_UPLOAD_CANCEL: "HUB.FILE_UPLOAD_CANCEL",
		HubFILE_TREE: "HUB.FILE_TREE",
		HubUSB_DEVICES_REQ: "HUB.USB_DEVICES_REQ",
		HubUSB_DEVICES_RESP: "HUB.USB_DEVICES_RESP",
		HubUSB_RESET_BUS: "HUB.USB_RESET_BUS",
		HubCODEC_STATUS_REQ: "HUB.CODEC_STATUS_REQ",
		HubCODEC_STATUS_RESP: "HUB.CODEC_STATUS_RESP",
		HubSLAVE_INFO: "HUB.SLAVE_INFO", HubSLAVE_INFO_RESP: "HUB.SLAVE_INFO_RESP",
		// Streaming
		StreamBEGIN: "STREAM_BEGIN", StreamDATA: "STREAM_DATA", StreamEND: "STREAM_END",
	}
	if name, ok := names[ptype]; ok {
		return name
	}
	return fmt.Sprintf("0x%02X", ptype)
}

// GearStateName returns gear state name.
func GearStateName(state byte) string {
	names := map[byte]string{
		0: "UNKNOWN", 1: "DEPLOYED", 2: "RETRACTED",
		3: "DEPLOYING", 4: "RETRACTING", 5: "ERROR", 6: "CALIBRATING",
	}
	if n, ok := names[state]; ok {
		return n
	}
	return fmt.Sprintf("?(%d)", state)
}

// DoorModeName returns door mode name.
func DoorModeName(mode byte) string {
	names := map[byte]string{
		0: "NONE", 1: "SINGLE", 2: "DUAL_SYNC", 3: "DUAL_DELAY", 4: "DUAL_SEQ",
	}
	if n, ok := names[mode]; ok {
		return n
	}
	return fmt.Sprintf("?(%d)", mode)
}

// DoorStateName returns door state name.
func DoorStateName(state byte) string {
	names := map[byte]string{
		0: "unknown", 1: "closed", 2: "open", 3: "opening", 4: "closing",
	}
	if n, ok := names[state]; ok {
		return n
	}
	return fmt.Sprintf("?(%d)", state)
}

// EngineStateName returns engine state name.
func EngineStateName(state byte) string {
	names := map[byte]string{0: "Stopped", 1: "Starting", 2: "Running", 3: "Stopping"}
	if n, ok := names[state]; ok {
		return n
	}
	return fmt.Sprintf("Unknown(%d)", state)
}

// SlaveTypeName returns slave type name.
func SlaveTypeName(stype byte) string {
	names := map[byte]string{0: "Unknown", 1: "GunFX", 2: "LightFX", 3: "GearControl"}
	if n, ok := names[stype]; ok {
		return n
	}
	return fmt.Sprintf("Unknown(%d)", stype)
}

// DiagLevelName returns diagnostic log level name.
func DiagLevelName(level byte) string {
	names := map[byte]string{0: "DEBUG", 1: "INFO", 2: "WARN", 3: "ERROR"}
	if n, ok := names[level]; ok {
		return n
	}
	return fmt.Sprintf("L%d", level)
}

// DetectControllerType determines controller type from device name.
func DetectControllerType(name string) string {
	for _, entry := range []struct {
		prefix string
		ctype  string
	}{
		{"GearControl", CtrlGearControl},
		{"GunFX", CtrlGunFX},
		{"HubFX", CtrlHubFX},
		{"LightFX", CtrlLightFX},
		{"NoOp", CtrlNoOp},
	} {
		if strings.HasPrefix(name, entry.prefix) {
			return entry.ctype
		}
	}
	return ""
}
