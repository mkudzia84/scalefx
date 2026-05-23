package client

import (
	"scalefx/protocol/gunfx"
)

// Gun is the master-side GunFX effect.  Each unit couples a muzzle
// flash LED with optional recoil servo, smoke heater, and RC trigger.
type Gun struct{ c *Client }

// Re-exports.
type (
	GunUnitStatus     = gunfx.GunStatus
	GunShot           = gunfx.Shot
	GunManualState    = gunfx.ManualState
	GunVerboseStatus  = gunfx.VerboseStatus
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

// ── Phase 1 of the GunFX rollout (instructions/22): manual override +
// verbose status. Firmware NACKs with GUN_NOT_IMPLEMENTED until Phase 2
// fills the bodies — Studio surfaces the error in its log so the
// round-trip is visible end-to-end.

// ManualSet drives one gun in manual override mode — Studio's
// "simulate" panel uses this to bypass RC and puppet yaw/pitch/fire/
// smoke directly from the GUI.  `state.Flags` selects which
// subsystems this call addresses (OR of `gunfx.ManualFlag*`).
//
// The firmware auto-releases manual mode after ~5 s without another
// MANUAL_SET (so a Studio crash never leaves a gun stuck in puppet
// mode); call ManualRelease for an immediate handover back to RC.
func (g *Gun) ManualSet(id byte, state GunManualState) error {
	return g.c.sendExpectACK(gunfx.CmdManualSet(id, state))
}

// ManualRelease exits manual mode for one gun — RC takes over
// immediately on the next tick.
func (g *Gun) ManualRelease(id byte) error {
	return g.c.sendExpectACK(gunfx.CmdManualRelease(id))
}

// VerboseStatusSubscribe enables (1) or disables (0) ~10 Hz
// GUN_VERBOSE_STATUS broadcasts for one gun.  The broadcasts arrive
// via the client's async-packet dispatcher and decode through
// `gunfx.DecodeVerboseStatus`.  Subscriptions are released on
// disconnect; on reconnect Studio must re-subscribe.
func (g *Gun) VerboseStatusSubscribe(id, enable byte) error {
	return g.c.sendExpectACK(gunfx.CmdVerboseStatusReq(id, enable))
}
