package client

import (
	"scalefx/protocol/gunfx"
)

// Gun is the master-side GunFX effect.  Each unit couples a muzzle
// flash LED with optional recoil servo, smoke heater, and RC trigger.
type Gun struct{ c *Client }

// Re-exports.
type (
	GunUnitStatus = gunfx.GunStatus
	GunShot       = gunfx.Shot
)

// FireOnce triggers exactly one shot on gun `id`.
func (g *Gun) FireOnce(id byte) error {
	return g.c.sendExpectACK(gunfx.CmdFireOnce(id))
}

// StartFiring kicks off auto-fire at `rpm` rounds / minute.  rpm == 0
// falls back to the unit's configured default cadence.
func (g *Gun) StartFiring(id byte, rpm uint16) error {
	return g.c.sendExpectACK(gunfx.CmdStartFiring(id, rpm))
}

// StopFiring halts auto-fire on gun `id`.
func (g *Gun) StopFiring(id byte) error {
	return g.c.sendExpectACK(gunfx.CmdStopFiring(id))
}

// SmokeArm toggles the smoke heater on gun `id`.  armed == 0 disables.
func (g *Gun) SmokeArm(id, armed byte) error {
	return g.c.sendExpectACK(gunfx.CmdSmokeArm(id, armed))
}

// Status returns one entry per registered gun unit — firing state +
// smoke-heater armed flag.
func (g *Gun) Status() ([]GunUnitStatus, error) {
	resp, err := g.c.sendForResp(gunfx.CmdStatusReq(), gunfx.StatusResp)
	if err != nil {
		return nil, err
	}
	return gunfx.DecodeStatus(resp.Payload)
}
