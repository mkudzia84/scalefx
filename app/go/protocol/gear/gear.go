// Package gear mirrors
// controllers/hubfx/esp32s3/src/effects/gearcontrol/gearcontrol_protocol.h —
// the GearControl wire surface.  A gear is one retractable landing-gear
// unit: H-bridge motor + 0..N status LEDs.  Packet slice: 0xBE..0xC6.
package gear

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ────────────────────────────────────────────────────

const (
	Deploy      protocol.PacketType = 0xBE
	Retract     protocol.PacketType = 0xBF
	Stop        protocol.PacketType = 0xC0
	All         protocol.PacketType = 0xC1
	StatusReq   protocol.PacketType = 0xC2
	StatusResp  protocol.PacketType = 0xC3
	PhaseEvent  protocol.PacketType = 0xC4
	ListReq     protocol.PacketType = 0xC5
	ListResp    protocol.PacketType = 0xC6
	// 0xC7..0xC9 belong to EngineFX (Start/Stop/StatusReq); reset lives at
	// 0xD7 to avoid the collision.
	Reset protocol.PacketType = 0xD7
	// Emergency stop (hold/freeze in place): []=whole set, [id]=one strut.
	// Distinct from STOP/ALL-stop — a HELD strut resumes on the next
	// deploy/retract.  Was GEAR_CALIBRATE (removed).
	Estop protocol.PacketType = 0xD8
	// Single-step [id][target] — advance one leg toward target (0=up,1=down)
	// then park.  The manual "do one item in sequence" command.  Was
	// GEAR_CALIB_CANCEL (removed).
	Step protocol.PacketType = 0xD9
)

// ─── GEAR_STEP target codes (mirror Gear::Target) ────────────────────

const (
	StepUp   byte = 0
	StepDown byte = 1
)

// ─── GEAR_ALL action codes ───────────────────────────────────────────

const (
	AllStop    byte = 0
	AllDeploy  byte = 1
	AllRetract byte = 2
)

func AllActionName(a byte) string {
	switch a {
	case AllStop:
		return "stop"
	case AllDeploy:
		return "deploy"
	case AllRetract:
		return "retract"
	default:
		return fmt.Sprintf("0x%02X?", a)
	}
}

// ─── Lifecycle phase ─────────────────────────────────────────────────

// Target-driven FSM: Up/Down are the stable states; MovingUp/MovingDown are in
// transit; Unknown = boot (position uncertain) and Held = emergency-stopped.
// Wire byte values 1..5 are unchanged from the old Retracted/Deploying/
// Deployed/Retracting/Error; 6/7 are Rule-11 appends.
const (
	PhaseUnconfigured byte = 0
	PhaseUp           byte = 1
	PhaseMovingDown   byte = 2
	PhaseDown         byte = 3
	PhaseMovingUp     byte = 4
	PhaseError        byte = 5
	PhaseUnknown      byte = 6
	PhaseHeld         byte = 7
	// Back-compat aliases (same values).
	PhaseRetracted  = PhaseUp
	PhaseDeploying  = PhaseMovingDown
	PhaseDeployed   = PhaseDown
	PhaseRetracting = PhaseMovingUp
)

func PhaseName(p byte) string {
	switch p {
	case PhaseUnconfigured:
		return "unconfigured"
	case PhaseUp:
		return "gear up"
	case PhaseMovingDown:
		return "lowering"
	case PhaseDown:
		return "gear down"
	case PhaseMovingUp:
		return "raising"
	case PhaseError:
		return "ERROR"
	case PhaseUnknown:
		return "unknown"
	case PhaseHeld:
		return "held"
	default:
		return fmt.Sprintf("0x%02X?", p)
	}
}

// PhaseSummary collapses the phase into the host's high-level status
// view: idle (settled), moving, error, held, or unknown.
func PhaseSummary(p byte) string {
	switch p {
	case PhaseUp, PhaseDown:
		return "idle"
	case PhaseMovingUp, PhaseMovingDown:
		return "moving"
	case PhaseError:
		return "error"
	case PhaseHeld:
		return "held"
	default:
		return "unknown"
	}
}

// ─── Sub-phase ───────────────────────────────────────────────────────
// The door-bracket op-queue position inside a deploy/retract cycle
// (instructions/29 decision #5).  Trailing byte of GEAR_STATUS_RESP +
// GEAR_PHASE_EVENT (Rule 11 append — old clients read the first 2 bytes).

const (
	SubPhaseIdle         byte = 0
	SubPhaseDoorsOpening byte = 1
	SubPhaseDoorsOpen    byte = 2
	SubPhaseStrutMoving  byte = 3
	SubPhaseStrutDone    byte = 4
	SubPhaseDoorsClosing byte = 5
	SubPhaseDoorsClosed  byte = 6
	// Back-compat aliases (same values).
	SubPhaseMotorRunning = SubPhaseStrutMoving
	SubPhaseMotorDone    = SubPhaseStrutDone
)

func SubPhaseName(s byte) string {
	switch s {
	case SubPhaseIdle:
		return "idle"
	case SubPhaseDoorsOpening:
		return "doors-opening"
	case SubPhaseDoorsOpen:
		return "doors-open"
	case SubPhaseStrutMoving:
		return "strut-moving"
	case SubPhaseStrutDone:
		return "strut-done"
	case SubPhaseDoorsClosing:
		return "doors-closing"
	case SubPhaseDoorsClosed:
		return "doors-closed"
	default:
		return fmt.Sprintf("0x%02X?", s)
	}
}

// ─── Error codes ─────────────────────────────────────────────────────
// CLAUDE.md allocates GearControl 0x60..0x6F.  Old values 0xC1..0xC6
// collided with EngineError::ENGINE_NOT_AVAILABLE (0xC6) on the wire
// — fixed atomically with the firmware in gearcontrol_protocol.h.

const (
	ErrUnknownID        protocol.ErrorCode = 0x60
	ErrTableFull        protocol.ErrorCode = 0x61
	ErrMotorUnavailable protocol.ErrorCode = 0x62
	ErrInErrorState     protocol.ErrorCode = 0x63
	ErrTimeout          protocol.ErrorCode = 0x64
	// 0x65 was ErrNoStallDetected (calibration) — REMOVED with calibration.
	ErrNoMotor protocol.ErrorCode = 0x66 // drove the strut but |I|≈0 → no motor / open circuit
)

// ErrorReasonTag is the short tag the Studio/CLI shows behind an Error
// phase (mirrors GearError::getTag).  `reason` is the GEAR_STATUS_RESP /
// GEAR_PHASE_EVENT trailing byte; 0 = unspecified fault.
func ErrorReasonTag(reason byte) string {
	switch protocol.ErrorCode(reason) {
	case ErrTimeout:
		return "timeout"
	case ErrNoMotor:
		return "no motor"
	case 0:
		return ""
	default:
		return "fault"
	}
}

// ─── Decoded types ───────────────────────────────────────────────────

// Gear is one entry in GEAR_LIST_RESP.
type Gear struct {
	ID   byte   `json:"id"`
	Name string `json:"name"`
}

// GearStatus is one entry in GEAR_STATUS_RESP.  SubPhase + ErrReason are
// Rule 11 trailing bytes (0 for pre-v2 peers that omit them).  ErrReason is
// the GearError code behind an Error phase.
type GearStatus struct {
	ID        byte `json:"id"`
	Phase     byte `json:"phase"`
	SubPhase  byte `json:"subPhase"`
	ErrReason byte `json:"errReason"`
}

// PhaseChange is the decoded GEAR_PHASE_EVENT async payload.  SubPhase +
// ErrReason are Rule 11 trailing bytes (0 when a pre-v2 peer omits them).
type PhaseChange struct {
	ID        byte `json:"id"`
	Phase     byte `json:"phase"`
	SubPhase  byte `json:"subPhase"`
	ErrReason byte `json:"errReason"`
}

// ─── Decoders ────────────────────────────────────────────────────────

// DecodeList parses GEAR_LIST_RESP:
//
//	[count:u8] per-entry: [id:u8][nameLen:u8][name:str]
func DecodeList(p []byte) ([]Gear, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("gear list: empty")
	}
	count := int(p[0])
	off := 1
	out := make([]Gear, 0, count)
	for i := 0; i < count; i++ {
		if off+2 > len(p) {
			return nil, fmt.Errorf("gear list[%d]: truncated header", i)
		}
		id := p[off]
		nameLen := int(p[off+1])
		off += 2
		if off+nameLen > len(p) {
			return nil, fmt.Errorf("gear list[%d]: truncated name", i)
		}
		out = append(out, Gear{ID: id, Name: string(p[off : off+nameLen])})
		off += nameLen
	}
	return out, nil
}

// DecodeStatus parses GEAR_STATUS_RESP:
//
//	[count:u8] per-entry: [id:u8][phase:u8][subPhase:u8][errReason:u8]
//
// Rule 11: entries grew 2 → 3 (subPhase) → 4 (errReason).  The stride is
// derived from the payload so any of the three vintages decodes (the missing
// trailing bytes default to 0).
func DecodeStatus(p []byte) ([]GearStatus, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("gear status: empty")
	}
	count := int(p[0])
	if count == 0 {
		return nil, nil
	}
	body := len(p) - 1
	stride := 2
	if body >= 4*count {
		stride = 4 // v3 — trailing subPhase + errReason
	} else if body >= 3*count {
		stride = 3 // v2 — trailing subPhase only
	}
	if body < stride*count {
		return nil, fmt.Errorf("gear status: truncated (need %d)", count)
	}
	out := make([]GearStatus, count)
	for i := 0; i < count; i++ {
		off := 1 + stride*i
		gs := GearStatus{ID: p[off], Phase: p[off+1]}
		if stride >= 3 {
			gs.SubPhase = p[off+2]
		}
		if stride >= 4 {
			gs.ErrReason = p[off+3]
		}
		out[i] = gs
	}
	return out, nil
}

// DecodePhaseEvent parses a GEAR_PHASE_EVENT async payload:
//
//	[id:u8][phase:u8][subPhase:u8][errReason:u8]
//
// Rule 11: the trailing subPhase + errReason are optional (0 when a peer
// omits them).
func DecodePhaseEvent(p []byte) (PhaseChange, error) {
	if len(p) < 2 {
		return PhaseChange{}, fmt.Errorf("gear phase event: need 2 bytes, got %d", len(p))
	}
	pc := PhaseChange{ID: p[0], Phase: p[1]}
	if len(p) >= 3 {
		pc.SubPhase = p[2]
	}
	if len(p) >= 4 {
		pc.ErrReason = p[3]
	}
	return pc, nil
}

// ─── Command builders ────────────────────────────────────────────────

func CmdDeploy(id byte) []byte      { return protocol.BuildPacket(Deploy, []byte{id}, 0) }
func CmdRetract(id byte) []byte     { return protocol.BuildPacket(Retract, []byte{id}, 0) }
func CmdStop(id byte) []byte        { return protocol.BuildPacket(Stop, []byte{id}, 0) }
func CmdAll(action byte) []byte     { return protocol.BuildPacket(All, []byte{action}, 0) }
func CmdStatusReq() []byte          { return protocol.BuildPacket(StatusReq, nil, 0) }
func CmdListReq() []byte            { return protocol.BuildPacket(ListReq, nil, 0) }
func CmdReset(id byte) []byte       { return protocol.BuildPacket(Reset, []byte{id}, 0) }
func CmdEstop(id byte) []byte       { return protocol.BuildPacket(Estop, []byte{id}, 0) }
func CmdEstopAll() []byte           { return protocol.BuildPacket(Estop, nil, 0) }
func CmdStep(id, target byte) []byte { return protocol.BuildPacket(Step, []byte{id, target}, 0) }

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		Deploy:      "GEAR_DEPLOY",
		Retract:     "GEAR_RETRACT",
		Stop:        "GEAR_STOP",
		All:         "GEAR_ALL",
		StatusReq:   "GEAR_STATUS_REQ",
		StatusResp:  "GEAR_STATUS_RESP",
		PhaseEvent:  "GEAR_PHASE_EVENT",
		ListReq:    "GEAR_LIST_REQ",
		ListResp:   "GEAR_LIST_RESP",
		Reset:      "GEAR_RESET",
		Estop:      "GEAR_ESTOP",
		Step:       "GEAR_STEP",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUnknownID:        "GEAR_UNKNOWN_ID",
		ErrTableFull:        "GEAR_TABLE_FULL",
		ErrMotorUnavailable: "GEAR_MOTOR_UNAVAILABLE",
		ErrInErrorState:     "GEAR_IN_ERROR_STATE",
		ErrTimeout:          "GEAR_TIMEOUT",
		ErrNoMotor:          "GEAR_NO_MOTOR",
	})
}
