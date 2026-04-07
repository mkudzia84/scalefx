package api

import (
	"scalefx/protocol"
	"scalefx/protocol/gearcontrol"
)

// GearControlApi provides GearControl landing gear operations.
type GearControlApi struct{ apiClient }

func NewGearControlApi(conn *protocol.Connection) *GearControlApi {
	return &GearControlApi{apiClient{conn}}
}

func (a *GearControlApi) Deploy(id byte) ApiResult  { return a.sendACK(gearcontrol.CmdDeploy(id)) }
func (a *GearControlApi) Retract(id byte) ApiResult { return a.sendACK(gearcontrol.CmdRetract(id)) }
func (a *GearControlApi) Stop(id byte) ApiResult    { return a.sendACK(gearcontrol.CmdStop(id)) }

func (a *GearControlApi) AllDeploy() ApiResult  { return a.sendACK(gearcontrol.CmdAll(1)) }
func (a *GearControlApi) AllRetract() ApiResult { return a.sendACK(gearcontrol.CmdAll(0)) }
func (a *GearControlApi) AllStop() ApiResult    { return a.sendACK(gearcontrol.CmdAll(2)) }

func (a *GearControlApi) ServoSet(id byte, pulse_us uint16) ApiResult {
	return a.sendACK(gearcontrol.CmdServoSet(id, pulse_us))
}

func (a *GearControlApi) ServoConfig(id byte, min, max, speed, accel, decel uint16) ApiResult {
	return a.sendACK(gearcontrol.CmdServoSettings(id, min, max, speed, accel, decel))
}

func (a *GearControlApi) GearConfig(id, flags byte, stall_mA, timeout_ms uint16) ApiResult {
	return a.sendACK(gearcontrol.CmdGearConfig(id, flags, stall_mA, timeout_ms))
}

func (a *GearControlApi) DoorConfig(id byte, o0, c0, o1, c1 uint16) ApiResult {
	return a.sendACK(gearcontrol.CmdDoorConfig(id, o0, c0, o1, c1))
}

func (a *GearControlApi) DoorMode(id, pre, post byte, delay_ms uint16) ApiResult {
	return a.sendACK(gearcontrol.CmdDoorMode(id, pre, post, delay_ms))
}

func (a *GearControlApi) YawConfig(id byte, neutral, min, max uint16) ApiResult {
	return a.sendACK(gearcontrol.CmdYawConfig(id, neutral, min, max))
}

func (a *GearControlApi) YawInput(pos uint16) ApiResult { return a.sendACK(gearcontrol.CmdYawInput(pos)) }

func (a *GearControlApi) Calibrate(id, timeout byte) ApiResult {
	return a.sendACK(gearcontrol.CmdCalibrate(id, timeout))
}

func (a *GearControlApi) CalibrateCancel(id byte) ApiResult      { return a.sendACK(gearcontrol.CmdCalibCancel(id)) }
func (a *GearControlApi) Reset(id byte) ApiResult                { return a.sendACK(gearcontrol.CmdReset(id)) }
func (a *GearControlApi) Enable(id byte, enabled bool) ApiResult { return a.sendACK(gearcontrol.CmdEnable(id, enabled)) }
func (a *GearControlApi) BatteryConfig(enabled, autoDeploy bool) ApiResult {
	return a.sendACK(gearcontrol.CmdBatteryConfig(enabled, autoDeploy))
}
