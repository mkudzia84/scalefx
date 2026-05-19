// Package slave mirrors the generic-slave wire format from
// controllers/lib/sfx_serial/serial/slave/slave.h.  Master-side
// (HubFX C++ + this Go SDK) and slave-side firmware reference the
// same constants — Rule 1 in CLAUDE.md ("Protocol mirror").
//
// Async events (TAG_ASYNC, slave → master/PC):
//   SERVO_TARGET_REACHED  — motion profile converged on target
//   SERVO_MOTION_UPDATE   — periodic mid-motion progress (configurable rate)
//   PWM_STALL             — stall guard tripped
//   LED_PROGRAM_DONE      — non-repeating LED program finished
//   SLAVE_STATUS_BROADCAST — periodic bundled state of every component
//
// All other packets are synchronous query / command pairs with normal
// tag correlation.
package slave

// ── Packet IDs (mirror of SlavePacket::* in slave.h) ──────────────────

const (
	// Identity / enumeration / status broadcast (0x01..0x0F)
	ComponentListReq      = 0x01
	ComponentListResp     = 0x02
	IdentGetReq           = 0x03
	IdentGetResp          = 0x04
	IdentSet              = 0x05
	SlaveStatusBroadcast  = 0x06
	SlaveStatusRate       = 0x07
	SlaveStatusReq        = 0x08

	// Servo (0x10..0x2F)
	ServoSet              = 0x10
	ServoConfig           = 0x11
	ServoQuery            = 0x12
	ServoQueryResp        = 0x13
	ServoTargetReached    = 0x14 // async
	ServoApplyJerk        = 0x15
	ServoSetMotion        = 0x16
	ServoHold             = 0x17
	ServoMotionUpdate     = 0x18 // async
	ServoMotionUpdates    = 0x19

	// PWM (0x30..0x4F)
	PwmSetMode            = 0x30
	PwmSetDuty            = 0x31
	PwmSetMotor           = 0x32
	PwmSetHeater          = 0x33
	PwmSetFreq            = 0x34
	PwmQuery              = 0x35
	PwmQueryResp          = 0x36
	PwmReconfigure        = 0x37
	PwmGetConfig          = 0x38
	PwmGetConfigResp      = 0x39
	PwmSetStallGuard      = 0x3A
	PwmClearStall         = 0x3B
	PwmStall              = 0x3C // async

	// LED (0x50..0x7F)
	LedSetBrightness      = 0x50
	LedProgramLoad        = 0x51
	LedProgramRun         = 0x52
	LedProgramStop        = 0x53
	LedQuery              = 0x54
	LedQueryResp          = 0x55
	LedProgramDone        = 0x56 // async
	LedProgramRestart     = 0x57
	LedResetChannel       = 0x58
	LedEnableChannel      = 0x59
	LedSetMasterBri       = 0x5A
	LedSeqStatusReq       = 0x5B
	LedSeqStatusResp      = 0x5C
)

// ── ComponentKind (mirror of sfx_peripherals::ComponentKind) ──────────

type ComponentKind uint8

const (
	KindNone       ComponentKind = 0x00
	KindServo      ComponentKind = 0x01
	KindPwmGeneric ComponentKind = 0x02
	KindPwmLed     ComponentKind = 0x03
	KindPwmMotor   ComponentKind = 0x04
	KindPwmHeater  ComponentKind = 0x05
	KindLedDigital ComponentKind = 0x06
)

func (k ComponentKind) String() string {
	switch k {
	case KindNone:
		return "none"
	case KindServo:
		return "servo"
	case KindPwmGeneric:
		return "pwm"
	case KindPwmLed:
		return "pwm-led"
	case KindPwmMotor:
		return "pwm-motor"
	case KindPwmHeater:
		return "pwm-heater"
	case KindLedDigital:
		return "led-digital"
	}
	return "?"
}

// ComponentInfo — wire format: [idx:u8][kind:u8][flags:u8][reserved:u8] = 4 bytes.
type ComponentInfo struct {
	Index    uint8
	Kind     ComponentKind
	Flags    uint8 // ServoFlags / PwmFlags / LedFlags depending on Kind
	Reserved uint8
}

// ── Capability flags (mirror of *Flags namespaces in component_kind.h) ─

const (
	// ServoFlags
	ServoHasMotionProfile = 1 << 0
	ServoHasPersistedCfg  = 1 << 1

	// PwmFlags (hardware capabilities — set at compile time)
	PwmModeMutable      = 1 << 0
	PwmHasVoltageSense  = 1 << 1
	PwmHasCurrentSense  = 1 << 2
	PwmIsBidirPair      = 1 << 3
	PwmHasThermistor    = 1 << 4

	// LedFlags
	LedHasHardwarePwm   = 1 << 0
	LedSupportsPrograms = 1 << 1

	// PwmConfigFlags (runtime per-channel)
	PwmCfgInvertOutput   = 0x01
	PwmCfgDcBrakeOnStop  = 0x02
	PwmCfgHeaterBangBang = 0x04

	// StallFlags
	StallEnabled    = 0x01
	StallAutoStop   = 0x02
	StallBrakeOnStop = 0x04
	StallLatch      = 0x08

	// LedProgramFlags
	LedRepeat     = 0x01
	LedSyncStart  = 0x02

	// StatusKinds (for SLAVE_STATUS_RATE / SLAVE_STATUS_REQ)
	StatusKindAll        = 0x00
	StatusKindServo      = 0x01
	StatusKindPwm        = 0x02
	StatusKindLed        = 0x04
	StatusKindHeaderOnly = 0x80
)

// ── PortId helpers (mirror of SlavePacket::PortId namespace) ──────────

type PortKind uint8

const (
	PortNone   PortKind = 0
	PortServo  PortKind = 1
	PortPwm    PortKind = 2
	PortLedDed PortKind = 3
	PortLedPwm PortKind = 4
)

func PortIdMake(k PortKind, idx uint8) uint8 { return (uint8(k) << 5) | (idx & 0x1F) }
func PortIdKind(pid uint8) PortKind            { return PortKind(pid >> 5) }
func PortIdIndex(pid uint8) uint8              { return pid & 0x1F }

// ── LED address helpers (mirror of SlavePacket::LedAddr) ──────────────

const (
	LedAddrPwmBorrowedBit = 0x80
	LedAddrIndexMask      = 0x7F
)

func LedAddrDedicated(idx uint8) uint8     { return idx & LedAddrIndexMask }
func LedAddrPwmBorrowed(idx uint8) uint8   { return LedAddrPwmBorrowedBit | (idx & LedAddrIndexMask) }
func LedAddrIsPwmBorrowed(addr uint8) bool { return (addr & LedAddrPwmBorrowedBit) != 0 }
func LedAddrIndex(addr uint8) uint8        { return addr & LedAddrIndexMask }

// ── LedEventType (mirror of SlavePacket::LedEventType) ────────────────

const (
	LedEvOn       = 0
	LedEvOff      = 1
	LedEvFlashing = 2
	LedEvFadeIn   = 3
	LedEvFadeOut  = 4
	LedEvFading   = 5
	LedEvBeacon   = 6
)

// LedEvent — wire format (8 bytes).  Field meaning depends on Type;
// see LedEventType doc-comment in slave.h for per-type p1..p5 layout.
type LedEvent struct {
	Type uint8
	P1   uint16
	P2   uint16
	P3   uint8
	P4   uint8
	P5   uint8
}

// ── Error codes (mirror of SlaveError::*) ─────────────────────────────

const (
	ErrInvalidIndex          = 0xA0
	ErrWrongComponentKind    = 0xA1
	ErrModeNotSupported      = 0xA2
	ErrSensingUnavailable    = 0xA3
	ErrIdentTooLong          = 0xA4
	ErrIdentPersistFailed    = 0xA5
	ErrIdentInvalidChars     = 0xA6
	ErrNotInitialised        = 0xA7
	ErrProgramTooLarge       = 0xA8
	ErrInvalidProgramId      = 0xA9
)

// ErrorMessage maps a slave error code to a human-readable string.
func ErrorMessage(code uint8) string {
	switch code {
	case ErrInvalidIndex:
		return "invalid component index"
	case ErrWrongComponentKind:
		return "wrong component kind for command"
	case ErrModeNotSupported:
		return "mode not supported by channel"
	case ErrSensingUnavailable:
		return "sensing not available on channel"
	case ErrIdentTooLong:
		return "identifier too long (max 32 bytes)"
	case ErrIdentPersistFailed:
		return "identifier persistence failed"
	case ErrIdentInvalidChars:
		return "identifier has invalid characters"
	case ErrNotInitialised:
		return "slave not initialised — send INIT first"
	case ErrProgramTooLarge:
		return "LED program exceeds slot capacity"
	case ErrInvalidProgramId:
		return "invalid LED program id"
	}
	return ""
}
