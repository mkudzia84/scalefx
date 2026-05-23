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
	ServoSetProfile    protocol.PacketType = 0x4D // Phase 2.9.x: live motion-profile retune
	ServoGetProfileReq protocol.PacketType = 0x4E
	ServoProfileResp   protocol.PacketType = 0x4F

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
	MotorSetDuty       protocol.PacketType = 0x60
	MotorBrake         protocol.PacketType = 0x61
	MotorGetStatusReq  protocol.PacketType = 0x62
	MotorStatusResp    protocol.PacketType = 0x63
	MotorStallEvent    protocol.PacketType = 0x64
	MotorSetElement    protocol.PacketType = 0x65 // Phase 2.9.x: live element-scaling retune
	MotorGetElementReq protocol.PacketType = 0x66
	MotorElementResp   protocol.PacketType = 0x67

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
	HeaterSetTarget     protocol.PacketType = 0x70
	HeaterGetStatusReq  protocol.PacketType = 0x71
	HeaterStatusResp    protocol.PacketType = 0x72
	HeaterSetElement    protocol.PacketType = 0x73 // Phase 2.9.x: live element + drivePct + hyst retune
	HeaterGetElementReq protocol.PacketType = 0x74
	HeaterElementResp   protocol.PacketType = 0x75

	// DC motor intent-layer driver (continuation slot — the primary
	// 0x60..0x67 range is exhausted, so MOTOR_SET_PCT lives in the
	// spare slot at the top of the heater range).  Rule 42 intent
	// surface — role applies scaleDuty() internally.
	MotorSetPct protocol.PacketType = 0x76 // [portIdx:u8][pct:u8] → ACK

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

// ServoMotionProfile mirrors the firmware-side `sfx_core::ServoMotionProfile`
// — the per-port motion shape (clamp + speed + accel + jerk + REV).
// Live-tunable via SERVO_SET_PROFILE / SERVO_GET_PROFILE_REQ.
type ServoMotionProfile struct {
	MinUs             uint16 `json:"minUs"`
	MaxUs             uint16 `json:"maxUs"`
	MaxSpeedUsPerSec  uint16 `json:"maxSpeedUsPerSec"`
	Reversed          bool   `json:"reversed"`
	CenterUs          uint16 `json:"centerUs"`
	MaxAccelUsPerSec2 uint16 `json:"maxAccelUsPerSec2"`
	MaxJerkUsPerSec3  uint16 `json:"maxJerkUsPerSec3"`
}

const servoProfileBodyBytes = 13 // not counting the leading portIdx byte

func encodeServoProfileBody(p ServoMotionProfile) []byte {
	b := make([]byte, servoProfileBodyBytes)
	binary.LittleEndian.PutUint16(b[0:2],  p.MinUs)
	binary.LittleEndian.PutUint16(b[2:4],  p.MaxUs)
	binary.LittleEndian.PutUint16(b[4:6],  p.MaxSpeedUsPerSec)
	if p.Reversed {
		b[6] = 1
	}
	binary.LittleEndian.PutUint16(b[7:9],   p.CenterUs)
	binary.LittleEndian.PutUint16(b[9:11],  p.MaxAccelUsPerSec2)
	binary.LittleEndian.PutUint16(b[11:13], p.MaxJerkUsPerSec3)
	return b
}

func CmdServoSetProfile(portIdx byte, p ServoMotionProfile) []byte {
	body := encodeServoProfileBody(p)
	return protocol.BuildPacket(ServoSetProfile, append([]byte{portIdx}, body...), 0)
}

func CmdServoGetProfile(portIdx byte) []byte {
	return protocol.BuildPacket(ServoGetProfileReq, []byte{portIdx}, 0)
}

// DecodeServoProfile decodes SERVO_PROFILE_RESP: [portIdx][14 B profile body].
func DecodeServoProfile(payload []byte) (portIdx byte, p ServoMotionProfile, err error) {
	if len(payload) < 1+servoProfileBodyBytes {
		err = fmt.Errorf("servo profile: truncated (%d B)", len(payload))
		return
	}
	portIdx = payload[0]
	b := payload[1:]
	p.MinUs             = binary.LittleEndian.Uint16(b[0:2])
	p.MaxUs             = binary.LittleEndian.Uint16(b[2:4])
	p.MaxSpeedUsPerSec  = binary.LittleEndian.Uint16(b[4:6])
	p.Reversed          = b[6] != 0
	p.CenterUs          = binary.LittleEndian.Uint16(b[7:9])
	p.MaxAccelUsPerSec2 = binary.LittleEndian.Uint16(b[9:11])
	p.MaxJerkUsPerSec3  = binary.LittleEndian.Uint16(b[11:13])
	return
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

// ElementScalingMode mirrors `sfx_core::ElementScalingMode` (Phase 2 of
// the GunFX rollout — Rule 42). Used on both DC-motor and heater
// elements that need voltage scaling.
type ElementScalingMode byte

const (
	ScalingPassthrough ElementScalingMode = 0 // duty = requestedPct (no scaling)
	ScalingLinear      ElementScalingMode = 1 // duty = (Ve/Vp)  * requestedPct
	ScalingQuadratic   ElementScalingMode = 2 // duty = (Ve/Vp)² * requestedPct
)

// MotorElementConfig is what MOTOR_SET_ELEMENT writes and
// MOTOR_GET_ELEMENT reads back. PortRailMv is read-only (port-side).
type MotorElementConfig struct {
	ElementMv  uint16             `json:"elementMv"`
	Scaling    ElementScalingMode `json:"scaling"`
	PortRailMv uint16             `json:"portRailMv"` // ignored on SET; populated on GET
}

func CmdMotorSetElement(portIdx byte, c MotorElementConfig) []byte {
	body := make([]byte, 3)
	binary.LittleEndian.PutUint16(body[0:2], c.ElementMv)
	body[2] = byte(c.Scaling)
	return protocol.BuildPacket(MotorSetElement, append([]byte{portIdx}, body...), 0)
}

func CmdMotorGetElement(portIdx byte) []byte {
	return protocol.BuildPacket(MotorGetElementReq, []byte{portIdx}, 0)
}

// CmdMotorSetPct — intent-layer DC motor drive (Phase 2.9.x).  Drive
// at `pct` % of the element's rated voltage; the role applies
// scaleDuty() internally.  Used by GunFx smoke-fan puffing and any
// future intent-layer caller.
func CmdMotorSetPct(portIdx, pct byte) []byte {
	if pct > 100 {
		pct = 100
	}
	return protocol.BuildPacket(MotorSetPct, []byte{portIdx, pct}, 0)
}

// DecodeMotorElement decodes MOTOR_ELEMENT_RESP:
//   [portIdx][elementMv:u16][scaling:u8][portRailMv:u16]
func DecodeMotorElement(payload []byte) (portIdx byte, c MotorElementConfig, err error) {
	if len(payload) < 6 {
		err = fmt.Errorf("motor element: truncated (%d B)", len(payload))
		return
	}
	portIdx = payload[0]
	c.ElementMv  = binary.LittleEndian.Uint16(payload[1:3])
	c.Scaling    = ElementScalingMode(payload[3])
	c.PortRailMv = binary.LittleEndian.Uint16(payload[4:6])
	return
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

// HeaterElementConfig is what HEATER_SET_ELEMENT writes and
// HEATER_GET_ELEMENT reads back. PortRailMv is read-only.
type HeaterElementConfig struct {
	ElementMv  uint16             `json:"elementMv"`
	Scaling    ElementScalingMode `json:"scaling"`
	DrivePct   uint8              `json:"drivePct"`
	HystCx10   int16              `json:"hystCx10"`
	PortRailMv uint16             `json:"portRailMv"` // ignored on SET; populated on GET
}

func CmdHeaterSetElement(portIdx byte, c HeaterElementConfig) []byte {
	body := make([]byte, 6)
	binary.LittleEndian.PutUint16(body[0:2], c.ElementMv)
	body[2] = byte(c.Scaling)
	body[3] = c.DrivePct
	binary.LittleEndian.PutUint16(body[4:6], uint16(c.HystCx10))
	return protocol.BuildPacket(HeaterSetElement, append([]byte{portIdx}, body...), 0)
}

func CmdHeaterGetElement(portIdx byte) []byte {
	return protocol.BuildPacket(HeaterGetElementReq, []byte{portIdx}, 0)
}

// DecodeHeaterElement decodes HEATER_ELEMENT_RESP:
//   [portIdx][elementMv:u16][scaling:u8][drivePct:u8][hyst_cx10:i16][portRailMv:u16]
func DecodeHeaterElement(payload []byte) (portIdx byte, c HeaterElementConfig, err error) {
	if len(payload) < 9 {
		err = fmt.Errorf("heater element: truncated (%d B)", len(payload))
		return
	}
	portIdx = payload[0]
	c.ElementMv  = binary.LittleEndian.Uint16(payload[1:3])
	c.Scaling    = ElementScalingMode(payload[3])
	c.DrivePct   = payload[4]
	c.HystCx10   = int16(binary.LittleEndian.Uint16(payload[5:7]))
	c.PortRailMv = binary.LittleEndian.Uint16(payload[7:9])
	return
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
		ServoSetProfile:      "SERVO_SET_PROFILE",
		ServoGetProfileReq:   "SERVO_GET_PROFILE_REQ",
		ServoProfileResp:     "SERVO_PROFILE_RESP",
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
		MotorSetElement:      "MOTOR_SET_ELEMENT",
		MotorGetElementReq:   "MOTOR_GET_ELEMENT_REQ",
		MotorElementResp:     "MOTOR_ELEMENT_RESP",
		MotorSetPct:          "MOTOR_SET_PCT",
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
		HeaterSetElement:     "HEATER_SET_ELEMENT",
		HeaterGetElementReq:  "HEATER_GET_ELEMENT_REQ",
		HeaterElementResp:    "HEATER_ELEMENT_RESP",
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
