package main

// GunFxApi provides GunFX weapon effects operations.
type GunFxApi struct{ apiClient }

// NewGunFxApi creates a GunFxApi bound to the given connection.
func NewGunFxApi(conn *Connection) *GunFxApi {
	return &GunFxApi{apiClient{conn}}
}

func (a *GunFxApi) TriggerOn(rpm uint16) ApiResult      { return a.sendACK(CmdGfxTriggerOn(rpm)) }
func (a *GunFxApi) TriggerOff(delay_ms uint16) ApiResult { return a.sendACK(CmdGfxTriggerOff(delay_ms)) }

func (a *GunFxApi) ServoSet(id byte, pulse_us uint16) ApiResult {
	return a.sendACK(CmdGfxServoSet(id, pulse_us))
}

func (a *GunFxApi) ServoConfig(id byte, min, max, speed, accel, decel uint16) ApiResult {
	return a.sendACK(CmdGfxServoSettings(id, min, max, speed, accel, decel))
}

func (a *GunFxApi) ServoRecoil(id byte, jerk_us, variance_us uint16) ApiResult {
	return a.sendACK(CmdGfxServoRecoil(id, jerk_us, variance_us))
}

func (a *GunFxApi) SmokeHeat(on bool) ApiResult { return a.sendACK(CmdGfxSmokeHeat(on)) }

func (a *GunFxApi) SmokeConfig(pulsing bool, speed, high, low byte, pulse_ms, spindown_ms uint16) ApiResult {
	return a.sendACK(CmdGfxSmokeSettings(pulsing, speed, high, low, pulse_ms, spindown_ms))
}

func (a *GunFxApi) SmokeReset() ApiResult { return a.sendACK(CmdGfxSmokeReset()) }

func (a *GunFxApi) SmokeLimit(target byte, limit_mA uint16) ApiResult {
	return a.sendACK(CmdGfxSmokeCurrentLimit(target, limit_mA))
}
