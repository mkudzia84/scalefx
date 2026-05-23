// Package lightfx mirrors
// controllers/hubfx/esp32s3/src/effects/lightfx/lightfx_protocol.h —
// the master-side LightFX wire surface (program selector + master
// brightness scalar).  Packets sit in 0xB7..0xBD.
package lightfx

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types (0xB7..0xBD) ───────────────────────────────────────

const (
	ProgramListReq      protocol.PacketType = 0xB7
	ProgramListResp     protocol.PacketType = 0xB8
	ProgramSelect       protocol.PacketType = 0xB9
	ProgramReset        protocol.PacketType = 0xBA
	StatusReq           protocol.PacketType = 0xBB
	StatusResp          protocol.PacketType = 0xBC
	MasterBrightnessSet protocol.PacketType = 0xBD
)

// NoActiveProgram is the sentinel returned in StatusResp.activeIdx
// when no program is currently active.
const NoActiveProgram byte = 0xFF

// ─── Error codes (0x50..0x5F per CLAUDE.md) ──────────────────────────

const (
	ErrUnknownProgram  protocol.ErrorCode = 0x50
	ErrProgramTooLarge protocol.ErrorCode = 0x51
	ErrBrightnessRange protocol.ErrorCode = 0x52
)

// ─── Decoded types ───────────────────────────────────────────────────

// Program is one entry in a LIGHTFX_PROGRAM_LIST_RESP.
type Program struct {
	Index byte   `json:"index"`
	Name  string `json:"name"`
}

// Status is the decoded LIGHTFX_STATUS_RESP payload.
type Status struct {
	ActiveIdx          byte `json:"activeIdx"`  // 0xFF == none
	MasterBrightnessPct byte `json:"masterBrightnessPct"`
}

// HasActiveProgram is true when an LightFX program is currently running.
func (s Status) HasActiveProgram() bool { return s.ActiveIdx != NoActiveProgram }

// ─── Decoders ────────────────────────────────────────────────────────

// DecodeProgramList parses LIGHTFX_PROGRAM_LIST_RESP:
//
//	[count:u8] then per-entry: [idx:u8][nameLen:u8][name:str]
func DecodeProgramList(p []byte) ([]Program, error) {
	if len(p) < 1 {
		return nil, fmt.Errorf("lightfx program list: empty")
	}
	count := int(p[0])
	off := 1
	out := make([]Program, 0, count)
	for i := 0; i < count; i++ {
		if off+2 > len(p) {
			return nil, fmt.Errorf("lightfx program list[%d]: truncated header", i)
		}
		idx := p[off]
		nameLen := int(p[off+1])
		off += 2
		if off+nameLen > len(p) {
			return nil, fmt.Errorf("lightfx program list[%d]: truncated name (need %d)", i, nameLen)
		}
		out = append(out, Program{
			Index: idx,
			Name:  string(p[off : off+nameLen]),
		})
		off += nameLen
	}
	return out, nil
}

// DecodeStatus parses LIGHTFX_STATUS_RESP:
//
//	[activeIdx:u8][masterBrightnessPct:u8]
func DecodeStatus(p []byte) (Status, error) {
	if len(p) < 2 {
		return Status{}, fmt.Errorf("lightfx status: need 2 bytes, got %d", len(p))
	}
	return Status{
		ActiveIdx:           p[0],
		MasterBrightnessPct: p[1],
	}, nil
}

// ─── Command builders ────────────────────────────────────────────────

// CmdProgramListReq builds a LIGHTFX_PROGRAM_LIST_REQ.
func CmdProgramListReq() []byte {
	return protocol.BuildPacket(ProgramListReq, nil, 0)
}

// CmdProgramSelect builds a LIGHTFX_PROGRAM_SELECT for the given program
// index.  The hub atomically switches to that program (or NACKs with
// UNKNOWN_PROGRAM if the index is out of range).
func CmdProgramSelect(idx byte) []byte {
	return protocol.BuildPacket(ProgramSelect, []byte{idx}, 0)
}

// CmdProgramReset builds a LIGHTFX_PROGRAM_RESET — drops the active
// program; every claimed LED goes off.
func CmdProgramReset() []byte {
	return protocol.BuildPacket(ProgramReset, nil, 0)
}

// CmdStatusReq builds a LIGHTFX_STATUS_REQ.
func CmdStatusReq() []byte {
	return protocol.BuildPacket(StatusReq, nil, 0)
}

// CmdMasterBrightness builds a LIGHTFX_MASTER_BRIGHTNESS_SET command —
// pct must be 0..100; firmware NACKs with BRIGHTNESS_RANGE otherwise.
func CmdMasterBrightness(pct byte) []byte {
	return protocol.BuildPacket(MasterBrightnessSet, []byte{pct}, 0)
}

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		ProgramListReq:      "LIGHTFX_PROGRAM_LIST_REQ",
		ProgramListResp:     "LIGHTFX_PROGRAM_LIST_RESP",
		ProgramSelect:       "LIGHTFX_PROGRAM_SELECT",
		ProgramReset:        "LIGHTFX_PROGRAM_RESET",
		StatusReq:           "LIGHTFX_STATUS_REQ",
		StatusResp:          "LIGHTFX_STATUS_RESP",
		MasterBrightnessSet: "LIGHTFX_MASTER_BRIGHTNESS_SET",
	})
	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrUnknownProgram:  "LIGHTFX_UNKNOWN_PROGRAM",
		ErrProgramTooLarge: "LIGHTFX_PROGRAM_TOO_LARGE",
		ErrBrightnessRange: "LIGHTFX_BRIGHTNESS_RANGE",
	})
}
