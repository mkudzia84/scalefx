package gear_protocol_test

// Unit tests for the GearControl wire protocol
// (CLAUDE.md "GearControl 0xBE-0xC6 + 0xD7-0xD9").
//
// The 0xD7-0xD9 split came from the 2026-05-22 collision history
// (GEAR_RESET/CALIBRATE/CALIB_CANCEL originally at 0xC7-0xC9 ate
// EngineFx START/STOP/STATUS).  This test pins the post-fix layout.

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/gear"
)

func TestGearPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"DEPLOY", gear.Deploy, 0xBE},
		{"RETRACT", gear.Retract, 0xBF},
		{"STOP", gear.Stop, 0xC0},
		{"ALL (broadcast id)", gear.All, 0xC1},
		{"STATUS_REQ", gear.StatusReq, 0xC2},
		{"STATUS_RESP", gear.StatusResp, 0xC3},
		{"PHASE_EVENT", gear.PhaseEvent, 0xC4},
		{"LIST_REQ", gear.ListReq, 0xC5},
		{"LIST_RESP", gear.ListResp, 0xC6},
		// Calibration extension at 0xD7-0xD9 (post-collision-fix).
		{"RESET (0xD7)", gear.Reset, 0xD7},
		{"CALIBRATE (0xD8)", gear.Calibrate, 0xD8},
		{"CALIB_CANCEL (0xD9)", gear.CalibCancel, 0xD9},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X — CLAUDE.md byte-allocation drift "+
				"(check the 2026-05-22 collision history)",
				tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestGearDeployRetractStopShape(t *testing.T) {
	for _, tc := range []struct {
		name string
		raw  []byte
		want protocol.PacketType
	}{
		{"Deploy(1)", gear.CmdDeploy(1), gear.Deploy},
		{"Retract(2)", gear.CmdRetract(2), gear.Retract},
		{"Stop(0)", gear.CmdStop(0), gear.Stop},
	} {
		ptype, _, payload, ok := protocol.ParsePacket(tc.raw)
		if !ok {
			t.Fatalf("%s: ParsePacket failed", tc.name)
		}
		if ptype != tc.want {
			t.Errorf("%s: type=0x%02X want 0x%02X", tc.name, byte(ptype), byte(tc.want))
		}
		// All three carry exactly one byte (the gear id).
		if len(payload) != 1 {
			t.Errorf("%s: payload len=%d want 1", tc.name, len(payload))
		}
	}
}
