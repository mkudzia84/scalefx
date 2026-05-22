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

	// Config-store wire surface — owned by sfx_config / ConfigServicePolicy.
	// Mirrors `controllers/lib/sfx_config/protocol/config_protocol.h`.
	ConfigReload     protocol.PacketType = 0x90 //  [] or [pathLen:u8][path] → ACK
	ConfigStatus     protocol.PacketType = 0x91 //  []                       → ConfigStatusResp
	ConfigStatusResp protocol.PacketType = 0x92 //  [loaded:u8][size:u16LE][validOk:u8]
	ConfigSave       protocol.PacketType = 0xAC //  [] or [pathLen:u8][path] → ACK
)

// CmdConfigReload — re-read every registered config store (or a single
// path when one is supplied).  Server fan-out walks each ConfigStore's
// `onLoaded` callback so effect services re-configure in one shot.
func CmdConfigReload() []byte { return protocol.BuildPacket(ConfigReload, nil, 0) }

// CmdConfigReloadPath — reload only the matching store.
func CmdConfigReloadPath(path string) []byte {
	buf := make([]byte, 1+len(path))
	buf[0] = byte(len(path))
	copy(buf[1:], path)
	return protocol.BuildPacket(ConfigReload, buf, 0)
}

// CmdConfigStatus — request aggregate config status (all stores).
func CmdConfigStatus() []byte { return protocol.BuildPacket(ConfigStatus, nil, 0) }

// CmdConfigSavePath — save in-memory state for the named store back to flash.
func CmdConfigSavePath(path string) []byte {
	buf := make([]byte, 1+len(path))
	buf[0] = byte(len(path))
	copy(buf[1:], path)
	return protocol.BuildPacket(ConfigSave, buf, 0)
}

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

// ─── Capability Bitmask — board feature catalog ───
//
// Mirrors the `CoreCapability` namespace in
// [controllers/lib/sfx_serial/serial/core/core.h] verbatim.  Append-only
// (Rule 11) — never renumber bits; assign new features to reserved
// positions.  0 simply means "no capabilities advertised" (e.g. a no-op
// firmware with no user policies).
//
// Domains:
//   Storage                   bits 0..1   (FLASH, SD)
//   Audio                     bit 2       (AUDIO)
//   Comm / bus                bits 3, 6   (USB_HOST, EXPANDER_BUS)
//   Logic / config            bits 4..5   (ENGINE, CONFIG)
//   Sensors                   bit 7       (BATTERY)
//   Generic-expander services bits 8..10  (PORTS, ROLES, TOPOLOGY)
//   Effect policies           bits 11..15 (ALERTS, LIGHTFX, LANDING, GEARCTRL, GUNFX)
//   Port-kind presence        bits 16..19 (HAS_*_PORTS)
//   Reserved                  bits 20..31

const (
	// Storage
	CapFlash uint32 = 1 << 0 // LittleFS storage commands
	CapSd    uint32 = 1 << 1 // SD card storage commands

	// Audio
	CapAudio uint32 = 1 << 2 // AudioMixer + audio playback commands

	// Comm / bus
	CapUsbHost     uint32 = 1 << 3 // USB host stack + device enumeration
	CapExpanderBus uint32 = 1 << 6 // Master can enumerate / route to expanders
	CapSlaveBus    uint32 = CapExpanderBus // legacy alias

	// Logic / config
	CapEngine uint32 = 1 << 4 // Sound-engine commands
	CapConfig uint32 = 1 << 5 // YAML config store commands

	// Sensors
	CapBattery uint32 = 1 << 7 // Battery sensor present

	// Generic-expander services (bits 8..10)
	CapPorts    uint32 = 1 << 8  // PortServicePolicy raw-port commands (0x10..0x3F)
	CapRoles    uint32 = 1 << 9  // RoleServicePolicy attach/detach + per-role commands (0x40..0x7F)
	CapTopology uint32 = 1 << 10 // TopologyServicePolicy GUID-addressed access (master only)

	// Effect policies (bits 11..15) — one bit per master-side effect
	// service, set by each policy's `kCapabilityBits`.  Lets the host
	// hide a verb when its backing policy isn't actually loaded.
	CapAlerts         uint32 = 1 << 11
	CapLightFx        uint32 = 1 << 12
	CapLandingLights  uint32 = 1 << 13
	CapGearCtrl       uint32 = 1 << 14
	CapGunFx          uint32 = 1 << 15

	// Port-kind presence (bits 16..19) — populated by BoardOf<>::begin().
	CapHasServoPorts   uint32 = 1 << 16
	CapHasPwmPorts     uint32 = 1 << 17
	CapHasHBridgePorts uint32 = 1 << 18
	CapHasInputPorts   uint32 = 1 << 19
)

// HasCapability returns true iff every bit in want is set in caps.
func HasCapability(caps, want uint32) bool { return caps&want == want }

// capDef is one entry in the feature catalog used by CapabilityNames.
type capDef struct {
	bit  uint32
	name string
}

// capCatalog is the canonical ordered list of feature bits.  Used by
// `CapabilityNames` and by CLI / Studio for display.  Adding a new bit
// is a one-line append here (after adding it to the const block above).
var capCatalog = []capDef{
	{CapFlash, "FLASH"},
	{CapSd, "SD"},
	{CapAudio, "AUDIO"},
	{CapUsbHost, "USB_HOST"},
	{CapEngine, "ENGINE"},
	{CapConfig, "CONFIG"},
	{CapExpanderBus, "EXPANDER_BUS"},
	{CapBattery, "BATTERY"},
	{CapPorts, "PORTS"},
	{CapRoles, "ROLES"},
	{CapTopology, "TOPOLOGY"},
	{CapAlerts, "ALERTS"},
	{CapLightFx, "LIGHTFX"},
	{CapLandingLights, "LANDING_LIGHTS"},
	{CapGearCtrl, "GEARCTRL"},
	{CapGunFx, "GUNFX"},
	{CapHasServoPorts, "HAS_SERVO_PORTS"},
	{CapHasPwmPorts, "HAS_PWM_PORTS"},
	{CapHasHBridgePorts, "HAS_HBRIDGE_PORTS"},
	{CapHasInputPorts, "HAS_INPUT_PORTS"},
}

// CapabilityNames returns symbolic names for the bits set in caps, in
// catalog order.
func CapabilityNames(caps uint32) []string {
	out := []string{}
	for _, d := range capCatalog {
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
