package main

// ScaleFX CLI - GearControl Command Handler
// Commands for GearControl landing gear controller.

import (
	"fmt"
	"strings"
)

var gearNames = map[int]string{0: "nose", 1: "left main", 2: "right main"}

func (c *CLI) gearcontrolCommands() *cmdGroup {
	return &cmdGroup{
		Name:       "GearControl",
		Controller: CtrlGearControl,
		Color:      colorGreen,
		Commands: map[string]cmdEntry{
			"deploy":           {c.cmdGcDeploy, "deploy <id> | all", "Deploy landing gear", true},
			"retract":          {c.cmdGcRetract, "retract <id> | all", "Retract landing gear", true},
			"stop":             {c.cmdGcStop, "stop <id> | all", "Emergency stop motor", true},
			"servo":            {c.cmdGcServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config":     {c.cmdGcServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec]", "Configure servo", true},
			"gear.config":      {c.cmdGcGearConfig, "gear.config <id> <flags> [stall_mA] [timeout_ms]", "Configure gear", true},
			"door.config":      {c.cmdGcDoorConfig, "door.config <id> <o0> <c0> <o1> <c1>", "Configure door servos", true},
			"door.mode":        {c.cmdGcDoorMode, "door.mode <id> <pre> [post] [delay_ms]", "Set door activation mode", true},
			"yaw.config":       {c.cmdGcYawConfig, "yaw.config <id> <neutral> <min> <max>", "Configure yaw servo", true},
			"yaw":              {c.cmdGcYaw, "yaw <pulse_us>", "Set yaw position", true},
			"calibrate":        {c.cmdGcCalibrate, "calibrate <id> | all [timeout_s]", "Calibrate stall current", true},
			"calibrate.cancel": {c.cmdGcCalibCancel, "calibrate.cancel <id> | all", "Cancel calibration", true},
			"reset":            {c.cmdGcReset, "reset <id> | all", "Clear error state", true},
			"enable":           {c.cmdGcEnable, "enable <id> | all", "Enable gear channel", true},
			"disable":          {c.cmdGcDisable, "disable <id> | all", "Disable gear channel", true},
			"battery":          {c.cmdGcBattery, "battery on|off [autodeploy]", "Battery monitoring", true},
		},
	}
}

// ─── GearControl Command Handlers ───

func (c *CLI) cmdGcDeploy(args []string) {
	if !requireArgs(args, 1, "deploy <gear_id> | all") {
		return
	}
	api := NewGearControlApi(c.conn)
	if strings.ToLower(args[0]) == "all" {
		c.ack(api.AllDeploy(), "Deploy ALL gears")
	} else {
		id := atoi(args[0])
		c.ack(api.Deploy(byte(id)), fmt.Sprintf("Deploy gear %d (%s)", id, gearNames[id]))
	}
}

func (c *CLI) cmdGcRetract(args []string) {
	if !requireArgs(args, 1, "retract <gear_id> | all") {
		return
	}
	api := NewGearControlApi(c.conn)
	if strings.ToLower(args[0]) == "all" {
		c.ack(api.AllRetract(), "Retract ALL gears")
	} else {
		id := atoi(args[0])
		c.ack(api.Retract(byte(id)), fmt.Sprintf("Retract gear %d (%s)", id, gearNames[id]))
	}
}

func (c *CLI) cmdGcStop(args []string) {
	if !requireArgs(args, 1, "stop <gear_id> | all") {
		return
	}
	api := NewGearControlApi(c.conn)
	if strings.ToLower(args[0]) == "all" {
		c.ack(api.AllStop(), "STOP ALL motors")
	} else {
		id := atoi(args[0])
		c.ack(api.Stop(byte(id)), fmt.Sprintf("STOP motor %d", id))
	}
}

// Servo commands delegate to shared helpers (Strategy pattern)
func (c *CLI) cmdGcServo(args []string) {
	c.servoSet(args, "servo set <id> <pulse_us>", NewGearControlApi(c.conn).ServoSet)
}

func (c *CLI) cmdGcServoConfig(args []string) {
	c.servoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec]", NewGearControlApi(c.conn).ServoConfig)
}

func (c *CLI) cmdGcGearConfig(args []string) {
	if !requireArgs(args, 2, "gear.config <id> <flags> [stall_mA] [timeout_ms]") {
		return
	}
	id := atoi(args[0])
	flags := 0
	switch strings.ToLower(args[1]) {
	case "yaw":
		flags = 0x01
	case "none":
		flags = 0
	default:
		flags = atoi(args[1])
	}
	stall, timeout := 500, 60000
	if len(args) > 2 {
		stall = atoi(args[2])
	}
	if len(args) > 3 {
		timeout = atoi(args[3])
	}
	c.ack(NewGearControlApi(c.conn).GearConfig(byte(id), byte(flags), uint16(stall), uint16(timeout)),
		fmt.Sprintf("Gear %d: flags=0x%02X, stall=%dmA, timeout=%dms", id, flags, stall, timeout))
}

func (c *CLI) cmdGcDoorConfig(args []string) {
	if !requireArgs(args, 5, "door.config <id> <open0_us> <close0_us> <open1_us> <close1_us>") {
		return
	}
	id := atoi(args[0])
	o0, c0, o1, c1 := atoi(args[1]), atoi(args[2]), atoi(args[3]), atoi(args[4])
	c.ack(NewGearControlApi(c.conn).DoorConfig(byte(id), uint16(o0), uint16(c0), uint16(o1), uint16(c1)),
		fmt.Sprintf("Gear %d doors: A=%d-%dµs, B=%d-%dµs", id, c0, o0, c1, o1))
}

func (c *CLI) cmdGcDoorMode(args []string) {
	if !requireArgs(args, 2, "door.mode <id> <pre_deploy> [post_deploy] [delay_ms]") {
		return
	}
	id := atoi(args[0])
	mode := parseDoorMode(args[1])
	postDeploy := byte(0)
	delay_ms := uint16(500)
	if len(args) > 2 {
		postDeploy = parseDoorMode(args[2])
	}
	if len(args) > 3 {
		delay_ms = uint16(atoi(args[3]))
	}
	c.ack(NewGearControlApi(c.conn).DoorMode(byte(id), mode, postDeploy, delay_ms),
		fmt.Sprintf("Gear %d: pre=%s post=%s", id, DoorModeName(mode), DoorModeName(postDeploy)))
}

func parseDoorMode(s string) byte {
	modeMap := map[string]byte{
		"none": 0, "single": 1, "dual-sync": 2, "sync": 2,
		"dual-delay": 3, "delay": 3, "dual-seq": 4, "seq": 4,
	}
	if v, ok := modeMap[strings.ToLower(s)]; ok {
		return v
	}
	return byte(atoi(s))
}

func (c *CLI) cmdGcYawConfig(args []string) {
	if !requireArgs(args, 4, "yaw.config <gear_id> <neutral_us> <min_us> <max_us>") {
		return
	}
	id, neutral, min, max := atoi(args[0]), atoi(args[1]), atoi(args[2]), atoi(args[3])
	c.ack(NewGearControlApi(c.conn).YawConfig(byte(id), uint16(neutral), uint16(min), uint16(max)),
		fmt.Sprintf("Yaw gear %d: neutral=%dµs, range=%d-%dµs", id, neutral, min, max))
}

func (c *CLI) cmdGcYaw(args []string) {
	if !requireArgs(args, 1, "yaw <pulse_us>") {
		return
	}
	pos := atoi(args[0])
	c.ack(NewGearControlApi(c.conn).YawInput(uint16(pos)), fmt.Sprintf("Yaw → %dµs", pos))
}

func (c *CLI) cmdGcCalibrate(args []string) {
	if !requireArgs(args, 1, "calibrate <gear_id> | all [timeout_s]") {
		return
	}
	timeout := byte(0)
	if len(args) > 1 {
		timeout = byte(atoi(args[1]))
	}
	api := NewGearControlApi(c.conn)
	c.forEachGear(args[0], func(id byte) {
		c.ack(api.Calibrate(id, timeout),
			fmt.Sprintf("Calibrating gear %d (%s)", id, gearNames[int(id)]))
	})
}

func (c *CLI) cmdGcCalibCancel(args []string) {
	if !requireArgs(args, 1, "calibrate.cancel <gear_id> | all") {
		return
	}
	api := NewGearControlApi(c.conn)
	c.forEachGear(args[0], func(id byte) {
		c.ack(api.CalibrateCancel(id),
			fmt.Sprintf("Cancelled gear %d (%s)", id, gearNames[int(id)]))
	})
}

func (c *CLI) cmdGcReset(args []string) {
	if !requireArgs(args, 1, "reset <gear_id> | all") {
		return
	}
	api := NewGearControlApi(c.conn)
	c.forEachGear(args[0], func(id byte) {
		c.ack(api.Reset(id), fmt.Sprintf("Reset gear %d (%s)", id, gearNames[int(id)]))
	})
}

func (c *CLI) cmdGcEnable(args []string) {
	if !requireArgs(args, 1, "enable <gear_id> | all") {
		return
	}
	api := NewGearControlApi(c.conn)
	c.forEachGear(args[0], func(id byte) {
		c.ack(api.Enable(id, true), fmt.Sprintf("Enabled gear %d (%s)", id, gearNames[int(id)]))
	})
}

func (c *CLI) cmdGcDisable(args []string) {
	if !requireArgs(args, 1, "disable <gear_id> | all") {
		return
	}
	api := NewGearControlApi(c.conn)
	c.forEachGear(args[0], func(id byte) {
		c.ack(api.Enable(id, false), fmt.Sprintf("Disabled gear %d (%s)", id, gearNames[int(id)]))
	})
}

func (c *CLI) cmdGcBattery(args []string) {
	if !requireArgs(args, 1, "battery on|off [autodeploy]") {
		return
	}
	enabled := parseBool(args[0])
	autoDeploy := len(args) > 1 && strings.ToLower(args[1]) == "autodeploy"
	state := "DISABLED"
	if enabled {
		state = "ENABLED"
		if autoDeploy {
			state += " + auto-deploy"
		}
	}
	c.ack(NewGearControlApi(c.conn).BatteryConfig(enabled, autoDeploy), fmt.Sprintf("Battery monitoring: %s", state))
}

// forEachGear applies fn to a single gear ID or all 3 gears.
func (c *CLI) forEachGear(arg string, fn func(id byte)) {
	if strings.ToLower(arg) == "all" {
		for id := byte(0); id < 3; id++ {
			fn(id)
		}
	} else {
		fn(byte(atoi(arg)))
	}
}
