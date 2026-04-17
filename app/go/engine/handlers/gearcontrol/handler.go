package gearcontrol

// ScaleFX Engine - GearControl Command Handler
// Commands for GearControl landing gear controller.

import (
	"fmt"
	"scalefx/engine"
	pcore "scalefx/protocol/core"
	gcp "scalefx/protocol/gearcontrol"
	"strings"
)

// Handler groups all GearControl commands and parsers.
type Handler struct {
	E *engine.Engine
}

// Register adds the GearControl command group, status parser, and async handlers to the engine.
func Register(eng *engine.Engine) {
	h := &Handler{E: eng}
	eng.RegisterStatusParser(pcore.CtrlGearControl, h.parseGearControlStatus)
	eng.RegisterAsyncHandler(gcp.GearCalibStatus, h.parseGearCalibStatus)
	eng.RegisterAsyncHandler(gcp.GearSeqStatus, h.parseGearSeqStatus)
	eng.RegisterAsyncHandler(gcp.GearDoorStatus, h.parseGearDoorStatus)
	eng.AddGroup(h.commands())
}

func (h *Handler) commands() *engine.CmdGroup {
	g := &engine.CmdGroup{
		Name:       "GearControl",
		Controller: pcore.CtrlGearControl,
		Color:      engine.ColorGreen,
		Commands: map[string]engine.CmdEntry{
			"deploy":           {h.cmdDeploy, "deploy <id> | all", "Deploy landing gear", true},
			"retract":          {h.cmdRetract, "retract <id> | all", "Retract landing gear", true},
			"stop":             {h.cmdStop, "stop <id> | all", "Emergency stop motor", true},
			"servo":            {h.cmdServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config":     {h.cmdServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec] [rev]", "Configure servo", true},
			"gear.config":      {h.cmdGearConfig, "gear.config <id> <flags> [stall_mA] [timeout_ms]", "Configure gear", true},
			"door.mode":        {h.cmdDoorMode, "door.mode <id> <pre> [post] [delay_ms]", "Set door activation mode", true},
			"yaw.config":       {h.cmdYawConfig, "yaw.config <id> <neutral> <min> <max>", "Configure yaw servo", true},
			"yaw":              {h.cmdYaw, "yaw <pulse_us>", "Set yaw position", true},
			"calibrate":        {h.cmdCalibrate, "calibrate <id> | all [timeout_s]", "Calibrate stall current", true},
			"calibrate.cancel": {h.cmdCalibCancel, "calibrate.cancel <id> | all", "Cancel calibration", true},
			"reset":            {h.cmdReset, "reset <id> | all", "Clear error state", true},
			"enable":           {h.cmdEnable, "enable <id> | all", "Enable gear channel", true},
			"disable":          {h.cmdDisable, "disable <id> | all", "Disable gear channel", true},
			"battery":          {h.cmdBattery, "battery on|off [autodeploy]", "Battery monitoring", true},
		},
	}
	for k, v := range h.E.ConfigCommands() {
		g.Commands[k] = v
	}
	return g
}

// ─── GearControl Command Handlers ───

func (h *Handler) cmdDeploy(args []string) {
	if !h.E.RequireArgs(args, 1, "deploy <gear_id> | all") {
		return
	}
	gc := h.E.API.GearControl
	if strings.ToLower(args[0]) == "all" {
		h.E.Ack(gc.AllDeploy(), "Deploy ALL gears")
	} else {
		id := engine.Atoi(args[0])
		h.E.Ack(gc.Deploy(byte(id)), fmt.Sprintf("Deploy gear %d (%s)", id, engine.GearIDName(byte(id))))
	}
}

func (h *Handler) cmdRetract(args []string) {
	if !h.E.RequireArgs(args, 1, "retract <gear_id> | all") {
		return
	}
	gc := h.E.API.GearControl
	if strings.ToLower(args[0]) == "all" {
		h.E.Ack(gc.AllRetract(), "Retract ALL gears")
	} else {
		id := engine.Atoi(args[0])
		h.E.Ack(gc.Retract(byte(id)), fmt.Sprintf("Retract gear %d (%s)", id, engine.GearIDName(byte(id))))
	}
}

func (h *Handler) cmdStop(args []string) {
	if !h.E.RequireArgs(args, 1, "stop <gear_id> | all") {
		return
	}
	gc := h.E.API.GearControl
	if strings.ToLower(args[0]) == "all" {
		h.E.Ack(gc.AllStop(), "STOP ALL motors")
	} else {
		id := engine.Atoi(args[0])
		h.E.Ack(gc.Stop(byte(id)), fmt.Sprintf("STOP motor %d", id))
	}
}

func (h *Handler) cmdServo(args []string) {
	h.E.ServoSet(args, "servo set <id> <pulse_us>", h.E.API.GearControl.ServoSet)
}

func (h *Handler) cmdServoConfig(args []string) {
	h.E.ServoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec] [rev]", h.E.API.GearControl.ServoConfig)
}

func (h *Handler) cmdGearConfig(args []string) {
	if !h.E.RequireArgs(args, 2, "gear.config <id> <flags> [stall_mA] [timeout_ms]") {
		return
	}
	id := engine.Atoi(args[0])
	flags := 0
	switch strings.ToLower(args[1]) {
	case "yaw":
		flags = 0x01
	case "none":
		flags = 0
	default:
		flags = engine.Atoi(args[1])
	}
	stall, timeout := 500, 60000
	if len(args) > 2 {
		stall = engine.Atoi(args[2])
	}
	if len(args) > 3 {
		timeout = engine.Atoi(args[3])
	}
	h.E.Ack(h.E.API.GearControl.GearConfig(byte(id), byte(flags), uint16(stall), uint16(timeout)),
		fmt.Sprintf("Gear %d: flags=0x%02X, stall=%dmA, timeout=%dms", id, flags, stall, timeout))
}

func (h *Handler) cmdDoorMode(args []string) {
	if !h.E.RequireArgs(args, 2, "door.mode <id> <pre_deploy> [post_deploy] [delay_ms]") {
		return
	}
	id := engine.Atoi(args[0])
	mode := parseDoorMode(args[1])
	postDeploy := byte(0)
	delay_ms := uint16(500)
	if len(args) > 2 {
		postDeploy = parseDoorMode(args[2])
	}
	if len(args) > 3 {
		delay_ms = uint16(engine.Atoi(args[3]))
	}
	h.E.Ack(h.E.API.GearControl.DoorMode(byte(id), mode, postDeploy, delay_ms),
		fmt.Sprintf("Gear %d: pre=%s post=%s", id, gcp.DoorModeName(mode), gcp.DoorModeName(postDeploy)))
}

func parseDoorMode(s string) byte {
	modeMap := map[string]byte{
		"none": 0, "single": 1, "dual-sync": 2, "sync": 2,
		"dual-delay": 3, "delay": 3, "dual-seq": 4, "seq": 4,
	}
	if v, ok := modeMap[strings.ToLower(s)]; ok {
		return v
	}
	return byte(engine.Atoi(s))
}

func (h *Handler) cmdYawConfig(args []string) {
	if !h.E.RequireArgs(args, 4, "yaw.config <gear_id> <neutral_us> <min_us> <max_us>") {
		return
	}
	id, neutral, min, max := engine.Atoi(args[0]), engine.Atoi(args[1]), engine.Atoi(args[2]), engine.Atoi(args[3])
	h.E.Ack(h.E.API.GearControl.YawConfig(byte(id), uint16(neutral), uint16(min), uint16(max)),
		fmt.Sprintf("Yaw gear %d: neutral=%dµs, range=%d-%dµs", id, neutral, min, max))
}

func (h *Handler) cmdYaw(args []string) {
	if !h.E.RequireArgs(args, 1, "yaw <pulse_us>") {
		return
	}
	pos := engine.Atoi(args[0])
	h.E.Ack(h.E.API.GearControl.YawInput(uint16(pos)), fmt.Sprintf("Yaw → %dµs", pos))
}

func (h *Handler) cmdCalibrate(args []string) {
	if !h.E.RequireArgs(args, 1, "calibrate <gear_id> | all [timeout_s]") {
		return
	}
	timeout := byte(0)
	if len(args) > 1 {
		timeout = byte(engine.Atoi(args[1]))
	}
	gc := h.E.API.GearControl
	h.forEachGear(args[0], func(id byte) {
		h.E.Ack(gc.Calibrate(id, timeout),
			fmt.Sprintf("Calibrating gear %d (%s)", id, engine.GearIDName(id)))
	})
}

func (h *Handler) cmdCalibCancel(args []string) {
	if !h.E.RequireArgs(args, 1, "calibrate.cancel <gear_id> | all") {
		return
	}
	gc := h.E.API.GearControl
	h.forEachGear(args[0], func(id byte) {
		h.E.Ack(gc.CalibrateCancel(id),
			fmt.Sprintf("Cancelled gear %d (%s)", id, engine.GearIDName(id)))
	})
}

func (h *Handler) cmdReset(args []string) {
	if !h.E.RequireArgs(args, 1, "reset <gear_id> | all") {
		return
	}
	gc := h.E.API.GearControl
	h.forEachGear(args[0], func(id byte) {
		h.E.Ack(gc.Reset(id), fmt.Sprintf("Reset gear %d (%s)", id, engine.GearIDName(id)))
	})
}

func (h *Handler) cmdEnable(args []string) {
	if !h.E.RequireArgs(args, 1, "enable <gear_id> | all") {
		return
	}
	gc := h.E.API.GearControl
	h.forEachGear(args[0], func(id byte) {
		h.E.Ack(gc.Enable(id, true), fmt.Sprintf("Enabled gear %d (%s)", id, engine.GearIDName(id)))
	})
}

func (h *Handler) cmdDisable(args []string) {
	if !h.E.RequireArgs(args, 1, "disable <gear_id> | all") {
		return
	}
	gc := h.E.API.GearControl
	h.forEachGear(args[0], func(id byte) {
		h.E.Ack(gc.Enable(id, false), fmt.Sprintf("Disabled gear %d (%s)", id, engine.GearIDName(id)))
	})
}

func (h *Handler) cmdBattery(args []string) {
	if !h.E.RequireArgs(args, 1, "battery on|off [autodeploy]") {
		return
	}
	enabled := engine.ParseBool(args[0])
	autoDeploy := len(args) > 1 && strings.ToLower(args[1]) == "autodeploy"
	state := "DISABLED"
	if enabled {
		state = "ENABLED"
		if autoDeploy {
			state += " + auto-deploy"
		}
	}
	h.E.Ack(h.E.API.GearControl.BatteryConfig(enabled, autoDeploy), fmt.Sprintf("Battery monitoring: %s", state))
}

// forEachGear applies fn to a single gear ID or all 3 gears.
func (h *Handler) forEachGear(arg string, fn func(id byte)) {
	if strings.ToLower(arg) == "all" {
		for id := byte(0); id < 3; id++ {
			fn(id)
		}
	} else {
		fn(byte(engine.Atoi(arg)))
	}
}


