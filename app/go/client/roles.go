package client

// Roles — role-layer LIFECYCLE facet (attach / detach / list) on the
// CONNECTED board, with NO GUID and NO topology hop.  This is the
// low-level bench path: drive an expander's role layer straight from the
// CLI with no hub present.
//
// All role DRIVE / QUERY commands (servo profile + target, motor/heater
// element, LED queue, bi-motor seek/guard/status) moved to the
// GUID-transparent `RoleTarget` (`c.Role(guid)` in roletarget.go), which
// routes `guid == ""` to the hub and any other GUID through
// TOPOLOGY_ROLE_FORWARD.  Use that for every read/write of live role
// state; this facet only binds/unbinds roles.  The shared struct
// re-exports below are consumed by both facets.

import (
	"scalefx/protocol/roles"
)

// Re-exports so callers don't have to import the `roles` protocol
// package directly for the common struct shapes.
type (
	ServoProfile    = roles.ServoMotionProfile
	MotorElement    = roles.MotorElementConfig
	HeaterElement   = roles.HeaterElementConfig
	ElementScaling  = roles.ElementScalingMode
)

const (
	ScalingPassthrough = roles.ScalingPassthrough
	ScalingLinear      = roles.ScalingLinear
	ScalingQuadratic   = roles.ScalingQuadratic
)

// Roles is the role-layer live-tune facet.
type Roles struct{ c *Client }

// RoleListEntry re-export so callers don't import the protocol package.
type RoleListEntry = roles.RoleListEntry

// ─── Role lifecycle (direct role layer — no GUID, no topology hop) ────
//
// Attach / detach / list a role on the CONNECTED board's RoleServicePolicy
// directly (ROLE_ATTACH 0x40 / ROLE_DETACH 0x41 / ROLE_LIST_REQ 0x42).
// This is the LOW-LEVEL bench-testing path: drive an expander's role layer
// straight from the CLI with NO hub present.  The production path is the
// hub's GUID-addressed `Topology.AttachRole` (which forwards over CDC) —
// use that when a master owns the expander.  ROLE_ATTACH/DETACH ACK then
// emit an async ROLE_ATTACHED/DETACHED event; ROLE_LIST_REQ replies with a
// typed ROLE_LIST_RESP.

// Attach binds a role to (portKind, portIdx) on the connected board.  cfg
// is the role's optional attach-config blob (nil = role defaults).
func (r *Roles) Attach(portKind, portIdx, roleKind byte, cfg []byte) error {
	return r.c.sendExpectACK(roles.CmdRoleAttach(portKind, portIdx, roleKind, cfg))
}

// Detach removes whatever role is bound to (portKind, portIdx).
func (r *Roles) Detach(portKind, portIdx byte) error {
	return r.c.sendExpectACK(roles.CmdRoleDetach(portKind, portIdx))
}

// List reads the roles currently attached on the connected board.
func (r *Roles) List() ([]RoleListEntry, error) {
	resp, err := r.c.sendForResp(roles.CmdRoleListReq(), roles.RoleListResp)
	if err != nil {
		return nil, err
	}
	return roles.DecodeRoleListPayload(resp.Payload)
}

// ─── LED animator (shared type re-exports) ──────────────────────────

// LedEvent re-exports the protocol-level type so client callers (Studio
// preview, future CLI test commands) don't pull in protocol/roles.
type (
	LedEvent     = roles.LightEvent
	LedEventKind = roles.LightEventKind
)
const (
	LedEventOn      = roles.LightEventOn
	LedEventOff     = roles.LightEventOff
	LedEventFlash   = roles.LightEventFlash
	LedEventFadeIn  = roles.LightEventFadeIn
	LedEventFadeOut = roles.LightEventFadeOut
	LedEventFading  = roles.LightEventFading
	LedEventBeacon  = roles.LightEventBeacon
	LedEventLoop    = roles.LightEventFlagsLoop
)
