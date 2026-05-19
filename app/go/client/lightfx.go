package client

import (
	"fmt"

	"scalefx/protocol/lightfx"
)

// LightFx exposes the master-side LightFX effect — program selector +
// master brightness scalar.  All commands run on the hub.
type LightFx struct{ c *Client }

// Program re-exports lightfx.Program so CLI callers don't need to pull
// the protocol package directly.
type Program = lightfx.Program

// LightFxStatus re-exports lightfx.Status with a more descriptive name
// at the client layer.
type LightFxStatus = lightfx.Status

// Programs returns the catalog of LightFX programs the hub firmware
// has registered.  The hub answers from its compile-time program table.
func (l *LightFx) Programs() ([]Program, error) {
	resp, err := l.c.sendForResp(lightfx.CmdProgramListReq(), lightfx.ProgramListResp)
	if err != nil {
		return nil, err
	}
	return lightfx.DecodeProgramList(resp.Payload)
}

// SelectProgram atomically switches to program `idx`.  Returns
// LIGHTFX_UNKNOWN_PROGRAM when out of range.
func (l *LightFx) SelectProgram(idx byte) error {
	return l.c.sendExpectACK(lightfx.CmdProgramSelect(idx))
}

// ResetProgram drops the active program.  Every claimed LED goes off.
func (l *LightFx) ResetProgram() error {
	return l.c.sendExpectACK(lightfx.CmdProgramReset())
}

// SetMasterBrightness sets the global brightness scalar (0..100 %).
// The hub re-broadcasts LED_SET_BRIGHTNESS to every claimed channel.
func (l *LightFx) SetMasterBrightness(pct byte) error {
	if pct > 100 {
		return fmt.Errorf("brightness must be 0..100, got %d", pct)
	}
	return l.c.sendExpectACK(lightfx.CmdMasterBrightness(pct))
}

// Status returns the LightFX status snapshot — active program index
// (0xFF == none) + master brightness percentage.
func (l *LightFx) Status() (LightFxStatus, error) {
	resp, err := l.c.sendForResp(lightfx.CmdStatusReq(), lightfx.StatusResp)
	if err != nil {
		return LightFxStatus{}, err
	}
	return lightfx.DecodeStatus(resp.Payload)
}
