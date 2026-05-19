package api

import (
	"scalefx/protocol/lightfx"
)

// LightFxApi provides LightFX lighting effects operations.
type LightFxApi struct{ apiClient }

func (a *LightFxApi) LedSet(ch, brightness byte) ApiResult { return a.sendACK(lightfx.CmdLedSet(ch, brightness)) }
func (a *LightFxApi) LedOff(ch byte) ApiResult             { return a.sendACK(lightfx.CmdLedOff(ch)) }
func (a *LightFxApi) LedStatus() ApiResult                 { return a.sendQuery(lightfx.CmdLedStatus(), lightfx.LedStatusResp) }

func (a *LightFxApi) SeqAdd(ch, eventType byte, p1, p2 uint16, p3, p4 byte, extra ...byte) ApiResult {
	return a.sendACK(lightfx.CmdLedSeqAdd(ch, eventType, p1, p2, p3, p4, extra...))
}

func (a *LightFxApi) SeqClear(ch byte) ApiResult              { return a.sendACK(lightfx.CmdLedSeqClear(ch)) }
func (a *LightFxApi) SeqStart(ch byte, loops uint16) ApiResult { return a.sendACK(lightfx.CmdLedSeqStart(ch, loops)) }
func (a *LightFxApi) SeqStop(ch byte) ApiResult                { return a.sendACK(lightfx.CmdLedSeqStop(ch)) }
func (a *LightFxApi) SeqRestart(ch byte) ApiResult             { return a.sendACK(lightfx.CmdLedSeqRestart(ch)) }
func (a *LightFxApi) SeqStatus(ch byte) ApiResult              { return a.sendQuery(lightfx.CmdLedSeqStatus(ch), lightfx.LedSeqStatusResp) }
func (a *LightFxApi) SeqQueue(ch byte) ApiResult               { return a.sendQuery(lightfx.CmdLedSeqQueue(ch), lightfx.LedSeqQueueResp) }
func (a *LightFxApi) MasterBrightness(val byte) ApiResult      { return a.sendACK(lightfx.CmdMasterBrightness(val)) }

func (a *LightFxApi) ServoSet(id byte, pulse_us uint16) ApiResult {
	return a.sendACK(lightfx.CmdServoSet(id, pulse_us))
}

func (a *LightFxApi) ServoConfig(id byte, min, max, speed, accel, decel uint16, reversed bool) ApiResult {
	return a.sendACK(lightfx.CmdServoSettings(id, min, max, speed, accel, decel, reversed))
}

// LandingBind binds a landing group: servoID=0 means no servo, channelMask is bitmask (bit0=ch1..bit7=ch8).
func (a *LightFxApi) LandingBind(slot, servo, channelMask, bright byte) ApiResult {
	return a.sendACK(lightfx.CmdLandingLightBind(slot, servo, channelMask, bright))
}

func (a *LightFxApi) LandingUnbind(slot byte) ApiResult  { return a.sendACK(lightfx.CmdLandingLightUnbind(slot)) }
func (a *LightFxApi) LandingDeploy(slot byte) ApiResult  { return a.sendACK(lightfx.CmdLandingLightDeploy(slot)) }
func (a *LightFxApi) LandingRetract(slot byte) ApiResult { return a.sendACK(lightfx.CmdLandingLightRetract(slot)) }
func (a *LightFxApi) Reset(ch byte) ApiResult            { return a.sendACK(lightfx.CmdLedReset(ch)) }
func (a *LightFxApi) Enable(ch byte, enabled bool) ApiResult { return a.sendACK(lightfx.CmdLedEnable(ch, enabled)) }

// BatteryAutoCutoff toggles the LightFX low-voltage soft-cutoff (disable all
// LED channels when the pack drops below the chemistry's per-cell threshold).
func (a *LightFxApi) BatteryAutoCutoff(enabled bool) ApiResult {
	return a.sendACK(lightfx.CmdBatteryAutoCutoff(enabled))
}

// LightProgramSelect activates a program by 0-based index from the slave's
// loaded /lightfx.yaml. NACKs with ErrInvalidProgram if the slave has no
// config loaded or the index is past programCount.
func (a *LightFxApi) LightProgramSelect(index byte) ApiResult {
	return a.sendACK(lightfx.CmdLightProgramSelect(index))
}

// LightProgramReset stops sequences, retracts all groups, leaves no active program.
func (a *LightFxApi) LightProgramReset() ApiResult {
	return a.sendACK(lightfx.CmdLightProgramReset())
}
