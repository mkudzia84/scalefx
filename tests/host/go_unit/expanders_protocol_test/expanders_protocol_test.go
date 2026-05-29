package expanders_protocol_test

// Unit tests for the Expander wire protocol (CLAUDE.md "Expander 0x80-0x87").
// Expanders are the Pico-side per-board enumeration/discovery surface
// the HubFX master uses to track which expanders are mounted + their
// GUIDs + battery state.

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/expanders"
)

func TestExpandersPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"EXPANDER_LIST_REQ", expanders.ExpanderListReq, 0x80},
		{"EXPANDER_LIST_RESP", expanders.ExpanderListResp, 0x81},
		{"EXPANDER_CONNECTED", expanders.ExpanderConnected, 0x82},
		{"EXPANDER_DISCONNECTED", expanders.ExpanderDisconnected, 0x83},
		{"EXPANDER_IDENTIFIED", expanders.ExpanderIdentified, 0x84},
		{"EXPANDER_SYSTEM_INFO_REQ", expanders.ExpanderSystemInfoReq, 0x85},
		{"EXPANDER_SYSTEM_INFO_RESP", expanders.ExpanderSystemInfoResp, 0x86},
		{"EXPANDER_COLLISION", expanders.ExpanderCollision, 0x87},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

// Expander bytes must NOT stray into Topology (0x88-0x8F) — both
// live on the master and adjacency makes drift easy.
func TestExpanderDoesntStrayIntoTopology(t *testing.T) {
	allocs := []protocol.PacketType{
		expanders.ExpanderListReq, expanders.ExpanderListResp,
		expanders.ExpanderConnected, expanders.ExpanderDisconnected,
		expanders.ExpanderIdentified, expanders.ExpanderSystemInfoReq,
		expanders.ExpanderSystemInfoResp, expanders.ExpanderCollision,
	}
	for _, p := range allocs {
		b := byte(p)
		if b < 0x80 || b > 0x87 {
			t.Errorf("0x%02X outside CLAUDE.md 'Expander 0x80-0x87' range", b)
		}
	}
}
