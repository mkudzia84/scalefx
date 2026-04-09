package api

import (
	"scalefx/protocol"
	"scalefx/protocol/gunfx"
)

// GunFxApi provides GunFX weapon effects operations.
type GunFxApi struct{ apiClient }

func NewGunFxApi(conn *protocol.Connection) *GunFxApi {
	return &GunFxApi{apiClient{conn}}
}

func (a *GunFxApi) TriggerOn(rpm uint16) ApiResult      { return a.sendACK(gunfx.CmdTriggerOn(rpm)) }
func (a *GunFxApi) TriggerOff(delay_ms uint16) ApiResult { return a.sendACK(gunfx.CmdTriggerOff(delay_ms)) }

func (a *GunFxApi) ServoSet(id byte, pulse_us uint16) ApiResult {
	return a.sendACK(gunfx.CmdServoSet(id, pulse_us))
}

func (a *GunFxApi) ServoConfig(id byte, min, max, speed, accel, decel uint16, reversed bool) ApiResult {
	return a.sendACK(gunfx.CmdServoSettings(id, min, max, speed, accel, decel, reversed))
}

func (a *GunFxApi) ServoRecoil(id byte, jerk_us, variance_us uint16) ApiResult {
	return a.sendACK(gunfx.CmdServoRecoil(id, jerk_us, variance_us))
}

func (a *GunFxApi) SmokeHeat(on bool) ApiResult { return a.sendACK(gunfx.CmdSmokeHeat(on)) }

func (a *GunFxApi) SmokeConfig(pulsing bool, speed, high, low byte, pulse_ms, spindown_ms uint16) ApiResult {
	return a.sendACK(gunfx.CmdSmokeSettings(pulsing, speed, high, low, pulse_ms, spindown_ms))
}

func (a *GunFxApi) SmokeReset() ApiResult { return a.sendACK(gunfx.CmdSmokeReset()) }

func (a *GunFxApi) SmokeLimit(target byte, limit_mA uint16) ApiResult {
	return a.sendACK(gunfx.CmdSmokeCurrentLimit(target, limit_mA))
}
