package main

// ScaleFX CLI - GunFX Command Handler
// Commands for GunFX weapon effects controller.

import (
	"fmt"
	"scalefx/protocol/core"
	"strings"
)

func (c *CLI) gunfxCommands() *cmdGroup {
	return &cmdGroup{
		Name:       "GunFX",
		Controller: core.CtrlGunFX,
		Color:      colorRed,
		Commands: map[string]cmdEntry{
			"trigger":      {c.cmdGfxTrigger, "trigger on <rpm> | off [delay_ms]", "Control firing", true},
			"servo":        {c.cmdGfxServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config": {c.cmdGfxServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec]", "Configure servo", true},
			"servo.recoil": {c.cmdGfxServoRecoil, "servo.recoil <id> <jerk_us> <variance_us>", "Configure recoil", true},
			"smoke":        {c.cmdGfxSmoke, "smoke heat on|off", "Control smoke heater", true},
			"smoke.config": {c.cmdGfxSmokeConfig, "smoke.config [key=value ...]", "Configure smoke fan", true},
			"smoke.reset":  {c.cmdGfxSmokeReset, "smoke.reset", "Clear smoke errors", true},
			"smoke.limit":  {c.cmdGfxSmokeLimit, "smoke.limit heater|fan <mA>", "Set overcurrent limit", true},
		},
	}
}

// ─── GunFX Command Handlers ───

func (c *CLI) cmdGfxTrigger(args []string) {
	if !requireArgs(args, 1, "trigger on <rpm> | off [delay_ms]") {
		return
	}
	gfx := c.api.GunFx
	switch strings.ToLower(args[0]) {
	case "on":
		if !requireArgs(args, 2, "trigger on <rpm>") {
			return
		}
		rpm := atoi(args[1])
		c.ack(gfx.TriggerOn(uint16(rpm)), fmt.Sprintf("Trigger ON at %d RPM", rpm))
	case "off":
		delay := 3000
		if len(args) > 1 {
			delay = atoi(args[1])
		}
		c.ack(gfx.TriggerOff(uint16(delay)), fmt.Sprintf("Trigger OFF (spin-down: %dms)", delay))
	default:
		PrintError("Use 'on' or 'off'")
	}
}

// Servo commands delegate to shared helpers (Strategy pattern)
func (c *CLI) cmdGfxServo(args []string) {
	c.servoSet(args, "servo set <id> <pulse_us>", c.api.GunFx.ServoSet)
}

func (c *CLI) cmdGfxServoConfig(args []string) {
	c.servoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec]", c.api.GunFx.ServoConfig)
}

func (c *CLI) cmdGfxServoRecoil(args []string) {
	if !requireArgs(args, 3, "servo.recoil <id> <jerk_us> <variance_us>") {
		return
	}
	id, jerk, variance := atoi(args[0]), atoi(args[1]), atoi(args[2])
	c.ack(c.api.GunFx.ServoRecoil(byte(id), uint16(jerk), uint16(variance)),
		fmt.Sprintf("Servo %d recoil: jerk=%dµs, variance=±%dµs", id, jerk, variance))
}

func (c *CLI) cmdGfxSmoke(args []string) {
	if !requireArgs(args, 2, "smoke heat on|off") {
		return
	}
	if strings.ToLower(args[0]) != "heat" {
		PrintError("Usage: smoke heat on|off")
		return
	}
	on := parseBool(args[1])
	c.ack(c.api.GunFx.SmokeHeat(on), fmt.Sprintf("Smoke heater %s", onOff(on)))
}

func (c *CLI) cmdGfxSmokeConfig(args []string) {
	if len(args) == 0 {
		PrintError("Usage: smoke.config [key=value ...]")
		PrintInfo("  Keys: pulsing (0|1), speed (0-255), high, low, pulse_ms, spindown_ms")
		return
	}
	pulsing := false
	speed, high, low := byte(255), byte(255), byte(80)
	pulse_ms, spindown_ms := uint16(0), uint16(5000)
	for _, arg := range args {
		kv := strings.SplitN(arg, "=", 2)
		if len(kv) != 2 {
			PrintError("Invalid: %s (use key=value)", arg)
			return
		}
		k, v := strings.ToLower(kv[0]), kv[1]
		switch k {
		case "pulsing":
			pulsing = parseBool(v)
		case "speed":
			speed = byte(atoi(v))
		case "high":
			high = byte(atoi(v))
		case "low":
			low = byte(atoi(v))
		case "pulse_ms", "pulse":
			pulse_ms = uint16(atoi(v))
		case "spindown_ms", "spindown":
			spindown_ms = uint16(atoi(v))
		default:
			PrintError("Unknown key: %s", k)
			return
		}
	}
	mode := "constant"
	if pulsing {
		mode = "pulsing"
	}
	c.ack(c.api.GunFx.SmokeConfig(pulsing, speed, high, low, pulse_ms, spindown_ms),
		fmt.Sprintf("Smoke: %s, speed=%d, h/l=%d/%d, spindown=%dms", mode, speed, high, low, spindown_ms))
}

func (c *CLI) cmdGfxSmokeReset(_ []string) {
	c.ack(c.api.GunFx.SmokeReset(), "Smoke errors cleared")
}

func (c *CLI) cmdGfxSmokeLimit(args []string) {
	if !requireArgs(args, 2, "smoke.limit heater|fan <mA>") {
		return
	}
	ch := byte(0)
	switch strings.ToLower(args[0]) {
	case "heater", "0":
		ch = 0
	case "fan", "1":
		ch = 1
	default:
		PrintError("Channel must be 'heater' or 'fan'")
		return
	}
	limit := atoi(args[1])
	name := "Heater"
	if ch == 1 {
		name = "Fan"
	}
	msg := fmt.Sprintf("%s limit: %dmA", name, limit)
	if limit == 0 {
		msg = fmt.Sprintf("%s overcurrent protection disabled", name)
	}
	c.ack(c.api.GunFx.SmokeLimit(ch, uint16(limit)), msg)
}
