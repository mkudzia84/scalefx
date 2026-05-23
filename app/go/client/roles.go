package client

// Roles — direct access to the role layer's live-tune wire surface
// (Phase 2.9.x of GunFX rollout, instructions/22).  These commands sit
// BELOW any effect (`Engine`, `Gun`, `Gear`, …) and address ports by
// GUID + (kind, idx).  They're how Studio's port-role row pushes
// servo-profile / motor-element / heater-element changes WITHOUT
// re-attaching the role (which would lose target/position state).
//
// Targeting: every call carries a GUID + portIdx pair.  An empty GUID
// targets the hub itself; any other GUID is routed through
// TopologyService::sendRoleCommand to the addressed expander.  The
// CmdXxx builders below produce hub-local packets; for cross-board
// addressing use `c.Topology.SendRoleCommand(guid, ...)` instead.

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

// ─── Servo motion profile ────────────────────────────────────────────

// ServoSetProfile pushes a new motion profile (clamp + speed + accel +
// jerk + REV + center) to a servo on the local hub.  In-flight target
// is preserved (re-clamped into the new range).
func (r *Roles) ServoSetProfile(portIdx byte, p ServoProfile) error {
	return r.c.sendExpectACK(roles.CmdServoSetProfile(portIdx, p))
}

// ServoGetProfile reads the role's current motion profile back.
func (r *Roles) ServoGetProfile(portIdx byte) (ServoProfile, error) {
	resp, err := r.c.sendForResp(roles.CmdServoGetProfile(portIdx), roles.ServoProfileResp)
	if err != nil {
		return ServoProfile{}, err
	}
	_, p, err := roles.DecodeServoProfile(resp.Payload)
	return p, err
}

// ServoSetTarget commands a new target position (intent layer — the
// role's MotionProfile1D shapes the slew to it).
func (r *Roles) ServoSetTarget(portIdx byte, targetUs uint16) error {
	return r.c.sendExpectACK(roles.CmdServoSetTarget(portIdx, targetUs))
}

// ─── DC motor element (voltage scaling) ──────────────────────────────

// MotorSetElement updates the element rated voltage + scaling mode for
// a DC motor on a Pwm port.  Affects future `setPct(...)` calls; the
// last commanded duty stays in effect until the next set.
func (r *Roles) MotorSetElement(portIdx byte, e MotorElement) error {
	return r.c.sendExpectACK(roles.CmdMotorSetElement(portIdx, e))
}

// MotorGetElement reads the role's current element config + port rail
// voltage back.  PortRailMv is read-only (set at port-declaration time).
func (r *Roles) MotorGetElement(portIdx byte) (MotorElement, error) {
	resp, err := r.c.sendForResp(roles.CmdMotorGetElement(portIdx), roles.MotorElementResp)
	if err != nil {
		return MotorElement{}, err
	}
	_, e, err := roles.DecodeMotorElement(resp.Payload)
	return e, err
}

// MotorSetPct — Rule 42 intent layer.  "Drive this DC motor at `pct`
// percent of its rated voltage" — the role applies scaleDuty() using
// the element + port rail voltages.  Replaces the raw `MotorSetDuty`
// for callers that think in element-relative percent (the smoke-fan
// puff path in GunFx, future operators tuning fan strength from the
// IO tab).  Raw `MotorSetDuty` stays for advanced bypass.
func (r *Roles) MotorSetPct(portIdx, pct byte) error {
	return r.c.sendExpectACK(roles.CmdMotorSetPct(portIdx, pct))
}

// ─── Heater element (voltage scaling + drive pct + hysteresis) ──────

// HeaterSetElement updates the heater's element rated voltage, scaling
// mode, drive percentage, and hysteresis.  The bang-bang target temp
// stays on its own setter (`HeaterSetTarget`, future).
func (r *Roles) HeaterSetElement(portIdx byte, e HeaterElement) error {
	return r.c.sendExpectACK(roles.CmdHeaterSetElement(portIdx, e))
}

// HeaterGetElement reads the role's current element config + drive pct
// + hysteresis + port rail voltage back.
func (r *Roles) HeaterGetElement(portIdx byte) (HeaterElement, error) {
	resp, err := r.c.sendForResp(roles.CmdHeaterGetElement(portIdx), roles.HeaterElementResp)
	if err != nil {
		return HeaterElement{}, err
	}
	_, e, err := roles.DecodeHeaterElement(resp.Payload)
	return e, err
}
