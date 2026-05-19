package client

import (
	"scalefx/protocol/gear"
)

// Gear is the master-side GearControl effect: each unit is one
// retractable landing-gear assembly (H-bridge motor + status LEDs).
type Gear struct{ c *Client }

// Re-exports.
type (
	GearInfo        = gear.Gear
	GearStatusEntry = gear.GearStatus
	GearPhaseChange = gear.PhaseChange
)

const (
	GearAllStop    = gear.AllStop
	GearAllDeploy  = gear.AllDeploy
	GearAllRetract = gear.AllRetract
)

// List returns every configured gear unit on the hub.
func (g *Gear) List() ([]GearInfo, error) {
	resp, err := g.c.sendForResp(gear.CmdListReq(), gear.ListResp)
	if err != nil {
		return nil, err
	}
	return gear.DecodeList(resp.Payload)
}

// Status returns the current phase for every configured gear unit.
func (g *Gear) Status() ([]GearStatusEntry, error) {
	resp, err := g.c.sendForResp(gear.CmdStatusReq(), gear.StatusResp)
	if err != nil {
		return nil, err
	}
	return gear.DecodeStatus(resp.Payload)
}

// Deploy lowers gear `id`.  Returns GEAR_IN_ERROR_STATE if the unit
// needs a Stop() call to clear an error before it accepts motion.
func (g *Gear) Deploy(id byte) error  { return g.c.sendExpectACK(gear.CmdDeploy(id)) }

// Retract raises gear `id`.
func (g *Gear) Retract(id byte) error { return g.c.sendExpectACK(gear.CmdRetract(id)) }

// Stop halts gear `id` mid-motion.  Also doubles as an error-state
// reset (firmware clears the unit's error flag on receipt).
func (g *Gear) Stop(id byte) error    { return g.c.sendExpectACK(gear.CmdStop(id)) }

// All applies a single action to every configured gear (stop/deploy/retract).
func (g *Gear) All(action byte) error { return g.c.sendExpectACK(gear.CmdAll(action)) }
