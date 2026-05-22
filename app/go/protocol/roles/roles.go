// Package roles mirrors serial/roles.h — runtime role attach/detach and
// per-role wire commands.  A role is a behaviour ("LED animator", "DC
// motor", "heater", ...) that runs on top of one declared port; each
// port hosts at most one role at a time.
package roles

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol"
)

// ─── RoleKind — append-only u8 enum (Rule 11) ─────────────────────────

const (
	KindNone          byte = 0x00
	KindServoActuator byte = 0x01
	KindRcPwmInput    byte = 0x02
	KindSbusInput     byte = 0x03
	KindJetiExInput   byte = 0x04
	KindLedAnimator   byte = 0x10
	KindDcMotor       byte = 0x11
	KindHeater        byte = 0x12
	KindBiDcMotor     byte = 0x20
	KindReserved      byte = 0xFF
)

// KindName returns the canonical wire name for a role kind.
func KindName(k byte) string {
	switch k {
	case KindNone:
		return "none"
	case KindServoActuator:
		return "servo-actuator"
	case KindRcPwmInput:
		return "rc-pwm-input"
	case KindSbusInput:
		return "sbus-input"
	case KindJetiExInput:
		return "jeti-ex-input"
	case KindLedAnimator:
		return "led-animator"
	case KindDcMotor:
		return "dc-motor"
	case KindHeater:
		return "heater"
	case KindBiDcMotor:
		return "bi-dc-motor"
	default:
		return fmt.Sprintf("0x%02X", k)
	}
}

// KindFromName converts a canonical role name to its u8 wire constant,
// returning (KindNone, false) if unknown.
func KindFromName(s string) (byte, bool) {
	switch s {
	case "none":
		return KindNone, true
	case "servo-actuator", "servo":
		return KindServoActuator, true
	case "rc-pwm-input", "rcpwm", "rc-in":
		return KindRcPwmInput, true
	case "sbus-input", "sbus":
		return KindSbusInput, true
	case "jeti-ex-input", "jeti-ex", "jetiex":
		return KindJetiExInput, true
	case "led-animator", "led":
		return KindLedAnimator, true
	case "dc-motor", "motor":
		return KindDcMotor, true
	case "heater":
		return KindHeater, true
	case "bi-dc-motor", "bimotor":
		return KindBiDcMotor, true
	}
	return KindNone, false
}

// ─── Packet types (0x40..0x7F) ────────────────────────────────────────

const (
	// Attach / detach / enumeration (0x40..0x47)
	RoleAttach     protocol.PacketType = 0x40
	RoleDetach     protocol.PacketType = 0x41
	RoleListReq    protocol.PacketType = 0x42
	RoleListResp   protocol.PacketType = 0x43
	RoleAttached   protocol.PacketType = 0x44
	RoleDetached   protocol.PacketType = 0x45

	// Servo actuator (0x48..0x4F)
	ServoSetTarget     protocol.PacketType = 0x48
	ServoGetStatusReq  protocol.PacketType = 0x49
	ServoStatusResp    protocol.PacketType = 0x4A
	ServoTargetReached protocol.PacketType = 0x4B
	ServoMotionUpdate  protocol.PacketType = 0x4C

	// RC PWM input (0x50..0x57)
	RcinGetValueReq    protocol.PacketType = 0x50
	RcinValueResp      protocol.PacketType = 0x51
	RcinSetBroadcastHz protocol.PacketType = 0x52
	RcinValueBroadcast protocol.PacketType = 0x53

	// LED animator (0x58..0x5F)
	LedQueueLoad      protocol.PacketType = 0x58
	LedStart          protocol.PacketType = 0x59
	LedStop           protocol.PacketType = 0x5A
	LedSetBrightness  protocol.PacketType = 0x5B
	LedGetStatusReq   protocol.PacketType = 0x5C
	LedStatusResp     protocol.PacketType = 0x5D
	LedQueueDone      protocol.PacketType = 0x5E

	// DC motor (0x60..0x67)
	MotorSetDuty      protocol.PacketType = 0x60
	MotorBrake        protocol.PacketType = 0x61
	MotorGetStatusReq protocol.PacketType = 0x62
	MotorStatusResp   protocol.PacketType = 0x63
	MotorStallEvent   protocol.PacketType = 0x64

	// Bi-directional DC motor (0x68..0x6F)
	BiMotorSetSigned    protocol.PacketType = 0x68
	BiMotorBrake        protocol.PacketType = 0x69
	BiMotorCoast        protocol.PacketType = 0x6A
	BiMotorGetStatusReq  protocol.PacketType = 0x6B
	BiMotorStatusResp    protocol.PacketType = 0x6C
	BiMotorStallEvent    protocol.PacketType = 0x6D
	BiMotorSeekEndstop   protocol.PacketType = 0x6E
	BiMotorEndstopResult protocol.PacketType = 0x6F

	// Heater (0x70..0x77)
	HeaterSetTarget    protocol.PacketType = 0x70
	HeaterGetStatusReq protocol.PacketType = 0x71
	HeaterStatusResp   protocol.PacketType = 0x72

	// SBUS input (0x78..0x7B)
	SbusGetFrameReq    protocol.PacketType = 0x78
	SbusFrameResp      protocol.PacketType = 0x79
	SbusSetBroadcastHz protocol.PacketType = 0x7A
	SbusFrameBroadcast protocol.PacketType = 0x7B

	// Jeti EX input (0x7C..0x7F)
	JetiExGetFrameReq    protocol.PacketType = 0x7C
	JetiExFrameResp      protocol.PacketType = 0x7D
	JetiExSetBroadcastHz protocol.PacketType = 0x7E
	JetiExFrameBroadcast protocol.PacketType = 0x7F
)

// ─── Error codes (0x40..0x4F) ─────────────────────────────────────────

const (
	ErrRoleNotAttached      protocol.ErrorCode = 0x40
	ErrRoleKindMismatch     protocol.ErrorCode = 0x41
	ErrRoleKindNotSupported protocol.ErrorCode = 0x42
	ErrRoleConfigInvalid    protocol.ErrorCode = 0x43
	ErrRoleNoSenseRequired  protocol.ErrorCode = 0x44
	ErrRoleQueueFull        protocol.ErrorCode = 0x45
)

// ─── Decoded data types ───────────────────────────────────────────────

// RoleListEntry is one (portKind, portIdx, roleKind, flags) entry in
// ROLE_LIST_RESP.
type RoleListEntry struct {
	PortKind byte
	PortIdx  byte
	RoleKind byte
	Flags    byte
}

// DecodeRoleListPayload decodes a ROLE_LIST_RESP payload (without the
// guid prefix added by the topology layer).
func DecodeRoleListPayload(p []byte) ([]RoleListEntry, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("role list: empty payload")
	}
	n := int(p[0])
	if len(p) < 1+4*n {
		return nil, fmt.Errorf("role list: truncated (need %d entries, have %d bytes)", n, len(p))
	}
	out := make([]RoleListEntry, n)
	off := 1
	for i := 0; i < n; i++ {
		out[i] = RoleListEntry{
			PortKind: p[off],
			PortIdx:  p[off+1],
			RoleKind: p[off+2],
			Flags:    p[off+3],
		}
		off += 4
	}
	return out, nil
}

// ServoStatus decodes a SERVO_STATUS_RESP payload.
type ServoStatus struct {
	Index      byte
	PosUs      uint16
	TargetUs   uint16
	VelUsPerMs int16
	Flags      byte
}

func DecodeServoStatus(p []byte) (ServoStatus, error) {
	if len(p) != 8 {
		return ServoStatus{}, fmt.Errorf("servo status: expected 8 bytes, got %d", len(p))
	}
	return ServoStatus{
		Index:      p[0],
		PosUs:      binary.LittleEndian.Uint16(p[1:3]),
		TargetUs:   binary.LittleEndian.Uint16(p[3:5]),
		VelUsPerMs: int16(binary.LittleEndian.Uint16(p[5:7])),
		Flags:      p[7],
	}, nil
}

// LedStatus decodes a LED_STATUS_RESP payload.
type LedStatus struct {
	Index       byte
	Brightness  byte
	Playing     bool
	QueueDepth  byte
}

func DecodeLedStatus(p []byte) (LedStatus, error) {
	if len(p) != 4 {
		return LedStatus{}, fmt.Errorf("led status: expected 4 bytes, got %d", len(p))
	}
	return LedStatus{
		Index:      p[0],
		Brightness: p[1],
		Playing:    p[2] != 0,
		QueueDepth: p[3],
	}, nil
}

// MotorStatus decodes a MOTOR_STATUS_RESP payload.
type MotorStatus struct {
	Index      byte
	Duty       uint16
	VoltageMV  int16
	CurrentMA  int16
	StallFlags byte
}

func DecodeMotorStatus(p []byte) (MotorStatus, error) {
	if len(p) != 8 {
		return MotorStatus{}, fmt.Errorf("motor status: expected 8 bytes, got %d", len(p))
	}
	return MotorStatus{
		Index:      p[0],
		Duty:       binary.LittleEndian.Uint16(p[1:3]),
		VoltageMV:  int16(binary.LittleEndian.Uint16(p[3:5])),
		CurrentMA:  int16(binary.LittleEndian.Uint16(p[5:7])),
		StallFlags: p[7],
	}, nil
}

// SbusFrame decodes an SBUS_FRAME_RESP / SBUS_FRAME_BROADCAST payload.
type SbusFrame struct {
	Index    byte
	Flags    byte
	Channels []uint16
}

// SBUS frame-flags bits.
const (
	SbusFlagValid     byte = 1 << 0
	SbusFlagFailsafe  byte = 1 << 1
	SbusFlagFrameLost byte = 1 << 2
	SbusFlagCh17      byte = 1 << 3
	SbusFlagCh18      byte = 1 << 4
)

func DecodeSbusFrame(p []byte) (SbusFrame, error) {
	if len(p) < 3 {
		return SbusFrame{}, fmt.Errorf("sbus frame: payload too short")
	}
	count := int(p[1])
	if len(p) != 3+2*count {
		return SbusFrame{}, fmt.Errorf("sbus frame: expected %d bytes, got %d", 3+2*count, len(p))
	}
	out := SbusFrame{Index: p[0], Flags: p[2], Channels: make([]uint16, count)}
	for i := 0; i < count; i++ {
		out.Channels[i] = binary.LittleEndian.Uint16(p[3+2*i:])
	}
	return out, nil
}

// ─── Command builders ────────────────────────────────────────────────

func CmdRoleAttach(portKind, portIdx, roleKind byte, cfg []byte) []byte {
	payload := []byte{portKind, portIdx, roleKind, byte(len(cfg))}
	payload = append(payload, cfg...)
	return protocol.BuildPacket(RoleAttach, payload, 0)
}

func CmdRoleDetach(portKind, portIdx byte) []byte {
	return protocol.BuildPacket(RoleDetach, []byte{portKind, portIdx}, 0)
}

func CmdRoleListReq() []byte { return protocol.BuildPacket(RoleListReq, nil, 0) }

func CmdServoSetTarget(portIdx byte, targetUs uint16) []byte {
	return protocol.BuildPacket(ServoSetTarget, append([]byte{portIdx}, protocol.U16LE(targetUs)...), 0)
}
func CmdServoGetStatus(portIdx byte) []byte {
	return protocol.BuildPacket(ServoGetStatusReq, []byte{portIdx}, 0)
}

func CmdLedStart(portIdx byte) []byte           { return protocol.BuildPacket(LedStart, []byte{portIdx}, 0) }
func CmdLedStop(portIdx byte) []byte            { return protocol.BuildPacket(LedStop, []byte{portIdx}, 0) }
func CmdLedSetBrightness(portIdx, b byte) []byte {
	return protocol.BuildPacket(LedSetBrightness, []byte{portIdx, b}, 0)
}
func CmdLedGetStatus(portIdx byte) []byte {
	return protocol.BuildPacket(LedGetStatusReq, []byte{portIdx}, 0)
}

func CmdMotorSetDuty(portIdx byte, duty uint16) []byte {
	return protocol.BuildPacket(MotorSetDuty, append([]byte{portIdx}, protocol.U16LE(duty)...), 0)
}
func CmdMotorBrake(portIdx byte) []byte {
	return protocol.BuildPacket(MotorBrake, []byte{portIdx}, 0)
}
func CmdMotorGetStatus(portIdx byte) []byte {
	return protocol.BuildPacket(MotorGetStatusReq, []byte{portIdx}, 0)
}

func CmdBiMotorSetSigned(portIdx byte, signed int16) []byte {
	return protocol.BuildPacket(BiMotorSetSigned,
		append([]byte{portIdx}, protocol.U16LE(uint16(signed))...), 0)
}
func CmdBiMotorBrake(portIdx byte) []byte {
	return protocol.BuildPacket(BiMotorBrake, []byte{portIdx}, 0)
}
func CmdBiMotorCoast(portIdx byte) []byte {
	return protocol.BuildPacket(BiMotorCoast, []byte{portIdx}, 0)
}

func CmdHeaterSetTarget(portIdx byte, targetCx10 int16) []byte {
	return protocol.BuildPacket(HeaterSetTarget,
		append([]byte{portIdx}, protocol.U16LE(uint16(targetCx10))...), 0)
}

func CmdRcinGetValue(portIdx byte) []byte {
	return protocol.BuildPacket(RcinGetValueReq, []byte{portIdx}, 0)
}
func CmdRcinSetBroadcastHz(portIdx, hz byte) []byte {
	return protocol.BuildPacket(RcinSetBroadcastHz, []byte{portIdx, hz}, 0)
}

func CmdSbusGetFrame(portIdx byte) []byte {
	return protocol.BuildPacket(SbusGetFrameReq, []byte{portIdx}, 0)
}
func CmdSbusSetBroadcastHz(portIdx, hz byte) []byte {
	return protocol.BuildPacket(SbusSetBroadcastHz, []byte{portIdx, hz}, 0)
}

func CmdJetiExGetFrame(portIdx byte) []byte {
	return protocol.BuildPacket(JetiExGetFrameReq, []byte{portIdx}, 0)
}
func CmdJetiExSetBroadcastHz(portIdx, hz byte) []byte {
	return protocol.BuildPacket(JetiExSetBroadcastHz, []byte{portIdx, hz}, 0)
}

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		RoleAttach:           "ROLE_ATTACH",
		RoleDetach:           "ROLE_DETACH",
		RoleListReq:          "ROLE_LIST_REQ",
		RoleListResp:         "ROLE_LIST_RESP",
		RoleAttached:         "ROLE_ATTACHED",
		RoleDetached:         "ROLE_DETACHED",
		ServoSetTarget:       "SERVO_SET_TARGET",
		ServoGetStatusReq:    "SERVO_GET_STATUS_REQ",
		ServoStatusResp:      "SERVO_STATUS_RESP",
		ServoTargetReached:   "SERVO_TARGET_REACHED",
		ServoMotionUpdate:    "SERVO_MOTION_UPDATE",
		RcinGetValueReq:      "RCIN_GET_VALUE_REQ",
		RcinValueResp:        "RCIN_VALUE_RESP",
		RcinSetBroadcastHz:   "RCIN_SET_BROADCAST_HZ",
		RcinValueBroadcast:   "RCIN_VALUE_BROADCAST",
		LedQueueLoad:         "LED_QUEUE_LOAD",
		LedStart:             "LED_START",
		LedStop:              "LED_STOP",
		LedSetBrightness:     "LED_SET_BRIGHTNESS",
		LedGetStatusReq:      "LED_GET_STATUS_REQ",
		LedStatusResp:        "LED_STATUS_RESP",
		LedQueueDone:         "LED_QUEUE_DONE",
		MotorSetDuty:         "MOTOR_SET_DUTY",
		MotorBrake:           "MOTOR_BRAKE",
		MotorGetStatusReq:    "MOTOR_GET_STATUS_REQ",
		MotorStatusResp:      "MOTOR_STATUS_RESP",
		MotorStallEvent:      "MOTOR_STALL_EVENT",
		BiMotorSetSigned:     "BIMOTOR_SET_SIGNED",
		BiMotorBrake:         "BIMOTOR_BRAKE",
		BiMotorCoast:         "BIMOTOR_COAST",
		BiMotorGetStatusReq:  "BIMOTOR_GET_STATUS_REQ",
		BiMotorStatusResp:    "BIMOTOR_STATUS_RESP",
		BiMotorStallEvent:    "BIMOTOR_STALL_EVENT",
		BiMotorSeekEndstop:   "BIMOTOR_SEEK_ENDSTOP",
		BiMotorEndstopResult: "BIMOTOR_ENDSTOP_RESULT",
		HeaterSetTarget:      "HEATER_SET_TARGET",
		HeaterGetStatusReq:   "HEATER_GET_STATUS_REQ",
		HeaterStatusResp:     "HEATER_STATUS_RESP",
		SbusGetFrameReq:      "SBUS_GET_FRAME_REQ",
		SbusFrameResp:        "SBUS_FRAME_RESP",
		SbusSetBroadcastHz:   "SBUS_SET_BROADCAST_HZ",
		SbusFrameBroadcast:   "SBUS_FRAME_BROADCAST",
		JetiExGetFrameReq:    "JETIEX_GET_FRAME_REQ",
		JetiExFrameResp:      "JETIEX_FRAME_RESP",
		JetiExSetBroadcastHz: "JETIEX_SET_BROADCAST_HZ",
		JetiExFrameBroadcast: "JETIEX_FRAME_BROADCAST",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrRoleNotAttached:      "ROLE_NOT_ATTACHED",
		ErrRoleKindMismatch:     "ROLE_KIND_MISMATCH",
		ErrRoleKindNotSupported: "ROLE_KIND_NOT_SUPPORTED",
		ErrRoleConfigInvalid:    "ROLE_CONFIG_INVALID",
		ErrRoleNoSenseRequired:  "ROLE_NO_SENSE_REQUIRED",
		ErrRoleQueueFull:        "ROLE_QUEUE_FULL",
	})
}
