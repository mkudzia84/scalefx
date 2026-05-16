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
	Init        protocol.PacketType = 0xF0
	Shutdown    protocol.PacketType = 0xF1
	Keepalive   protocol.PacketType = 0xF2
	InitReady   protocol.PacketType = 0xF3
	Status      protocol.PacketType = 0xF4
	Error       protocol.PacketType = 0xF5
	Ack         protocol.PacketType = 0xF6
	Nack        protocol.PacketType = 0xF7
	Reboot      protocol.PacketType = 0xF8
	Bootsel     protocol.PacketType = 0xF9
	StatusReq   protocol.PacketType = 0xFA
	I2cScan     protocol.PacketType = 0xFB
	I2cScanRes  protocol.PacketType = 0xFC
	LogMessage  protocol.PacketType = 0xFD
	Identify    protocol.PacketType = 0xFE
	DiagHistory protocol.PacketType = 0xFF
	StatusUpdate  protocol.PacketType = 0xEF
	BatteryConfig protocol.PacketType = 0xEE
)

// Battery chemistry wire-format values (match C++ BatteryChemistry enum).
const (
	ChemistryLiPo  byte = 0
	ChemistryLiIon byte = 1
	ChemistryNiMH  byte = 2
)

// ChemistryFromString maps a canonical config string to the wire-format byte.
// Unknown values fall back to LiPo.
func ChemistryFromString(s string) byte {
	switch s {
	case "liion":
		return ChemistryLiIon
	case "nimh":
		return ChemistryNiMH
	default:
		return ChemistryLiPo
	}
}

// CmdBatteryConfig — 2-byte payload [chemistry, cellCount] sent to the
// generic BatteryServerT on any board with battery monitoring.
// cellCount == 0 = re-arm auto-detect (1..MAX_CELLS = pinned).
func CmdBatteryConfig(chemistry byte, cellCount byte) []byte {
	return protocol.BuildPacket(BatteryConfig, []byte{chemistry, cellCount}, 0)
}

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

// ─── Init Mode — mirrors InitMode namespace in core.h ───

const (
	InitModeSlave  byte = 0x00
	InitModeDirect byte = 0x01
)

// InitModeName returns a human-readable name for an init mode.
func InitModeName(mode byte) string {
	switch mode {
	case InitModeSlave:
		return "SLAVE"
	case InitModeDirect:
		return "DIRECT"
	default:
		return fmt.Sprintf("0x%02X", mode)
	}
}

// ─── Board State — mirrors BoardState namespace in core.h ───

const (
	BoardStateIdle       byte = 0x00
	BoardStateStandalone byte = 0x01
	BoardStateSlave      byte = 0x02
	BoardStateDirect     byte = 0x03
)

// BoardStateName returns a human-readable name for a board state.
func BoardStateName(state byte) string {
	switch state {
	case BoardStateIdle:
		return "IDLE"
	case BoardStateStandalone:
		return "STANDALONE"
	case BoardStateSlave:
		return "SLAVE"
	case BoardStateDirect:
		return "DIRECT"
	default:
		return fmt.Sprintf("0x%02X", state)
	}
}

// ─── Init Flags — mirrors InitFlags namespace in core.h ───

const (
	InitFlagNone    byte = 0x00
	InitFlagVerbose byte = 0x01
)

// StatusCoreHeaderSize is the size of the STATUS response core header in bytes.
// 5×u32 (counter, uptime, freeRam, lastActivity, keepalives) + boardState:u8 + initFlags:u8
const StatusCoreHeaderSize = 22

// ─── Capability Bitmask ───
// Mirrors CoreCapability namespace in
// [controllers/lib/sfx_serial/serial/core/core.h]. Appended to
// IDENTIFY/INIT_READY payload (Rule 11 append-only). A 0 bitmask means the
// firmware pre-dates the field — callers should fall back to probing rather
// than treating it as "no interfaces present".

const (
	CapFlash    uint32 = 1 << 0 // LittleFS flash storage commands available
	CapSd       uint32 = 1 << 1 // SD card storage commands available (slot present)
	CapAudio    uint32 = 1 << 2 // AudioMixer + audio playback commands available
	CapUsbHost  uint32 = 1 << 3 // USB host stack + device enumeration available
	CapEngine   uint32 = 1 << 4 // Sound engine commands available
	CapConfig   uint32 = 1 << 5 // YAML config store commands available
	CapSlaveBus uint32 = 1 << 6 // Master can enumerate / route to slaves
	CapBattery  uint32 = 1 << 7 // Battery sensor present (BATTERY_INFO_REQ / BATTERY_ALERT supported, status broadcast carries the battery section)
)

// HasCapability returns true if every bit in want is set in caps.
func HasCapability(caps, want uint32) bool { return caps&want == want }

// CapabilityNames returns short names for the bits set in caps, in order.
func CapabilityNames(caps uint32) []string {
	defs := []struct {
		bit  uint32
		name string
	}{
		{CapFlash, "FLASH"},
		{CapSd, "SD"},
		{CapAudio, "AUDIO"},
		{CapUsbHost, "USB_HOST"},
		{CapEngine, "ENGINE"},
		{CapConfig, "CONFIG"},
		{CapSlaveBus, "SLAVE_BUS"},
		{CapBattery, "BATTERY"},
	}
	out := []string{}
	for _, d := range defs {
		if caps&d.bit != 0 {
			out = append(out, d.name)
		}
	}
	return out
}

// ─── Status Update — mirrors StatusUpdateSource / StatusUpdateType in core.h ───

const (
	StatusUpdateSourceGunFX       byte = 0x01
	StatusUpdateSourceLightFX     byte = 0x40
	StatusUpdateSourceGearControl byte = 0x60
	StatusUpdateSourceHubFX       byte = 0x80
	StatusUpdateSourceCore        byte = 0xF0
)

const (
	StatusUpdateServoPosition  byte = 0x01
	StatusUpdateVoltage        byte = 0x02
	StatusUpdateCurrent        byte = 0x03
	StatusUpdateTemperature    byte = 0x04
	StatusUpdateStatusBroadcast byte = 0x10 // Full module status blob (same format as STATUS module data)
)

// ─── Commands ───

func CmdInit() []byte      { return protocol.BuildPacket(Init, nil, 0) }
func CmdInitMode(mode, flags byte) []byte {
	return protocol.BuildPacket(Init, []byte{mode, flags}, 0)
}
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
		Init:         "INIT",
		Shutdown:     "SHUTDOWN",
		Keepalive:    "KEEPALIVE",
		InitReady:    "INIT_READY",
		Status:       "STATUS",
		Error:        "ERROR",
		Ack:          "ACK",
		Nack:         "NACK",
		Reboot:       "REBOOT",
		Bootsel:      "BOOTSEL",
		StatusReq:    "STATUS_REQ",
		I2cScan:      "I2C_SCAN",
		I2cScanRes:   "I2C_SCAN_RESULT",
		LogMessage:   "LOG_MESSAGE",
		Identify:     "IDENTIFY",
		DiagHistory:  "DIAG_HISTORY",
		StatusUpdate:  "STATUS_UPDATE",
		BatteryConfig: "BATTERY_CONFIG",
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
