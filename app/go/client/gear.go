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

	GearStepUp   = gear.StepUp
	GearStepDown = gear.StepDown
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
// is in an error state — call Reset() to clear it first.
func (g *Gear) Deploy(id byte) error  { return g.c.sendExpectACK(gear.CmdDeploy(id)) }

// Retract raises gear `id`.
func (g *Gear) Retract(id byte) error { return g.c.sendExpectACK(gear.CmdRetract(id)) }

// Stop halts gear `id` in place (motor brake + door freeze → HELD).  Resumes
// on the next Deploy/Retract.  Does NOT clear an error — use Reset() for that.
func (g *Gear) Stop(id byte) error    { return g.c.sendExpectACK(gear.CmdStop(id)) }

// All applies a single action to every configured gear (stop/deploy/retract).
func (g *Gear) All(action byte) error { return g.c.sendExpectACK(gear.CmdAll(action)) }

// Step advances gear `id` ONE leg toward `target` (GearStepUp / GearStepDown)
// then parks at the next boundary — the manual "do one item in the sequence"
// command.  Auto-clears a prior error first.
func (g *Gear) Step(id, target byte) error { return g.c.sendExpectACK(gear.CmdStep(id, target)) }

// EStop emergency-holds gear `id` (brake + freeze in place → HELD).
func (g *Gear) EStop(id byte) error { return g.c.sendExpectACK(gear.CmdEstop(id)) }

// EStopAll emergency-holds EVERY strut in place (brake + freeze → HELD).
func (g *Gear) EStopAll() error { return g.c.sendExpectACK(gear.CmdEstopAll()) }

// Reset clears gear `id`'s error state (ERROR → Retracted) so it
// accepts deploy/retract again.  No-op if not in error.
//
// Gear-level calibration was removed (instructions/29 decision #3) —
// endstop calibration now lives entirely on the BiDcMotor role's
// LiveRatio + ceiling guard (tune it via the motor role's BIMOTOR_SET_GUARD).
func (g *Gear) Reset(id byte) error { return g.c.sendExpectACK(gear.CmdReset(id)) }
