package lightfx_protocol_test

// Unit tests for the LightFX wire protocol (CLAUDE.md "LightFX 0xB7-0xBD").

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/lightfx"
)

func TestLightFxPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"PROGRAM_LIST_REQ", lightfx.ProgramListReq, 0xB7},
		{"PROGRAM_LIST_RESP", lightfx.ProgramListResp, 0xB8},
		{"PROGRAM_SELECT", lightfx.ProgramSelect, 0xB9},
		{"PROGRAM_RESET", lightfx.ProgramReset, 0xBA},
		{"STATUS_REQ", lightfx.StatusReq, 0xBB},
		{"STATUS_RESP", lightfx.StatusResp, 0xBC},
		{"MASTER_BRIGHTNESS_SET", lightfx.MasterBrightnessSet, 0xBD},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestCmdProgramSelectCarriesIndex(t *testing.T) {
	for _, idx := range []byte{0, 1, 5, 255} {
		raw := lightfx.CmdProgramSelect(idx)
		ptype, _, payload, ok := protocol.ParsePacket(raw)
		if !ok {
			t.Fatalf("ProgramSelect(%d): ParsePacket failed", idx)
		}
		if ptype != lightfx.ProgramSelect {
			t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(lightfx.ProgramSelect))
		}
		if len(payload) != 1 || payload[0] != idx {
			t.Errorf("ProgramSelect(%d) payload = %v, want [%d]", idx, payload, idx)
		}
	}
}

func TestCmdProgramListReqIsEmpty(t *testing.T) {
	raw := lightfx.CmdProgramListReq()
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ProgramListReq: ParsePacket failed")
	}
	if ptype != lightfx.ProgramListReq || len(payload) != 0 {
		t.Errorf("ProgramListReq: type=0x%02X payload=%v, want 0xB7 / empty", byte(ptype), payload)
	}
}
