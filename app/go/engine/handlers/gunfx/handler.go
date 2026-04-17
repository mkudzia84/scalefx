package gunfx

// ScaleFX Engine - GunFX Command Handler + Parsers
// Commands and status parsing for GunFX weapon effects controller.

import (
	"fmt"
	"scalefx/engine"
	pcore "scalefx/protocol/core"
	"strings"
)

// Handler groups all GunFX commands, decoders, and parsers.
//
// Broadcast observers are silent by default — the CLI prints via the
// synchronous `status` command path. Studio subscribes by calling
// handler.OnStatusBroadcast.Add(fn).
type Handler struct {
	E *engine.Engine

	OnStatusBroadcast engine.Observers[StatusBroadcast] // periodic STATUS_BROADCAST
}

// Register adds the GunFX command group and status parser to the engine.
// Returns the Handler so external consumers can install listeners.
func Register(eng *engine.Engine) *Handler {
	h := &Handler{E: eng}
	eng.RegisterStatusParser(pcore.CtrlGunFX, func(data []byte) {
		if s := DecodeStatusBroadcast(data); s != nil {
			h.FormatStatusBroadcast(s)
		} else {
			h.E.Out.Printf("  GunFX: (incomplete: %d bytes)\n", len(data))
		}
	})
	eng.RegisterStatusBroadcastParser(pcore.CtrlGunFX, func(data []byte) {
		if h.OnStatusBroadcast.Len() == 0 {
			return
		}
		if s := DecodeStatusBroadcast(data); s != nil {
			h.OnStatusBroadcast.Fire(s)
		}
	})
	eng.AddGroup(h.commands())
	return h
}

func (h *Handler) commands() *engine.CmdGroup {
	return &engine.CmdGroup{
		Name:       "GunFX",
		Controller: pcore.CtrlGunFX,
		Color:      engine.ColorRed,
		Commands: map[string]engine.CmdEntry{
			"trigger":      {h.cmdTrigger, "trigger on <rpm> | off [delay_ms]", "Control firing", true},
			"servo":        {h.cmdServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config": {h.cmdServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec] [rev]", "Configure servo", true},
			"servo.recoil": {h.cmdServoRecoil, "servo.recoil <id> <jerk_us> <variance_us>", "Configure recoil", true},
			"smoke":        {h.cmdSmoke, "smoke heat on|off", "Control smoke heater", true},
			"smoke.config": {h.cmdSmokeConfig, "smoke.config [key=value ...]", "Configure smoke fan", true},
			"smoke.reset":  {h.cmdSmokeReset, "smoke.reset", "Clear smoke errors", true},
			"smoke.limit":  {h.cmdSmokeLimit, "smoke.limit heater|fan <mA>", "Set overcurrent limit", true},
		},
	}
}

// ─── GunFX Command Handlers ───

func (h *Handler) cmdTrigger(args []string) {
	if !h.E.RequireArgs(args, 1, "trigger on <rpm> | off [delay_ms]") {
		return
	}
	gfx := h.E.API.GunFx
	switch strings.ToLower(args[0]) {
	case "on":
		if !h.E.RequireArgs(args, 2, "trigger on <rpm>") {
			return
		}
		rpm := engine.Atoi(args[1])
		h.E.Ack(gfx.TriggerOn(uint16(rpm)), fmt.Sprintf("Trigger ON at %d RPM", rpm))
	case "off":
		delay := 3000
		if len(args) > 1 {
			delay = engine.Atoi(args[1])
		}
		h.E.Ack(gfx.TriggerOff(uint16(delay)), fmt.Sprintf("Trigger OFF (spin-down: %dms)", delay))
	default:
		h.E.Out.Error("Use 'on' or 'off'")
	}
}

func (h *Handler) cmdServo(args []string) {
	h.E.ServoSet(args, "servo set <id> <pulse_us>", h.E.API.GunFx.ServoSet)
}

func (h *Handler) cmdServoConfig(args []string) {
	h.E.ServoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec] [rev]", h.E.API.GunFx.ServoConfig)
}

func (h *Handler) cmdServoRecoil(args []string) {
	if !h.E.RequireArgs(args, 3, "servo.recoil <id> <jerk_us> <variance_us>") {
		return
	}
	id, jerk, variance := engine.Atoi(args[0]), engine.Atoi(args[1]), engine.Atoi(args[2])
	h.E.Ack(h.E.API.GunFx.ServoRecoil(byte(id), uint16(jerk), uint16(variance)),
		fmt.Sprintf("Servo %d recoil: jerk=%dµs, variance=±%dµs", id, jerk, variance))
}

func (h *Handler) cmdSmoke(args []string) {
	if !h.E.RequireArgs(args, 2, "smoke heat on|off") {
		return
	}
	if strings.ToLower(args[0]) != "heat" {
		h.E.Out.Error("Usage: smoke heat on|off")
		return
	}
	on := engine.ParseBool(args[1])
	h.E.Ack(h.E.API.GunFx.SmokeHeat(on), fmt.Sprintf("Smoke heater %s", engine.OnOff(on)))
}

func (h *Handler) cmdSmokeConfig(args []string) {
	if len(args) == 0 {
		h.E.Out.Error("Usage: smoke.config [key=value ...]")
		h.E.Out.Info("  Keys: pulsing (0|1), speed (0-255), high, low, pulse_ms, spindown_ms")
		return
	}
	pulsing := false
	speed, high, low := byte(255), byte(255), byte(80)
	pulse_ms, spindown_ms := uint16(0), uint16(5000)
	for _, arg := range args {
		kv := strings.SplitN(arg, "=", 2)
		if len(kv) != 2 {
			h.E.Out.Error("Invalid: %s (use key=value)", arg)
			return
		}
		k, v := strings.ToLower(kv[0]), kv[1]
		switch k {
		case "pulsing":
			pulsing = engine.ParseBool(v)
		case "speed":
			speed = byte(engine.Atoi(v))
		case "high":
			high = byte(engine.Atoi(v))
		case "low":
			low = byte(engine.Atoi(v))
		case "pulse_ms", "pulse":
			pulse_ms = uint16(engine.Atoi(v))
		case "spindown_ms", "spindown":
			spindown_ms = uint16(engine.Atoi(v))
		default:
			h.E.Out.Error("Unknown key: %s", k)
			return
		}
	}
	mode := "constant"
	if pulsing {
		mode = "pulsing"
	}
	h.E.Ack(h.E.API.GunFx.SmokeConfig(pulsing, speed, high, low, pulse_ms, spindown_ms),
		fmt.Sprintf("Smoke: %s, speed=%d, h/l=%d/%d, spindown=%dms", mode, speed, high, low, spindown_ms))
}

func (h *Handler) cmdSmokeReset(_ []string) {
	h.E.Ack(h.E.API.GunFx.SmokeReset(), "Smoke errors cleared")
}

func (h *Handler) cmdSmokeLimit(args []string) {
	if !h.E.RequireArgs(args, 2, "smoke.limit heater|fan <mA>") {
		return
	}
	ch := byte(0)
	switch strings.ToLower(args[0]) {
	case "heater", "0":
		ch = 0
	case "fan", "1":
		ch = 1
	default:
		h.E.Out.Error("Channel must be 'heater' or 'fan'")
		return
	}
	limit := engine.Atoi(args[1])
	name := "Heater"
	if ch == 1 {
		name = "Fan"
	}
	msg := fmt.Sprintf("%s limit: %dmA", name, limit)
	if limit == 0 {
		msg = fmt.Sprintf("%s overcurrent protection disabled", name)
	}
	h.E.Ack(h.E.API.GunFx.SmokeLimit(ch, uint16(limit)), msg)
}

