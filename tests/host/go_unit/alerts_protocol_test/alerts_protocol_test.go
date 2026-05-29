package alerts_protocol_test

// Unit tests for the Alerts wire protocol (CLAUDE.md "Alerts 0xD3-0xD6").
// AlertService plays sound-effect chimes on the HubFx Alert channel
// (system events: boot, battery low, expander mounted, etc.).

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/alerts"
)

func TestAlertsPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"BEEP", alerts.Beep, 0xD3},
		{"STOP_ALERT", alerts.StopAlert, 0xD4},
		{"STATUS_REQ", alerts.StatusReq, 0xD5},
		{"STATUS_RESP", alerts.StatusResp, 0xD6},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestCmdBeepPayloadShape(t *testing.T) {
	// Wire: [severity:u8][outputMask:u8]  per audio Rule 47 routing.
	raw := alerts.CmdBeep(3, 0x03 /* stereo */)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != alerts.Beep {
		t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(alerts.Beep))
	}
	if len(payload) != 2 || payload[0] != 3 || payload[1] != 0x03 {
		t.Errorf("Beep(3, 0x03) payload = %v, want [3 3]", payload)
	}
}

func TestCmdStopPayloadIsEmpty(t *testing.T) {
	raw := alerts.CmdStop()
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != alerts.StopAlert || len(payload) != 0 {
		t.Errorf("Stop: type=0x%02X payload=%v, want 0xD4 / empty", byte(ptype), payload)
	}
}
