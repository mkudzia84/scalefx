package landing_protocol_test

// Unit tests for the Landing-Light wire protocol
// (CLAUDE.md "LandingLight 0xB1-0xB6").

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/landing"
)

func TestLandingPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"LIST_REQ", landing.ListReq, 0xB1},
		{"LIST_RESP", landing.ListResp, 0xB2},
		{"SET_STATE", landing.SetState, 0xB3},
		{"STATUS_REQ", landing.StatusReq, 0xB4},
		{"STATUS_RESP", landing.StatusResp, 0xB5},
		{"PHASE_EVENT", landing.PhaseEvent, 0xB6},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

// Landing-light SET_STATE is the high-value command — pins down the
// 2-byte payload format the firmware expects.
func TestCmdSetStatePayload(t *testing.T) {
	raw := landing.CmdSetState(3, 1)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != landing.SetState {
		t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(landing.SetState))
	}
	// Wire: [lightId:u8][state:u8]
	if len(payload) != 2 || payload[0] != 3 || payload[1] != 1 {
		t.Errorf("SetState(3,1) payload = %v, want [3 1]", payload)
	}
}
