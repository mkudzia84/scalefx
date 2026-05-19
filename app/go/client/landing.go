package client

import (
	"scalefx/protocol/landing"
)

// LandingLights is the master-side landing-light effect: each unit is
// a (servo + LED group) primitive owned exclusively by the hub.  Other
// effects reference these by ID — Studio / CLI talks to the hub.
type LandingLights struct{ c *Client }

// Re-exports so CLI callers don't pull `protocol/landing` directly.
type (
	LandingLight       = landing.Light
	LandingLightStatus = landing.LightStatus
	LandingPhaseChange = landing.PhaseChange
)

const (
	LandingOff = landing.StateOff
	LandingOn  = landing.StateOn
)

// List returns every landing light declared on the hub, with the
// current owner effect ID and lifecycle phase.
func (l *LandingLights) List() ([]LandingLight, error) {
	resp, err := l.c.sendForResp(landing.CmdListReq(), landing.ListResp)
	if err != nil {
		return nil, err
	}
	return landing.DecodeList(resp.Payload)
}

// SetState toggles a landing-light unit on/off — only honored when the
// caller's effect owns the unit.  Master-internal calls (Studio / CLI)
// hit the special-case "no-owner" code path.
func (l *LandingLights) SetState(id, state byte) error {
	return l.c.sendExpectACK(landing.CmdSetState(id, state))
}

// On is a convenience for SetState(id, LandingOn).
func (l *LandingLights) On(id byte) error { return l.SetState(id, LandingOn) }

// Off is a convenience for SetState(id, LandingOff).
func (l *LandingLights) Off(id byte) error { return l.SetState(id, LandingOff) }

// Status returns per-landing-light lifecycle phases (every entry).
func (l *LandingLights) Status() ([]LandingLightStatus, error) {
	resp, err := l.c.sendForResp(landing.CmdStatusReq(), landing.StatusResp)
	if err != nil {
		return nil, err
	}
	return landing.DecodeStatus(resp.Payload)
}
