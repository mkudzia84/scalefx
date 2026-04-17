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

// Handler groups all GearControl commands, decoders, and parsers.
//
// Each async event has a multicast Observers[T] set. Register() appends the
// built-in CLI console formatter to the event listener sets that are meant to
// print (CalibStatus, SeqStatus, DoorStatus). Broadcasts stay silent by
// default because the synchronous `status` command is the CLI entry point.
// External consumers (Studio) append their own listeners via .Add().
type Handler struct {
	E *engine.Engine

	OnStatusBroadcast engine.Observers[StatusBroadcast] // periodic STATUS_BROADCAST (~1 Hz)
	OnCalibStatus     engine.Observers[CalibStatus]     // GEAR_CALIB_STATUS (0x6B)
	OnSeqStatus       engine.Observers[SeqStatus]       // GEAR_SEQ_STATUS (0x6C)
	OnDoorStatus      engine.Observers[DoorStatus]      // GEAR_DOOR_STATUS (0x6D)
}

// Register adds the GearControl command group, status parser, and async
// handlers. Returns the Handler so external consumers can install listeners.
func Register(eng *engine.Engine) *Handler {
	h := &Handler{E: eng}
	h.OnCalibStatus.Add(h.FormatCalibStatus)
	h.OnSeqStatus.Add(h.FormatSeqStatus)
	h.OnDoorStatus.Add(h.FormatDoorStatus)

	eng.RegisterStatusParser(pcore.CtrlGearControl, func(data []byte) {
		if s := DecodeStatusBroadcast(data); s != nil {
			h.FormatStatusBroadcast(s)
		} else {
			h.E.Out.Printf("  GearControl: (incomplete: %d bytes)\n", len(data))
		}
	})
	eng.RegisterStatusBroadcastParser(pcore.CtrlGearControl, func(data []byte) {
		if h.OnStatusBroadcast.Len() == 0 {
			return
		}
		if s := DecodeStatusBroadcast(data); s != nil {
			h.OnStatusBroadcast.Fire(s)
		}
	})
	eng.RegisterAsyncHandler(gcp.GearCalibStatus, func(p []byte) {
		h.OnCalibStatus.Dispatch(h.E.Out, "CalibStatus", p, DecodeCalibStatus)
	})
	eng.RegisterAsyncHandler(gcp.GearSeqStatus, func(p []byte) {
		h.OnSeqStatus.Dispatch(h.E.Out, "SeqStatus", p, DecodeSeqStatus)
	})
	eng.RegisterAsyncHandler(gcp.GearDoorStatus, func(p []byte) {
		h.OnDoorStatus.Dispatch(h.E.Out, "DoorStatus", p, DecodeDoorStatus)
	})
	eng.AddGroup(h.commands())
	return h
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
			"battery":          {h.cmdBattery, "battery [autodeploy] [lipo|liion|nimh] [cells:N|auto]", "Battery profile — monitor is always on; no args = LiPo, auto cells", true},
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

// cmdBattery accepts any mix of:
//
//	autodeploy                  (enables auto-deploy on low voltage)
//	lipo | liion | nimh         (chemistry; defaults to LiPo)
//	cells:N | cells=N | auto    (cell count, 1-6 or auto-detect; defaults to auto)
//
// Bare `battery` → LiPo + auto-detect cells. Monitor is always on.
func (h *Handler) cmdBattery(args []string) {
	autoDeploy := false
	chemistry := gcp.ChemistryLiPo
	cellCount := byte(0) // 0 = auto-detect

	for _, raw := range args {
		a := strings.ToLower(raw)
		switch {
		case a == "autodeploy" || a == "auto-deploy":
			autoDeploy = true
		case a == "lipo":
			chemistry = gcp.ChemistryLiPo
		case a == "liion" || a == "li-ion":
			chemistry = gcp.ChemistryLiIon
		case a == "nimh" || a == "ni-mh":
			chemistry = gcp.ChemistryNiMH
		case a == "auto" || a == "autodetect" || a == "cells:auto" || a == "cells=auto":
			cellCount = 0
		case strings.HasPrefix(a, "cells:") || strings.HasPrefix(a, "cells="):
			n := engine.Atoi(a[6:])
			if n < 0 || n > 6 {
				h.E.Out.Error("cells must be 0..6 (got %d)", n)
				return
			}
			cellCount = byte(n)
		default:
			h.E.Out.Error("Unknown battery arg: %s", raw)
			return
		}
	}

	cellLabel := "auto"
	if cellCount > 0 {
		cellLabel = fmt.Sprintf("%dS", cellCount)
	}
	autoLabel := ""
	if autoDeploy {
		autoLabel = " + auto-deploy"
	}
	summary := fmt.Sprintf("Battery: %s | cells=%s%s", chemistryLabel(chemistry), cellLabel, autoLabel)
	h.E.Ack(h.E.API.GearControl.BatteryConfig(autoDeploy, chemistry, cellCount), summary)
}

func chemistryLabel(c byte) string {
	switch c {
	case gcp.ChemistryLiIon:
		return "Li-Ion"
	case gcp.ChemistryNiMH:
		return "NiMH"
	default:
		return "LiPo"
	}
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


