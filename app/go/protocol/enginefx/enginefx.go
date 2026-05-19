// Package enginefx mirrors
// controllers/hubfx/esp32s3/src/effects/enginefx/enginefx_protocol.h —
// the master-side EngineFX wire surface (4-state engine sound state
// machine driving the local mixer).  Packet slice: 0xC7..0xCB.
package enginefx

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ────────────────────────────────────────────────────

const (
	Start      protocol.PacketType = 0xC7
	Stop       protocol.PacketType = 0xC8
	StatusReq  protocol.PacketType = 0xC9
	StatusResp protocol.PacketType = 0xCA
	StateEvent protocol.PacketType = 0xCB
)

// ─── Engine state machine ────────────────────────────────────────────

const (
	StateStopped  byte = 0
	StateStarting byte = 1
	StateRunning  byte = 2
	StateStopping byte = 3
)

// StateName returns the canonical state label.
func StateName(s byte) string {
	switch s {
	case StateStopped:
		return "stopped"
	case StateStarting:
		return "starting"
	case StateRunning:
		return "running"
	case StateStopping:
		return "stopping"
	default:
		return fmt.Sprintf("0x%02X?", s)
	}
}

// ─── Error codes ─────────────────────────────────────────────────────

const (
	ErrEngineNotAvailable protocol.ErrorCode = 0xC6
	ErrMissingPath        protocol.ErrorCode = 0xC7
)

// ─── Decoded types ───────────────────────────────────────────────────

// Status is the decoded ENGINE_STATUS_RESP payload.
type Status struct {
	State         byte `json:"state"`
	ToggleEngaged bool `json:"toggleEngaged"`
	Active        bool `json:"active"`
}

// StateChange is the decoded ENGINE_STATE_EVENT async payload.
type StateChange struct {
	State byte `json:"state"`
}

// ─── Decoders ────────────────────────────────────────────────────────

// DecodeStatus parses ENGINE_STATUS_RESP:
//
//	[state:u8][toggleEngaged:u8][active:u8]
func DecodeStatus(p []byte) (Status, error) {
	if len(p) < 3 {
		return Status{}, fmt.Errorf("engine status: need 3 bytes, got %d", len(p))
	}
	return Status{
		State:         p[0],
		ToggleEngaged: p[1] != 0,
		Active:        p[2] != 0,
	}, nil
}

// DecodeStateEvent parses an ENGINE_STATE_EVENT async payload.
func DecodeStateEvent(p []byte) (StateChange, error) {
	if len(p) < 1 {
		return StateChange{}, fmt.Errorf("engine state event: empty")
	}
	return StateChange{State: p[0]}, nil
}

// ─── Command builders ────────────────────────────────────────────────

func CmdStart() []byte     { return protocol.BuildPacket(Start, nil, 0) }
func CmdStop() []byte      { return protocol.BuildPacket(Stop, nil, 0) }
func CmdStatusReq() []byte { return protocol.BuildPacket(StatusReq, nil, 0) }

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		Start:      "ENGINE_START",
		Stop:       "ENGINE_STOP",
		StatusReq:  "ENGINE_STATUS_REQ",
		StatusResp: "ENGINE_STATUS_RESP",
		StateEvent: "ENGINE_STATE_EVENT",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrEngineNotAvailable: "ENGINE_NOT_AVAILABLE",
		ErrMissingPath:        "ENGINE_MISSING_PATH",
	})
}
