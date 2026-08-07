package client

// RoleTarget — the GUID-transparent role I/O layer (Phase B of transparent
// expander roles).  A RoleTarget drives + queries a role on ONE board and
// routes every call transparently:
//
//   guid == ""  → hub-local  (direct role layer: sendExpectACK / sendForResp)
//   guid != ""  → forwarded  (Topology.SendRoleCommand / QueryRole over CDC)
//
// so a caller (CLI, Studio) treats an expander role exactly like a hub-local
// one.  It is GENERIC by construction: the two private primitives (`send` /
// `query`) reuse the EXISTING roles.CmdXxx builders + DecodeXxx decoders via
// protocol.ParsePacket — the transport never decodes the role packet.  Adding
// the transparent path for a new role (or a future board) is a one-line
// wrapper over send/query; no new transport, forwarding, or wire code.

import (
	"fmt"

	"scalefx/protocol"
	"scalefx/protocol/roles"
)

// RoleTarget addresses a single board's role layer (see package doc above).
type RoleTarget struct {
	c    *Client
	guid string // "" = hub-local
}

// Role returns a transparent role-I/O target for board `guid` ("" = the hub).
func (c *Client) Role(guid string) *RoleTarget { return &RoleTarget{c: c, guid: guid} }

// GUID returns the target board ("" = hub); IsLocal is the convenience predicate.
func (rt *RoleTarget) GUID() string  { return rt.guid }
func (rt *RoleTarget) IsLocal() bool { return rt.guid == "" }

// send routes a built role-command packet (from a roles.CmdXxx builder): the
// hub-local ACK path, or a GUID forward (the inner type + body are recovered
// from the packet, so we reuse the same builder for both directions).
func (rt *RoleTarget) send(pkt []byte) error {
	if rt.guid == "" {
		return rt.c.sendExpectACK(pkt)
	}
	t, _, body, ok := protocol.ParsePacket(pkt)
	if !ok {
		return fmt.Errorf("role: malformed command packet")
	}
	return rt.c.Topology.SendRoleCommand(rt.guid, byte(t), body)
}

// query routes a built role-query packet and returns the typed RESP payload —
// hub-local sendForResp, or the GUID-forwarded request-response (QueryRole).
func (rt *RoleTarget) query(pkt []byte, respType protocol.PacketType) ([]byte, error) {
	if rt.guid == "" {
		resp, err := rt.c.sendForResp(pkt, respType)
		if err != nil {
			return nil, err
		}
		return resp.Payload, nil
	}
	t, _, body, ok := protocol.ParsePacket(pkt)
	if !ok {
		return nil, fmt.Errorf("role: malformed query packet")
	}
	_, payload, err := rt.c.Topology.QueryRole(rt.guid, byte(t), body)
	return payload, err
}

// ─── Servo ────────────────────────────────────────────────────────────

func (rt *RoleTarget) ServoSetTarget(portIdx byte, targetUs uint16) error {
	return rt.send(roles.CmdServoSetTarget(portIdx, targetUs))
}
func (rt *RoleTarget) ServoSetProfile(portIdx byte, p ServoProfile) error {
	return rt.send(roles.CmdServoSetProfile(portIdx, p))
}
func (rt *RoleTarget) ServoGetProfile(portIdx byte) (ServoProfile, error) {
	payload, err := rt.query(roles.CmdServoGetProfile(portIdx), roles.ServoProfileResp)
	if err != nil {
		return ServoProfile{}, err
	}
	_, p, err := roles.DecodeServoProfile(payload)
	return p, err
}
func (rt *RoleTarget) ServoSetBroadcastHz(hz byte) error {
	return rt.send(roles.CmdServoSetBroadcastHz(hz))
}

// ─── Bi-directional DC motor (gear/door) ──────────────────────────────

func (rt *RoleTarget) BiMotorMoveToEnd(portIdx byte, pos roles.BiMotorPosition, signedDuty int16, timeoutMs uint16) error {
	return rt.send(roles.CmdBiMotorMoveToEnd(portIdx, pos, signedDuty, timeoutMs))
}
func (rt *RoleTarget) BiMotorSeekEndstop(portIdx byte, signedDuty int16, timeoutMs uint16) error {
	return rt.send(roles.CmdBiMotorSeekEndstop(portIdx, signedDuty, timeoutMs))
}
func (rt *RoleTarget) BiMotorBrake(portIdx byte) error {
	return rt.send(roles.CmdBiMotorBrake(portIdx))
}
func (rt *RoleTarget) BiMotorSetSigned(portIdx byte, signed int16) error {
	return rt.send(roles.CmdBiMotorSetSigned(portIdx, signed))
}
// The optional trailing elementMv (motor rated voltage → duty cap; 0 = cap
// off) emits the 16-byte SET_GUARD form; omitting it leaves the peer's cap
// untouched — see roles.CmdBiMotorSetGuard.
func (rt *RoleTarget) BiMotorSetGuardFixed(portIdx, thresholdMa, windowMs uint16, elementMv ...uint16) error {
	return rt.send(roles.CmdBiMotorSetGuard(byte(portIdx), roles.BiMotorGuardFixed, windowMs, thresholdMa, 0, 0, 0, 0, elementMv...))
}
func (rt *RoleTarget) BiMotorSetGuardLiveRatio(portIdx, ratioX100, runSampleMs, inrushBlankMs, windowMs, maxTravelMs, absMaxMa uint16, elementMv ...uint16) error {
	return rt.send(roles.CmdBiMotorSetGuard(byte(portIdx), roles.BiMotorGuardLiveRatio, windowMs, ratioX100, runSampleMs, inrushBlankMs, maxTravelMs, absMaxMa, elementMv...))
}
func (rt *RoleTarget) BiMotorGetStatus(portIdx byte) (roles.BiMotorStatus, error) {
	payload, err := rt.query(roles.CmdBiMotorGetStatus(portIdx), roles.BiMotorStatusResp)
	if err != nil {
		return roles.BiMotorStatus{}, err
	}
	return roles.DecodeBiMotorStatus(payload)
}

// ─── DC motor (element scaling) ───────────────────────────────────────

func (rt *RoleTarget) MotorSetPct(portIdx, pct byte) error {
	return rt.send(roles.CmdMotorSetPct(portIdx, pct))
}
func (rt *RoleTarget) MotorSetElement(portIdx byte, e MotorElement) error {
	return rt.send(roles.CmdMotorSetElement(portIdx, e))
}
func (rt *RoleTarget) MotorGetElement(portIdx byte) (MotorElement, error) {
	payload, err := rt.query(roles.CmdMotorGetElement(portIdx), roles.MotorElementResp)
	if err != nil {
		return MotorElement{}, err
	}
	_, e, err := roles.DecodeMotorElement(payload)
	return e, err
}

// ─── Heater (element scaling) ─────────────────────────────────────────

func (rt *RoleTarget) HeaterSetElement(portIdx byte, e HeaterElement) error {
	return rt.send(roles.CmdHeaterSetElement(portIdx, e))
}
func (rt *RoleTarget) HeaterGetElement(portIdx byte) (HeaterElement, error) {
	payload, err := rt.query(roles.CmdHeaterGetElement(portIdx), roles.HeaterElementResp)
	if err != nil {
		return HeaterElement{}, err
	}
	_, e, err := roles.DecodeHeaterElement(payload)
	return e, err
}

// ─── LED animator ─────────────────────────────────────────────────────

func (rt *RoleTarget) LedQueueLoad(portIdx byte, events []LedEvent) error {
	return rt.send(roles.CmdLedQueueLoad(portIdx, events))
}
func (rt *RoleTarget) LedStart(portIdx byte) error        { return rt.send(roles.CmdLedStart(portIdx)) }
func (rt *RoleTarget) LedStop(portIdx byte) error         { return rt.send(roles.CmdLedStop(portIdx)) }
func (rt *RoleTarget) LedSetBrightness(portIdx, pct byte) error {
	return rt.send(roles.CmdLedSetBrightness(portIdx, pct))
}
