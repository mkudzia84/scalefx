package engine

// ScaleFX Engine - GunFX Command Handler
// Commands for GunFX weapon effects controller.

import (
	"fmt"
	"scalefx/protocol/core"
	"strings"
)

func (e *Engine) gunfxCommands() *CmdGroup {
	return &CmdGroup{
		Name:       "GunFX",
		Controller: core.CtrlGunFX,
		Color:      ColorRed,
		Commands: map[string]CmdEntry{
			"trigger":      {e.cmdGfxTrigger, "trigger on <rpm> | off [delay_ms]", "Control firing", true},
			"servo":        {e.cmdGfxServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config": {e.cmdGfxServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec]", "Configure servo", true},
			"servo.recoil": {e.cmdGfxServoRecoil, "servo.recoil <id> <jerk_us> <variance_us>", "Configure recoil", true},
			"smoke":        {e.cmdGfxSmoke, "smoke heat on|off", "Control smoke heater", true},
			"smoke.config": {e.cmdGfxSmokeConfig, "smoke.config [key=value ...]", "Configure smoke fan", true},
			"smoke.reset":  {e.cmdGfxSmokeReset, "smoke.reset", "Clear smoke errors", true},
			"smoke.limit":  {e.cmdGfxSmokeLimit, "smoke.limit heater|fan <mA>", "Set overcurrent limit", true},
		},
	}
}

// ─── GunFX Command Handlers ───

func (e *Engine) cmdGfxTrigger(args []string) {
	if !e.requireArgs(args, 1, "trigger on <rpm> | off [delay_ms]") {
		return
	}
	gfx := e.API.GunFx
	switch strings.ToLower(args[0]) {
	case "on":
		if !e.requireArgs(args, 2, "trigger on <rpm>") {
			return
		}
		rpm := Atoi(args[1])
		e.ack(gfx.TriggerOn(uint16(rpm)), fmt.Sprintf("Trigger ON at %d RPM", rpm))
	case "off":
		delay := 3000
		if len(args) > 1 {
			delay = Atoi(args[1])
		}
		e.ack(gfx.TriggerOff(uint16(delay)), fmt.Sprintf("Trigger OFF (spin-down: %dms)", delay))
	default:
		e.Out.Error("Use 'on' or 'off'")
	}
}

// Servo commands delegate to shared helpers (Strategy pattern)
func (e *Engine) cmdGfxServo(args []string) {
	e.servoSet(args, "servo set <id> <pulse_us>", e.API.GunFx.ServoSet)
}

func (e *Engine) cmdGfxServoConfig(args []string) {
	e.servoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec]", e.API.GunFx.ServoConfig)
}

func (e *Engine) cmdGfxServoRecoil(args []string) {
	if !e.requireArgs(args, 3, "servo.recoil <id> <jerk_us> <variance_us>") {
		return
	}
	id, jerk, variance := Atoi(args[0]), Atoi(args[1]), Atoi(args[2])
	e.ack(e.API.GunFx.ServoRecoil(byte(id), uint16(jerk), uint16(variance)),
		fmt.Sprintf("Servo %d recoil: jerk=%dµs, variance=±%dµs", id, jerk, variance))
}

func (e *Engine) cmdGfxSmoke(args []string) {
	if !e.requireArgs(args, 2, "smoke heat on|off") {
		return
	}
	if strings.ToLower(args[0]) != "heat" {
		e.Out.Error("Usage: smoke heat on|off")
		return
	}
	on := ParseBool(args[1])
	e.ack(e.API.GunFx.SmokeHeat(on), fmt.Sprintf("Smoke heater %s", OnOff(on)))
}

func (e *Engine) cmdGfxSmokeConfig(args []string) {
	if len(args) == 0 {
		e.Out.Error("Usage: smoke.config [key=value ...]")
		e.Out.Info("  Keys: pulsing (0|1), speed (0-255), high, low, pulse_ms, spindown_ms")
		return
	}
	pulsing := false
	speed, high, low := byte(255), byte(255), byte(80)
	pulse_ms, spindown_ms := uint16(0), uint16(5000)
	for _, arg := range args {
		kv := strings.SplitN(arg, "=", 2)
		if len(kv) != 2 {
			e.Out.Error("Invalid: %s (use key=value)", arg)
			return
		}
		k, v := strings.ToLower(kv[0]), kv[1]
		switch k {
		case "pulsing":
			pulsing = ParseBool(v)
		case "speed":
			speed = byte(Atoi(v))
		case "high":
			high = byte(Atoi(v))
		case "low":
			low = byte(Atoi(v))
		case "pulse_ms", "pulse":
			pulse_ms = uint16(Atoi(v))
		case "spindown_ms", "spindown":
			spindown_ms = uint16(Atoi(v))
		default:
			e.Out.Error("Unknown key: %s", k)
			return
		}
	}
	mode := "constant"
	if pulsing {
		mode = "pulsing"
	}
	e.ack(e.API.GunFx.SmokeConfig(pulsing, speed, high, low, pulse_ms, spindown_ms),
		fmt.Sprintf("Smoke: %s, speed=%d, h/l=%d/%d, spindown=%dms", mode, speed, high, low, spindown_ms))
}

func (e *Engine) cmdGfxSmokeReset(_ []string) {
	e.ack(e.API.GunFx.SmokeReset(), "Smoke errors cleared")
}

func (e *Engine) cmdGfxSmokeLimit(args []string) {
	if !e.requireArgs(args, 2, "smoke.limit heater|fan <mA>") {
		return
	}
	ch := byte(0)
	switch strings.ToLower(args[0]) {
	case "heater", "0":
		ch = 0
	case "fan", "1":
		ch = 1
	default:
		e.Out.Error("Channel must be 'heater' or 'fan'")
		return
	}
	limit := Atoi(args[1])
	name := "Heater"
	if ch == 1 {
		name = "Fan"
	}
	msg := fmt.Sprintf("%s limit: %dmA", name, limit)
	if limit == 0 {
		msg = fmt.Sprintf("%s overcurrent protection disabled", name)
	}
	e.ack(e.API.GunFx.SmokeLimit(ch, uint16(limit)), msg)
}
