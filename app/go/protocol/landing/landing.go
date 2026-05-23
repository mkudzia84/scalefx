// Package landing mirrors
// controllers/hubfx/esp32s3/src/effects/landing_lights/landing_light_protocol.h —
// the HubFX landing-light wire surface.  Each landing light is a
// (servo + LED group) primitive owned exclusively by the master.
// Packet slice: 0xB1..0xB6.
package landing

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ────────────────────────────────────────────────────

const (
	ListReq    protocol.PacketType = 0xB1
	ListResp   protocol.PacketType = 0xB2
	SetState   protocol.PacketType = 0xB3
	StatusReq  protocol.PacketType = 0xB4
	StatusResp protocol.PacketType = 0xB5
	PhaseEvent protocol.PacketType = 0xB6
)

// ─── State (sent in SetState payload) ────────────────────────────────

const (
	StateOff byte = 0
	StateOn  byte = 1
)

// StateName returns "off" / "on" / "?".
func StateName(s byte) string {
	switch s {
	case StateOff:
		return "off"
	case StateOn:
		return "on"
	default:
		return fmt.Sprintf("0x%02X?", s)
	}
}

// ─── Lifecycle phase (status / async event) ──────────────────────────

const (
	PhaseUnconfigured byte = 0
	PhaseRetracted    byte = 1
	PhaseDeploying    byte = 2
	PhaseDeployed     byte = 3
	PhaseRetracting   byte = 4
)

// PhaseName returns the canonical name (matches the firmware enum).
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
	default:
		return fmt.Sprintf("0x%02X?", p)
	}
}

// ─── Error codes (0x88..0x8F per CLAUDE.md) ──────────────────────────
// Co-tenants with ExpanderError (0x80..0x87) in the infrastructure
// error range 0x80..0x8F.

const (
	ErrUnknownID        protocol.ErrorCode = 0x88
	ErrWrongOwner       protocol.ErrorCode = 0x89
	ErrServoUnavailable protocol.ErrorCode = 0x8A
	ErrTableFull        protocol.ErrorCode = 0x8B
)

// ─── Decoded types ───────────────────────────────────────────────────

// Light is one entry in LL_LIST_RESP.  `Owner` is the EffectId of the
// effect that currently owns this landing light (0 == none).
type Light struct {
	ID    byte   `json:"id"`
	Name  string `json:"name"`
	Owner byte   `json:"owner"`
	Phase byte   `json:"phase"`
}

// LightStatus is one entry in LL_STATUS_RESP.
type LightStatus struct {
	ID    byte `json:"id"`
	Phase byte `json:"phase"`
}

// PhaseChange is the decoded LL_PHASE_EVENT async payload.
type PhaseChange struct {
	ID    byte `json:"id"`
	Phase byte `json:"phase"`
}

// ─── Decoders ────────────────────────────────────────────────────────

// DecodeList parses LL_LIST_RESP:
//
//	[count:u8] per-entry: [id:u8][nameLen:u8][name:str][owner:u8][phase:u8]
func DecodeList(p []byte) ([]Light, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("ll list: empty")
	}
	count := int(p[0])
	off := 1
	out := make([]Light, 0, count)
	for i := 0; i < count; i++ {
		if off+2 > len(p) {
			return nil, fmt.Errorf("ll list[%d]: truncated header", i)
		}
		id := p[off]
		nameLen := int(p[off+1])
		off += 2
		if off+nameLen+2 > len(p) {
			return nil, fmt.Errorf("ll list[%d]: truncated entry", i)
		}
		name := string(p[off : off+nameLen])
		off += nameLen
		owner := p[off]
		phase := p[off+1]
		off += 2
		out = append(out, Light{ID: id, Name: name, Owner: owner, Phase: phase})
	}
	return out, nil
}

// DecodeStatus parses LL_STATUS_RESP:
//
//	[count:u8] per-entry: [id:u8][phase:u8]
func DecodeStatus(p []byte) ([]LightStatus, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("ll status: empty")
	}
	count := int(p[0])
	if 1+2*count > len(p) {
		return nil, fmt.Errorf("ll status: truncated (need %d)", count)
	}
	out := make([]LightStatus, count)
	for i := 0; i < count; i++ {
		off := 1 + 2*i
		out[i] = LightStatus{ID: p[off], Phase: p[off+1]}
	}
	return out, nil
}

// DecodePhaseEvent parses an LL_PHASE_EVENT async payload.
func DecodePhaseEvent(p []byte) (PhaseChange, error) {
	if len(p) < 2 {
		return PhaseChange{}, fmt.Errorf("ll phase event: need 2 bytes, got %d", len(p))
	}
	return PhaseChange{ID: p[0], Phase: p[1]}, nil
}

// ─── Command builders ────────────────────────────────────────────────

// CmdListReq builds an LL_LIST_REQ.
func CmdListReq() []byte { return protocol.BuildPacket(ListReq, nil, 0) }

// CmdSetState builds an LL_SET_STATE.
func CmdSetState(id, state byte) []byte {
	return protocol.BuildPacket(SetState, []byte{id, state}, 0)
}

// CmdStatusReq builds an LL_STATUS_REQ.
func CmdStatusReq() []byte { return protocol.BuildPacket(StatusReq, nil, 0) }

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		ListReq:    "LL_LIST_REQ",
		ListResp:   "LL_LIST_RESP",
		SetState:   "LL_SET_STATE",
		StatusReq:  "LL_STATUS_REQ",
		StatusResp: "LL_STATUS_RESP",
		PhaseEvent: "LL_PHASE_EVENT",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUnknownID:        "LL_UNKNOWN_ID",
		ErrWrongOwner:       "LL_WRONG_OWNER",
		ErrServoUnavailable: "LL_SERVO_UNAVAILABLE",
		ErrTableFull:        "LL_TABLE_FULL",
	})
}
