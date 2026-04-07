package engine

// ScaleFX Engine - GearControl Command Handler
// Commands for GearControl landing gear controller.

import (
	"fmt"
	"scalefx/protocol/core"
	"scalefx/protocol/gearcontrol"
	"strings"
)

func (e *Engine) gearcontrolCommands() *CmdGroup {
	return &CmdGroup{
		Name:       "GearControl",
		Controller: core.CtrlGearControl,
		Color:      ColorGreen,
		Commands: map[string]CmdEntry{
			"deploy":           {e.cmdGcDeploy, "deploy <id> | all", "Deploy landing gear", true},
			"retract":          {e.cmdGcRetract, "retract <id> | all", "Retract landing gear", true},
			"stop":             {e.cmdGcStop, "stop <id> | all", "Emergency stop motor", true},
			"servo":            {e.cmdGcServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config":     {e.cmdGcServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec]", "Configure servo", true},
			"gear.config":      {e.cmdGcGearConfig, "gear.config <id> <flags> [stall_mA] [timeout_ms]", "Configure gear", true},
			"door.config":      {e.cmdGcDoorConfig, "door.config <id> <o0> <c0> <o1> <c1>", "Configure door servos", true},
			"door.mode":        {e.cmdGcDoorMode, "door.mode <id> <pre> [post] [delay_ms]", "Set door activation mode", true},
			"yaw.config":       {e.cmdGcYawConfig, "yaw.config <id> <neutral> <min> <max>", "Configure yaw servo", true},
			"yaw":              {e.cmdGcYaw, "yaw <pulse_us>", "Set yaw position", true},
			"calibrate":        {e.cmdGcCalibrate, "calibrate <id> | all [timeout_s]", "Calibrate stall current", true},
			"calibrate.cancel": {e.cmdGcCalibCancel, "calibrate.cancel <id> | all", "Cancel calibration", true},
			"reset":            {e.cmdGcReset, "reset <id> | all", "Clear error state", true},
			"enable":           {e.cmdGcEnable, "enable <id> | all", "Enable gear channel", true},
			"disable":          {e.cmdGcDisable, "disable <id> | all", "Disable gear channel", true},
			"battery":          {e.cmdGcBattery, "battery on|off [autodeploy]", "Battery monitoring", true},
		},
	}
}

// ─── GearControl Command Handlers ───

func (e *Engine) cmdGcDeploy(args []string) {
	if !e.requireArgs(args, 1, "deploy <gear_id> | all") {
		return
	}
	gc := e.API.GearControl
	if strings.ToLower(args[0]) == "all" {
		e.ack(gc.AllDeploy(), "Deploy ALL gears")
	} else {
		id := Atoi(args[0])
		e.ack(gc.Deploy(byte(id)), fmt.Sprintf("Deploy gear %d (%s)", id, GearIDName(byte(id))))
	}
}

func (e *Engine) cmdGcRetract(args []string) {
	if !e.requireArgs(args, 1, "retract <gear_id> | all") {
		return
	}
	gc := e.API.GearControl
	if strings.ToLower(args[0]) == "all" {
		e.ack(gc.AllRetract(), "Retract ALL gears")
	} else {
		id := Atoi(args[0])
		e.ack(gc.Retract(byte(id)), fmt.Sprintf("Retract gear %d (%s)", id, GearIDName(byte(id))))
	}
}

func (e *Engine) cmdGcStop(args []string) {
	if !e.requireArgs(args, 1, "stop <gear_id> | all") {
		return
	}
	gc := e.API.GearControl
	if strings.ToLower(args[0]) == "all" {
		e.ack(gc.AllStop(), "STOP ALL motors")
	} else {
		id := Atoi(args[0])
		e.ack(gc.Stop(byte(id)), fmt.Sprintf("STOP motor %d", id))
	}
}

// Servo commands delegate to shared helpers (Strategy pattern)
func (e *Engine) cmdGcServo(args []string) {
	e.servoSet(args, "servo set <id> <pulse_us>", e.API.GearControl.ServoSet)
}

func (e *Engine) cmdGcServoConfig(args []string) {
	e.servoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec]", e.API.GearControl.ServoConfig)
}

func (e *Engine) cmdGcGearConfig(args []string) {
	if !e.requireArgs(args, 2, "gear.config <id> <flags> [stall_mA] [timeout_ms]") {
		return
	}
	id := Atoi(args[0])
	flags := 0
	switch strings.ToLower(args[1]) {
	case "yaw":
		flags = 0x01
	case "none":
		flags = 0
	default:
		flags = Atoi(args[1])
	}
	stall, timeout := 500, 60000
	if len(args) > 2 {
		stall = Atoi(args[2])
	}
	if len(args) > 3 {
		timeout = Atoi(args[3])
	}
	e.ack(e.API.GearControl.GearConfig(byte(id), byte(flags), uint16(stall), uint16(timeout)),
		fmt.Sprintf("Gear %d: flags=0x%02X, stall=%dmA, timeout=%dms", id, flags, stall, timeout))
}

func (e *Engine) cmdGcDoorConfig(args []string) {
	if !e.requireArgs(args, 5, "door.config <id> <open0_us> <close0_us> <open1_us> <close1_us>") {
		return
	}
	id := Atoi(args[0])
	o0, c0, o1, c1 := Atoi(args[1]), Atoi(args[2]), Atoi(args[3]), Atoi(args[4])
	e.ack(e.API.GearControl.DoorConfig(byte(id), uint16(o0), uint16(c0), uint16(o1), uint16(c1)),
		fmt.Sprintf("Gear %d doors: A=%d-%dµs, B=%d-%dµs", id, c0, o0, c1, o1))
}

func (e *Engine) cmdGcDoorMode(args []string) {
	if !e.requireArgs(args, 2, "door.mode <id> <pre_deploy> [post_deploy] [delay_ms]") {
		return
	}
	id := Atoi(args[0])
	mode := parseDoorMode(args[1])
	postDeploy := byte(0)
	delay_ms := uint16(500)
	if len(args) > 2 {
		postDeploy = parseDoorMode(args[2])
	}
	if len(args) > 3 {
		delay_ms = uint16(Atoi(args[3]))
	}
	e.ack(e.API.GearControl.DoorMode(byte(id), mode, postDeploy, delay_ms),
		fmt.Sprintf("Gear %d: pre=%s post=%s", id, gearcontrol.DoorModeName(mode), gearcontrol.DoorModeName(postDeploy)))
}

func parseDoorMode(s string) byte {
	modeMap := map[string]byte{
		"none": 0, "single": 1, "dual-sync": 2, "sync": 2,
		"dual-delay": 3, "delay": 3, "dual-seq": 4, "seq": 4,
	}
	if v, ok := modeMap[strings.ToLower(s)]; ok {
		return v
	}
	return byte(Atoi(s))
}

func (e *Engine) cmdGcYawConfig(args []string) {
	if !e.requireArgs(args, 4, "yaw.config <gear_id> <neutral_us> <min_us> <max_us>") {
		return
	}
	id, neutral, min, max := Atoi(args[0]), Atoi(args[1]), Atoi(args[2]), Atoi(args[3])
	e.ack(e.API.GearControl.YawConfig(byte(id), uint16(neutral), uint16(min), uint16(max)),
		fmt.Sprintf("Yaw gear %d: neutral=%dµs, range=%d-%dµs", id, neutral, min, max))
}

func (e *Engine) cmdGcYaw(args []string) {
	if !e.requireArgs(args, 1, "yaw <pulse_us>") {
		return
	}
	pos := Atoi(args[0])
	e.ack(e.API.GearControl.YawInput(uint16(pos)), fmt.Sprintf("Yaw → %dµs", pos))
}

func (e *Engine) cmdGcCalibrate(args []string) {
	if !e.requireArgs(args, 1, "calibrate <gear_id> | all [timeout_s]") {
		return
	}
	timeout := byte(0)
	if len(args) > 1 {
		timeout = byte(Atoi(args[1]))
	}
	gc := e.API.GearControl
	e.forEachGear(args[0], func(id byte) {
		e.ack(gc.Calibrate(id, timeout),
			fmt.Sprintf("Calibrating gear %d (%s)", id, GearIDName(id)))
	})
}

func (e *Engine) cmdGcCalibCancel(args []string) {
	if !e.requireArgs(args, 1, "calibrate.cancel <gear_id> | all") {
		return
	}
	gc := e.API.GearControl
	e.forEachGear(args[0], func(id byte) {
		e.ack(gc.CalibrateCancel(id),
			fmt.Sprintf("Cancelled gear %d (%s)", id, GearIDName(id)))
	})
}

func (e *Engine) cmdGcReset(args []string) {
	if !e.requireArgs(args, 1, "reset <gear_id> | all") {
		return
	}
	gc := e.API.GearControl
	e.forEachGear(args[0], func(id byte) {
		e.ack(gc.Reset(id), fmt.Sprintf("Reset gear %d (%s)", id, GearIDName(id)))
	})
}

func (e *Engine) cmdGcEnable(args []string) {
	if !e.requireArgs(args, 1, "enable <gear_id> | all") {
		return
	}
	gc := e.API.GearControl
	e.forEachGear(args[0], func(id byte) {
		e.ack(gc.Enable(id, true), fmt.Sprintf("Enabled gear %d (%s)", id, GearIDName(id)))
	})
}

func (e *Engine) cmdGcDisable(args []string) {
	if !e.requireArgs(args, 1, "disable <gear_id> | all") {
		return
	}
	gc := e.API.GearControl
	e.forEachGear(args[0], func(id byte) {
		e.ack(gc.Enable(id, false), fmt.Sprintf("Disabled gear %d (%s)", id, GearIDName(id)))
	})
}

func (e *Engine) cmdGcBattery(args []string) {
	if !e.requireArgs(args, 1, "battery on|off [autodeploy]") {
		return
	}
	enabled := ParseBool(args[0])
	autoDeploy := len(args) > 1 && strings.ToLower(args[1]) == "autodeploy"
	state := "DISABLED"
	if enabled {
		state = "ENABLED"
		if autoDeploy {
			state += " + auto-deploy"
		}
	}
	e.ack(e.API.GearControl.BatteryConfig(enabled, autoDeploy), fmt.Sprintf("Battery monitoring: %s", state))
}

// forEachGear applies fn to a single gear ID or all 3 gears.
func (e *Engine) forEachGear(arg string, fn func(id byte)) {
	if strings.ToLower(arg) == "all" {
		for id := byte(0); id < 3; id++ {
			fn(id)
		}
	} else {
		fn(byte(Atoi(arg)))
	}
}
