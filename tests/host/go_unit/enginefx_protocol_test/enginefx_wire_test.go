package enginefx_protocol_test

// Unit tests for the engine-effect wire protocol.
// Pins the packet-type byte assignments per CLAUDE.md ("EngineFX 0xC7-0xCB")
// and the Start/Stop/StatusReq command builders.

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/enginefx"
)

func TestEngineFxPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"START", enginefx.Start, 0xC7},
		{"STOP", enginefx.Stop, 0xC8},
		{"STATUS_REQ", enginefx.StatusReq, 0xC9},
		{"STATUS_RESP", enginefx.StatusResp, 0xCA},
		{"STATE_EVENT", enginefx.StateEvent, 0xCB},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X — CLAUDE.md 'Effects: EngineFX 0xC7-0xCB' drift",
				tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestEngineFxCommandPayloadShape(t *testing.T) {
	for _, tc := range []struct {
		name string
		raw  []byte
		want protocol.PacketType
	}{
		{"Start", enginefx.CmdStart(), enginefx.Start},
		{"Stop", enginefx.CmdStop(), enginefx.Stop},
		{"StatusReq", enginefx.CmdStatusReq(), enginefx.StatusReq},
	} {
		ptype, _, payload, ok := protocol.ParsePacket(tc.raw)
		if !ok {
			t.Errorf("%s: ParsePacket failed", tc.name)
			continue
		}
		if ptype != tc.want {
			t.Errorf("%s: type = 0x%02X, want 0x%02X", tc.name, byte(ptype), byte(tc.want))
		}
		if len(payload) != 0 {
			t.Errorf("%s: payload length = %d, want 0 (zero-byte commands)", tc.name, len(payload))
		}
	}
}
