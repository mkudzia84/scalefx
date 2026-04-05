package main

// GearControlApi provides GearControl landing gear operations.
type GearControlApi struct{ apiClient }

// NewGearControlApi creates a GearControlApi bound to the given connection.
func NewGearControlApi(conn *Connection) *GearControlApi {
	return &GearControlApi{apiClient{conn}}
}

func (a *GearControlApi) Deploy(id byte) ApiResult  { return a.sendACK(CmdGcDeploy(id)) }
func (a *GearControlApi) Retract(id byte) ApiResult { return a.sendACK(CmdGcRetract(id)) }
func (a *GearControlApi) Stop(id byte) ApiResult    { return a.sendACK(CmdGcStop(id)) }

func (a *GearControlApi) AllDeploy() ApiResult  { return a.sendACK(CmdGcAll(1)) }
func (a *GearControlApi) AllRetract() ApiResult { return a.sendACK(CmdGcAll(0)) }
func (a *GearControlApi) AllStop() ApiResult    { return a.sendACK(CmdGcAll(2)) }

func (a *GearControlApi) ServoSet(id byte, pulse_us uint16) ApiResult {
	return a.sendACK(CmdGcServoSet(id, pulse_us))
}

func (a *GearControlApi) ServoConfig(id byte, min, max, speed, accel, decel uint16) ApiResult {
	return a.sendACK(CmdGcServoSettings(id, min, max, speed, accel, decel))
}

func (a *GearControlApi) GearConfig(id, flags byte, stall_mA, timeout_ms uint16) ApiResult {
	return a.sendACK(CmdGcGearConfig(id, flags, stall_mA, timeout_ms))
}

func (a *GearControlApi) DoorConfig(id byte, o0, c0, o1, c1 uint16) ApiResult {
	return a.sendACK(CmdGcDoorConfig(id, o0, c0, o1, c1))
}

func (a *GearControlApi) DoorMode(id, pre, post byte, delay_ms uint16) ApiResult {
	return a.sendACK(CmdGcDoorMode(id, pre, post, delay_ms))
}

func (a *GearControlApi) YawConfig(id byte, neutral, min, max uint16) ApiResult {
	return a.sendACK(CmdGcYawConfig(id, neutral, min, max))
}

func (a *GearControlApi) YawInput(pos uint16) ApiResult { return a.sendACK(CmdGcYawInput(pos)) }

func (a *GearControlApi) Calibrate(id, timeout byte) ApiResult {
	return a.sendACK(CmdGcCalibrate(id, timeout))
}

func (a *GearControlApi) CalibrateCancel(id byte) ApiResult         { return a.sendACK(CmdGcCalibCancel(id)) }
func (a *GearControlApi) Reset(id byte) ApiResult                   { return a.sendACK(CmdGcReset(id)) }
func (a *GearControlApi) Enable(id byte, enabled bool) ApiResult    { return a.sendACK(CmdGcEnable(id, enabled)) }
func (a *GearControlApi) BatteryConfig(enabled, autoDeploy bool) ApiResult {
	return a.sendACK(CmdGcBatteryConfig(enabled, autoDeploy))
}
