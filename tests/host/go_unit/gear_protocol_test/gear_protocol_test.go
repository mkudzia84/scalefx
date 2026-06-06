package gear_protocol_test

// Unit tests for the GearControl wire protocol
// (CLAUDE.md "GearControl 0xBE-0xC6 + 0xD7").
//
// GEAR_RESET sits at 0xD7 (the 2026-05-22 collision history moved it off
// 0xC7-0xC9, which belong to EngineFx START/STOP/STATUS).  GEAR_CALIBRATE /
// GEAR_CALIB_CANCEL (0xD8/0xD9) were REMOVED (instructions/29 decision #3 —
// endstop calibration now lives on the BiDcMotor role) and are FREE.
// This test pins the layout + the Rule 11 subPhase status/event extension.

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
		// Error-reset at 0xD7 (post-collision-fix); 0xD8/0xD9 freed.
		{"RESET (0xD7)", gear.Reset, 0xD7},
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

// Rule 11: GEAR_STATUS_RESP entries grew from [id][phase] (2 B) to
// [id][phase][subPhase] (3 B).  DecodeStatus must read the new trailing
// subPhase AND still decode a legacy 2-byte-entry payload (subPhase=0).
func TestGearStatusSubPhaseExtension(t *testing.T) {
	// v2: 2 entries, 3 bytes each.
	v2 := []byte{
		2,
		0, gear.PhaseDeploying, gear.SubPhaseMotorRunning,
		1, gear.PhaseDeployed, gear.SubPhaseIdle,
	}
	st, err := gear.DecodeStatus(v2)
	if err != nil {
		t.Fatalf("v2 decode: %v", err)
	}
	if len(st) != 2 {
		t.Fatalf("v2: got %d entries, want 2", len(st))
	}
	if st[0].Phase != gear.PhaseDeploying || st[0].SubPhase != gear.SubPhaseMotorRunning {
		t.Errorf("v2[0] = phase %d sub %d, want deploying/motor-running", st[0].Phase, st[0].SubPhase)
	}

	// Legacy: 2 entries, 2 bytes each (no subPhase) → subPhase defaults to idle.
	legacy := []byte{2, 0, gear.PhaseRetracted, 1, gear.PhaseDeployed}
	st, err = gear.DecodeStatus(legacy)
	if err != nil {
		t.Fatalf("legacy decode: %v", err)
	}
	if len(st) != 2 || st[0].SubPhase != gear.SubPhaseIdle {
		t.Errorf("legacy decode mismatch: %+v", st)
	}
}

// GEAR_PHASE_EVENT also gained the trailing subPhase (Rule 11).
func TestGearPhaseEventSubPhase(t *testing.T) {
	pc, err := gear.DecodePhaseEvent([]byte{3, gear.PhaseRetracting, gear.SubPhaseDoorsClosing})
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if pc.ID != 3 || pc.Phase != gear.PhaseRetracting || pc.SubPhase != gear.SubPhaseDoorsClosing {
		t.Errorf("got %+v", pc)
	}
	// Pre-v2 (2-byte) event still decodes (subPhase=0).
	pc, _ = gear.DecodePhaseEvent([]byte{3, gear.PhaseDeployed})
	if pc.SubPhase != gear.SubPhaseIdle {
		t.Errorf("legacy event subPhase = %d, want idle", pc.SubPhase)
	}
}
