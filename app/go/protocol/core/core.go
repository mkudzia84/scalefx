package core

// Core Protocol — mirrors serial/core/core.h
// System-level packet types, generic error codes, core commands.

import (
	"fmt"
	"strings"

	"scalefx/protocol"
)

// ─── Core Packet Types (0xF0-0xFF) ───

const (
	Init       protocol.PacketType = 0xF0
	Shutdown   protocol.PacketType = 0xF1
	Keepalive  protocol.PacketType = 0xF2
	InitReady  protocol.PacketType = 0xF3
	Status     protocol.PacketType = 0xF4
	Error      protocol.PacketType = 0xF5
	Ack        protocol.PacketType = 0xF6
	Nack       protocol.PacketType = 0xF7
	Reboot     protocol.PacketType = 0xF8
	Bootsel    protocol.PacketType = 0xF9
	StatusReq  protocol.PacketType = 0xFA
	I2cScan    protocol.PacketType = 0xFB
	I2cScanRes protocol.PacketType = 0xFC
	LogMessage protocol.PacketType = 0xFD
	Identify   protocol.PacketType = 0xFE
	DiagHistory protocol.PacketType = 0xFF
)

// ─── Generic Error Codes — mirrors SerialError namespace ───

const (
	ErrOk             protocol.ErrorCode = 0x00
	ErrUnknown        protocol.ErrorCode = 0x01
	ErrNotInitialized protocol.ErrorCode = 0x02
	ErrInvalidCommand protocol.ErrorCode = 0x03
	ErrMissingParam   protocol.ErrorCode = 0x04
	ErrBusy           protocol.ErrorCode = 0x05
	ErrNotSupported   protocol.ErrorCode = 0x06
	ErrPermissionDenied protocol.ErrorCode = 0x07
	ErrInvalidParam   protocol.ErrorCode = 0x10
	ErrParamRange     protocol.ErrorCode = 0x11
	ErrInvalidId      protocol.ErrorCode = 0x12
	ErrInvalidValue   protocol.ErrorCode = 0x13
	ErrParamTooLong   protocol.ErrorCode = 0x14
	ErrInternal       protocol.ErrorCode = 0xF0
	ErrTimeout        protocol.ErrorCode = 0xF1
	ErrCommError      protocol.ErrorCode = 0xF2
	ErrBufferOverflow protocol.ErrorCode = 0xF3
	ErrCrcError       protocol.ErrorCode = 0xF4
	ErrFramingError   protocol.ErrorCode = 0xF5
)

// ─── Controller Type Strings ───

const (
	CtrlGearControl = "gearcontrol"
	CtrlGunFX       = "gunfx"
	CtrlHubFX       = "hubfx"
	CtrlLightFX     = "lightfx"
	CtrlNoOp        = "noop"
)

// ─── Commands ───

func CmdInit() []byte      { return protocol.BuildPacket(Init, nil, 0) }
func CmdShutdown() []byte  { return protocol.BuildPacket(Shutdown, nil, 0) }
func CmdKeepalive() []byte { return protocol.BuildPacket(Keepalive, nil, 0) }
func CmdReboot() []byte    { return protocol.BuildPacket(Reboot, nil, 0) }
func CmdBootsel() []byte   { return protocol.BuildPacket(Bootsel, nil, 0) }
func CmdStatusReq() []byte { return protocol.BuildPacket(StatusReq, nil, 0) }
func CmdIdentify() []byte  { return protocol.BuildPacket(Identify, nil, 0) }
func CmdI2cScan() []byte   { return protocol.BuildPacket(I2cScan, nil, 0) }

func CmdDiagHistory(count byte) []byte {
	if count == 0 {
		return protocol.BuildPacket(DiagHistory, nil, 0)
	}
	return protocol.BuildPacket(DiagHistory, []byte{count}, 0)
}

// ─── Name Lookups ───

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

// ─── Name Registration ───

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		Init:        "INIT",
		Shutdown:    "SHUTDOWN",
		Keepalive:   "KEEPALIVE",
		InitReady:   "INIT_READY",
		Status:      "STATUS",
		Error:       "ERROR",
		Ack:         "ACK",
		Nack:        "NACK",
		Reboot:      "REBOOT",
		Bootsel:     "BOOTSEL",
		StatusReq:   "STATUS_REQ",
		I2cScan:     "I2C_SCAN",
		I2cScanRes:  "I2C_SCAN_RESULT",
		LogMessage:  "LOG_MESSAGE",
		Identify:    "IDENTIFY",
		DiagHistory: "DIAG_HISTORY",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrOk:             "OK",
		ErrUnknown:        "UNKNOWN",
		ErrNotInitialized: "NOT_INITIALIZED",
		ErrInvalidCommand: "INVALID_COMMAND",
		ErrMissingParam:   "MISSING_PARAM",
		ErrBusy:           "BUSY",
		ErrNotSupported:   "NOT_SUPPORTED",
		ErrPermissionDenied: "PERMISSION_DENIED",
		ErrInvalidParam:   "INVALID_PARAM",
		ErrParamRange:     "PARAM_RANGE",
		ErrInvalidId:      "INVALID_ID",
		ErrInvalidValue:   "INVALID_VALUE",
		ErrParamTooLong:   "PARAM_TOO_LONG",
		ErrInternal:       "INTERNAL",
		ErrTimeout:        "TIMEOUT",
		ErrCommError:      "COMM_ERROR",
		ErrBufferOverflow: "BUFFER_OVERFLOW",
		ErrCrcError:       "CRC_ERROR",
		ErrFramingError:   "FRAMING_ERROR",
	})
}
