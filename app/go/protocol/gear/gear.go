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
	Reset       protocol.PacketType = 0xC7
	Calibrate   protocol.PacketType = 0xC8
	CalibCancel protocol.PacketType = 0xC9
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

const (
	PhaseUnconfigured byte = 0
	PhaseRetracted    byte = 1
	PhaseDeploying    byte = 2
	PhaseDeployed     byte = 3
	PhaseRetracting   byte = 4
	PhaseError        byte = 5
	PhaseCalibrating  byte = 6
)

func PhaseName(p byte) string {
	switch p {
	case PhaseUnconfigured:
		return "unconfigured"
	case PhaseRetracted:
		return "retracted"
	case PhaseDeploying:
		return "deploying"
	case PhaseDeployed:
		return "deployed"
	case PhaseRetracting:
		return "retracting"
	case PhaseError:
		return "ERROR"
	case PhaseCalibrating:
		return "calibrating"
	default:
		return fmt.Sprintf("0x%02X?", p)
	}
}

// PhaseSummary collapses the phase into the host's high-level status
// view: idle (settled), moving, calibrating, or error.
func PhaseSummary(p byte) string {
	switch p {
	case PhaseRetracted, PhaseDeployed:
		return "idle"
	case PhaseDeploying, PhaseRetracting:
		return "moving"
	case PhaseCalibrating:
		return "calibrating"
	case PhaseError:
		return "error"
	default:
		return "unknown"
	}
}

// ─── Error codes ─────────────────────────────────────────────────────

const (
	ErrUnknownID        protocol.ErrorCode = 0xC1
	ErrTableFull        protocol.ErrorCode = 0xC2
	ErrMotorUnavailable protocol.ErrorCode = 0xC3
	ErrInErrorState     protocol.ErrorCode = 0xC4
	ErrTimeout          protocol.ErrorCode = 0xC5
	ErrNoStallDetected  protocol.ErrorCode = 0xC6
)

// ─── Decoded types ───────────────────────────────────────────────────

// Gear is one entry in GEAR_LIST_RESP.
type Gear struct {
	ID   byte   `json:"id"`
	Name string `json:"name"`
}

// GearStatus is one entry in GEAR_STATUS_RESP.
type GearStatus struct {
	ID    byte `json:"id"`
	Phase byte `json:"phase"`
}

// PhaseChange is the decoded GEAR_PHASE_EVENT async payload.
type PhaseChange struct {
	ID    byte `json:"id"`
	Phase byte `json:"phase"`
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
//	[count:u8] per-entry: [id:u8][phase:u8]
func DecodeStatus(p []byte) ([]GearStatus, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("gear status: empty")
	}
	count := int(p[0])
	if 1+2*count > len(p) {
		return nil, fmt.Errorf("gear status: truncated (need %d)", count)
	}
	out := make([]GearStatus, count)
	for i := 0; i < count; i++ {
		off := 1 + 2*i
		out[i] = GearStatus{ID: p[off], Phase: p[off+1]}
	}
	return out, nil
}

// DecodePhaseEvent parses a GEAR_PHASE_EVENT async payload.
func DecodePhaseEvent(p []byte) (PhaseChange, error) {
	if len(p) < 2 {
		return PhaseChange{}, fmt.Errorf("gear phase event: need 2 bytes, got %d", len(p))
	}
	return PhaseChange{ID: p[0], Phase: p[1]}, nil
}

// ─── Command builders ────────────────────────────────────────────────

func CmdDeploy(id byte) []byte      { return protocol.BuildPacket(Deploy, []byte{id}, 0) }
func CmdRetract(id byte) []byte     { return protocol.BuildPacket(Retract, []byte{id}, 0) }
func CmdStop(id byte) []byte        { return protocol.BuildPacket(Stop, []byte{id}, 0) }
func CmdAll(action byte) []byte     { return protocol.BuildPacket(All, []byte{action}, 0) }
func CmdStatusReq() []byte          { return protocol.BuildPacket(StatusReq, nil, 0) }
func CmdListReq() []byte            { return protocol.BuildPacket(ListReq, nil, 0) }
func CmdReset(id byte) []byte       { return protocol.BuildPacket(Reset, []byte{id}, 0) }
func CmdCalibrate(id byte) []byte   { return protocol.BuildPacket(Calibrate, []byte{id}, 0) }
func CmdCalibCancel(id byte) []byte { return protocol.BuildPacket(CalibCancel, []byte{id}, 0) }

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
		ListReq:     "GEAR_LIST_REQ",
		ListResp:    "GEAR_LIST_RESP",
		Reset:       "GEAR_RESET",
		Calibrate:   "GEAR_CALIBRATE",
		CalibCancel: "GEAR_CALIB_CANCEL",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUnknownID:        "GEAR_UNKNOWN_ID",
		ErrTableFull:        "GEAR_TABLE_FULL",
		ErrMotorUnavailable: "GEAR_MOTOR_UNAVAILABLE",
		ErrInErrorState:     "GEAR_IN_ERROR_STATE",
		ErrTimeout:          "GEAR_TIMEOUT",
		ErrNoStallDetected:  "GEAR_NO_STALL_DETECTED",
	})
}
