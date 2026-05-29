package topology_protocol_test

// Unit tests for the Topology wire protocol
// (CLAUDE.md "Topology 0x88-0x8E", plus TopologyRoleForward at 0x8F).
// Topology is master-only — used by HubFX to enumerate
// GUID-addressable ports and roles across local + expander boards.

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/topology"
)

func TestTopologyPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"TOPOLOGY_PORT_LIST_REQ", topology.TopologyPortListReq, 0x88},
		{"TOPOLOGY_PORT_LIST_RESP", topology.TopologyPortListResp, 0x89},
		{"TOPOLOGY_ROLE_LIST_REQ", topology.TopologyRoleListReq, 0x8A},
		{"TOPOLOGY_ROLE_LIST_RESP", topology.TopologyRoleListResp, 0x8B},
		{"TOPOLOGY_ROLE_ATTACH", topology.TopologyRoleAttach, 0x8C},
		{"TOPOLOGY_ROLE_DETACH", topology.TopologyRoleDetach, 0x8D},
		{"TOPOLOGY_ROLE_EVENT", topology.TopologyRoleEvent, 0x8E},
		{"TOPOLOGY_ROLE_FORWARD", topology.TopologyRoleForward, 0x8F},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

// Topology lives wedged between Expander (0x80-0x87) and Config
// (0x90-0x92).  Verify nothing strayed into either neighbour's range —
// a regression here breaks BoardOf<> dispatch silently.
func TestTopologyDoesntStrayOutsideAllocation(t *testing.T) {
	allocs := []protocol.PacketType{
		topology.TopologyPortListReq, topology.TopologyPortListResp,
		topology.TopologyRoleListReq, topology.TopologyRoleListResp,
		topology.TopologyRoleAttach, topology.TopologyRoleDetach,
		topology.TopologyRoleEvent, topology.TopologyRoleForward,
	}
	for _, p := range allocs {
		b := byte(p)
		if b < 0x88 || b > 0x8F {
			t.Errorf("0x%02X outside CLAUDE.md 'Topology 0x88-0x8F' range "+
				"(neighbours: Expander 0x80-0x87, Config 0x90-0x92)", b)
		}
	}
}
