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

// ServoSetBroadcastHz subscribes (global) to the batched servo telemetry
// stream (SERVO_MOTION_UPDATE). hz=0 turns it off. Live positions then flow
// through Events.OnServoMotion.
func (r *Roles) ServoSetBroadcastHz(hz byte) error {
	return r.c.sendExpectACK(roles.CmdServoSetBroadcastHz(hz))
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

// ─── LED animator (queue / start / stop / brightness) ───────────────

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

// LedQueueLoad pushes a complete events list to one LED port.  Caller
// sets the LOOP flag on events[0] for repeating patterns.  Pair with
// LedStart to play, LedStop to halt.
func (r *Roles) LedQueueLoad(portIdx byte, events []LedEvent) error {
	return r.c.sendExpectACK(roles.CmdLedQueueLoad(portIdx, events))
}
// LedStart kicks off the queued pattern on one LED port (idempotent —
// re-starting a running queue restarts the cycle).
func (r *Roles) LedStart(portIdx byte) error {
	return r.c.sendExpectACK(roles.CmdLedStart(portIdx))
}
// LedStop halts the queue + drives the LED off.
func (r *Roles) LedStop(portIdx byte) error {
	return r.c.sendExpectACK(roles.CmdLedStop(portIdx))
}
// LedSetBrightness sets a per-port master scale (0..100) on top of
// each event's brightness — matches the per-channel brightness_pct
// in the program YAML.
func (r *Roles) LedSetBrightness(portIdx, pct byte) error {
	return r.c.sendExpectACK(roles.CmdLedSetBrightness(portIdx, pct))
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

// ─── Bi-directional DC motor (BiDcMotor role) ─────────────────────────
//
// Drives a gear/door motor to a logical endstop (A/B) with stall-detected
// seek, and reads back verbose status. The role lives on a GearControl
// expander; these wrap the protocol/roles BiMotor commands (Strategy A).

// BiMotorMoveToEnd drives the motor toward logical end `pos` (A/B) at
// `signedDuty` (sign = direction), aborting after `timeoutMs`. ACK is
// immediate; the seek OUTCOME arrives async as BIMOTOR_ENDSTOP_RESULT.
func (r *Roles) BiMotorMoveToEnd(portIdx byte, pos roles.BiMotorPosition, signedDuty int16, timeoutMs uint16) error {
	return r.c.sendExpectACK(roles.CmdBiMotorMoveToEnd(portIdx, pos, signedDuty, timeoutMs))
}

// BiMotorSeekEndstop is the position-agnostic seek (drives at signedDuty
// until a stall/endstop or timeout); doesn't label which end was reached.
func (r *Roles) BiMotorSeekEndstop(portIdx byte, signedDuty int16, timeoutMs uint16) error {
	return r.c.sendExpectACK(roles.CmdBiMotorSeekEndstop(portIdx, signedDuty, timeoutMs))
}

// BiMotorBrake hard-stops the motor (active brake — both leads low).
func (r *Roles) BiMotorBrake(portIdx byte) error {
	return r.c.sendExpectACK(roles.CmdBiMotorBrake(portIdx))
}

// BiMotorSetGuardFixed retunes the stall guard to Fixed mode — trip when
// |I| ≥ thresholdMa sustained for windowMs.
func (r *Roles) BiMotorSetGuardFixed(portIdx, thresholdMa, windowMs uint16) error {
	return r.c.sendExpectACK(roles.CmdBiMotorSetGuard(byte(portIdx),
		roles.BiMotorGuardFixed, windowMs, thresholdMa, 0, 0, 0))
}

// BiMotorSetGuardLiveRatio retunes the stall guard to LiveRatio mode — the
// role averages running current per stroke (after inrushBlankMs) for
// runSampleMs, then trips when |I| ≥ baseline × (ratioX100/100) sustained for
// windowMs.  Voltage-independent; no stored calibration (re-measures every
// stroke).  maxTravelMs is an absolute failsafe (0 = use the seek timeout).
func (r *Roles) BiMotorSetGuardLiveRatio(portIdx, ratioX100, runSampleMs, inrushBlankMs, windowMs, maxTravelMs uint16) error {
	return r.c.sendExpectACK(roles.CmdBiMotorSetGuard(byte(portIdx),
		roles.BiMotorGuardLiveRatio, windowMs, ratioX100, runSampleMs, inrushBlankMs, maxTravelMs))
}

// BiMotorSetSigned drives the motor at a raw signed duty with NO stall
// guard — for manual jogging.  Sign = direction; 0 = coast-to-brake.
func (r *Roles) BiMotorSetSigned(portIdx byte, signed int16) error {
	return r.c.sendExpectACK(roles.CmdBiMotorSetSigned(portIdx, signed))
}

// BiMotorSetProbe configures the dual-stage soft-start probe — before a seek
// applies full power, drive at probePct% of the seek duty and classify
// free-vs-already-at-stop from the current curve.  probePct 0 disables the
// probe.  Orthogonal to the stall guard.
func (r *Roles) BiMotorSetProbe(portIdx, probePct byte, windowMs uint16, dropPct byte) error {
	return r.c.sendExpectACK(roles.CmdBiMotorSetProbe(portIdx, probePct, windowMs, dropPct))
}

// BiMotorGetStatus reads the verbose live status (duty, voltage, current,
// stalled, position, guard mode).
func (r *Roles) BiMotorGetStatus(portIdx byte) (roles.BiMotorStatus, error) {
	resp, err := r.c.sendForResp(roles.CmdBiMotorGetStatus(portIdx), roles.BiMotorStatusResp)
	if err != nil {
		return roles.BiMotorStatus{}, err
	}
	return roles.DecodeBiMotorStatus(resp.Payload)
}
