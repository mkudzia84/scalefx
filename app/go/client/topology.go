package client

import (
	"scalefx/protocol/roles"
	"scalefx/protocol/topology"
)

// Topology exposes the GUID-addressed wire surface that the hub
// provides for enumerating ports and roles across the entire system
// and attaching / detaching roles on remote boards.
//
// `guid == ""` always targets the hub itself.  A 4-hex-char GUID
// targets a specific expander.
type Topology struct{ c *Client }

// BoardPorts is one board's contribution to a PortList result.
type BoardPorts = topology.BoardPorts

// BoardRoles is one board's contribution to a RoleList result.
type BoardRoles = topology.BoardRoles

// PortList returns the port descriptors for the given board.  Empty
// guid → returns every board in the system (hub + every connected
// expander) in one round-trip.
func (t *Topology) PortList(guid string) ([]BoardPorts, error) {
	resp, err := t.c.sendForResp(topology.CmdPortListReq(guid), topology.TopologyPortListResp)
	if err != nil {
		return nil, err
	}
	return topology.DecodePortListResp(resp.Payload)
}

// RoleList returns the attached roles for the given board.  Empty
// guid → every board in the system.
func (t *Topology) RoleList(guid string) ([]BoardRoles, error) {
	resp, err := t.c.sendForResp(topology.CmdRoleListReq(guid), topology.TopologyRoleListResp)
	if err != nil {
		return nil, err
	}
	return topology.DecodeRoleListResp(resp.Payload)
}

// AttachRole binds a role to (portKind, portIdx) on the board
// identified by `guid`.  `cfg` is the role-specific config blob
// (defined per-role in serial/roles.h).  Empty cfg → role defaults.
func (t *Topology) AttachRole(guid string, portKind, portIdx, roleKind byte, cfg []byte) error {
	return t.c.sendExpectACK(topology.CmdRoleAttach(guid, portKind, portIdx, roleKind, cfg))
}

// DetachRole clears the role on (portKind, portIdx) of the target board.
func (t *Topology) DetachRole(guid string, portKind, portIdx byte) error {
	return t.c.sendExpectACK(topology.CmdRoleDetach(guid, portKind, portIdx))
}

// FlattenRoles returns every attached role across every board, with
// the source GUID denormalized into each entry so callers can iterate
// without re-keying.
type FlatRole struct {
	GUID     string                `json:"guid"`
	PortKind byte                  `json:"portKind"`
	PortIdx  byte                  `json:"portIdx"`
	RoleKind byte                  `json:"roleKind"`
	Flags    byte                  `json:"flags"`
	_        roles.RoleListEntry   // keep dep on roles so its packet names register
}

func (t *Topology) FlattenRoles(boards []BoardRoles) []FlatRole {
	out := []FlatRole{}
	for _, b := range boards {
		for _, r := range b.Roles {
			out = append(out, FlatRole{
				GUID:     b.GUID,
				PortKind: r.PortKind,
				PortIdx:  r.PortIdx,
				RoleKind: r.RoleKind,
				Flags:    r.Flags,
			})
		}
	}
	return out
}
