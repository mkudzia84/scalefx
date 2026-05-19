package client

import (
	"scalefx/protocol/enginefx"
)

// Engine is the master-side EngineFX effect (4-state engine sound
// state machine: Stopped → Starting → Running → Stopping → Stopped).
type Engine struct{ c *Client }

// Re-exports.
type (
	EngineStatus      = enginefx.Status
	EngineStateChange = enginefx.StateChange
)

// Start kicks off the engine startup sequence (plays the starting
// sound, then loops the running sound).
func (e *Engine) Start() error { return e.c.sendExpectACK(enginefx.CmdStart()) }

// Stop kicks off the engine shutdown sequence (plays the stopping
// sound, then transitions back to Stopped).
func (e *Engine) Stop() error { return e.c.sendExpectACK(enginefx.CmdStop()) }

// Status returns the current engine state — including whether the RC
// PWM toggle (if configured) is presently engaged.
func (e *Engine) Status() (EngineStatus, error) {
	resp, err := e.c.sendForResp(enginefx.CmdStatusReq(), enginefx.StatusResp)
	if err != nil {
		return EngineStatus{}, err
	}
	return enginefx.DecodeStatus(resp.Payload)
}
