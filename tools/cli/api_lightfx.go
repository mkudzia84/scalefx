package main

// LightFxApi provides LightFX lighting effects operations.
type LightFxApi struct{ apiClient }

// NewLightFxApi creates a LightFxApi bound to the given connection.
func NewLightFxApi(conn *Connection) *LightFxApi {
	return &LightFxApi{apiClient{conn}}
}

func (a *LightFxApi) LedSet(ch, brightness byte) ApiResult { return a.sendACK(CmdLfxLedSet(ch, brightness)) }
func (a *LightFxApi) LedOff(ch byte) ApiResult             { return a.sendACK(CmdLfxLedOff(ch)) }
func (a *LightFxApi) LedStatus() ApiResult                 { return a.sendQuery(CmdLfxLedStatus(), LfxLED_STATUS_RESP) }

func (a *LightFxApi) SeqAdd(ch, eventType byte, p1, p2 uint16, p3, p4 byte) ApiResult {
	return a.sendACK(CmdLfxLedSeqAdd(ch, eventType, p1, p2, p3, p4))
}

func (a *LightFxApi) SeqClear(ch byte) ApiResult              { return a.sendACK(CmdLfxLedSeqClear(ch)) }
func (a *LightFxApi) SeqStart(ch byte, loops uint16) ApiResult { return a.sendACK(CmdLfxLedSeqStart(ch, loops)) }
func (a *LightFxApi) SeqStop(ch byte) ApiResult                { return a.sendACK(CmdLfxLedSeqStop(ch)) }
func (a *LightFxApi) SeqRestart(ch byte) ApiResult             { return a.sendACK(CmdLfxLedSeqRestart(ch)) }
func (a *LightFxApi) SeqStatus(ch byte) ApiResult              { return a.sendQuery(CmdLfxLedSeqStatus(ch), LfxLED_SEQ_STATUS_RESP) }
func (a *LightFxApi) SeqQueue(ch byte) ApiResult               { return a.sendQuery(CmdLfxLedSeqQueue(ch), LfxLED_SEQ_QUEUE_RESP) }
func (a *LightFxApi) MasterBrightness(val byte) ApiResult      { return a.sendACK(CmdLfxMasterBrightness(val)) }

func (a *LightFxApi) ServoSet(id byte, pulse_us uint16) ApiResult {
	return a.sendACK(CmdLfxServoSet(id, pulse_us))
}

func (a *LightFxApi) ServoConfig(id byte, min, max, speed, accel, decel uint16) ApiResult {
	return a.sendACK(CmdLfxServoSettings(id, min, max, speed, accel, decel))
}

func (a *LightFxApi) LandingBind(slot, servo, led byte, deploy, retract uint16, bright byte) ApiResult {
	return a.sendACK(CmdLfxLandingLightBind(slot, servo, led, deploy, retract, bright))
}

func (a *LightFxApi) LandingUnbind(slot byte) ApiResult  { return a.sendACK(CmdLfxLandingLightUnbind(slot)) }
func (a *LightFxApi) LandingDeploy(slot byte) ApiResult  { return a.sendACK(CmdLfxLandingLightDeploy(slot)) }
func (a *LightFxApi) LandingRetract(slot byte) ApiResult { return a.sendACK(CmdLfxLandingLightRetract(slot)) }
func (a *LightFxApi) Reset(ch byte) ApiResult            { return a.sendACK(CmdLfxLedReset(ch)) }
func (a *LightFxApi) Enable(ch byte, enabled bool) ApiResult { return a.sendACK(CmdLfxLedEnable(ch, enabled)) }
