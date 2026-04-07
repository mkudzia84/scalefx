package main

// ScaleFX CLI - LightFX Command Handler
// Commands for LightFX lighting effects controller.
// Aligned with Python CLI: tests/cli/handlers/lightfx.py

import (
	"fmt"
	"scalefx/protocol/core"
	"scalefx/protocol/lightfx"
	"strings"
)

func (c *CLI) lightfxCommands() *cmdGroup {
	return &cmdGroup{
		Name:       "LightFX",
		Controller: core.CtrlLightFX,
		Color:      colorBlue,
		Commands: map[string]cmdEntry{
			"led":             {c.cmdLfxLed, "led <ch> <brightness>", "Set LED brightness (0-100%)", true},
			"led.off":         {c.cmdLfxLedOff, "led.off [ch]", "Turn off LED (0=all)", true},
			"led.status":      {c.cmdLfxLedStatus, "led.status", "Show all LED channel statuses", true},
			"seq.add":         {c.cmdLfxSeqAdd, "seq.add <ch> <event> <params...>", "Add sequence event", true},
			"seq.clear":       {c.cmdLfxSeqClear, "seq.clear <ch>", "Clear sequence", true},
			"seq.start":       {c.cmdLfxSeqStart, "seq.start <ch> [loops]", "Start sequence", true},
			"seq.stop":        {c.cmdLfxSeqStop, "seq.stop <ch>", "Stop sequence", true},
			"seq.restart":     {c.cmdLfxSeqRestart, "seq.restart <ch>", "Restart sequence from beginning", true},
			"seq.status":      {c.cmdLfxSeqStatus, "seq.status <ch>", "Show sequence status", true},
			"seq.queue":       {c.cmdLfxSeqQueue, "seq.queue <ch>", "List sequence event queue", true},
			"brightness":      {c.cmdLfxMasterBright, "brightness <0-100>", "Set master LED brightness", true},
			"servo":           {c.cmdLfxServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config":    {c.cmdLfxServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec]", "Configure servo", true},
			"landing.bind":    {c.cmdLfxLandingBind, "landing.bind <slot> <servo> <led> <deploy_us> <retract_us> [bright]", "Bind landing light", true},
			"landing.unbind":  {c.cmdLfxLandingUnbind, "landing.unbind [slot]", "Unbind landing light (0=all)", true},
			"landing.deploy":  {c.cmdLfxLandingDeploy, "landing.deploy [slot]", "Deploy landing light (0=all)", true},
			"landing.retract": {c.cmdLfxLandingRetract, "landing.retract [slot]", "Retract landing light (0=all)", true},
			"reset":           {c.cmdLfxReset, "reset [ch]", "Reset LED channel (0=all)", true},
			"enable":          {c.cmdLfxEnable, "enable <ch>", "Enable LED channel (0=all)", true},
			"disable":         {c.cmdLfxDisable, "disable <ch>", "Disable LED channel (0=all)", true},
		},
	}
}

// ─── LED Direct Control ───

func (c *CLI) cmdLfxLed(args []string) {
	if !requireArgs(args, 2, "led <channel> <brightness>") {
		return
	}
	ch, bright := atoi(args[0]), atoi(args[1])
	c.ack(c.api.LightFx.LedSet(byte(ch), byte(bright)), fmt.Sprintf("LED %d → %d%%", ch, bright))
}

func (c *CLI) cmdLfxLedOff(args []string) {
	ch := 0
	if len(args) > 0 {
		ch = atoi(args[0])
	}
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	c.ack(c.api.LightFx.LedOff(byte(ch)), fmt.Sprintf("%s OFF", target))
}

func (c *CLI) cmdLfxLedStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	c.query(c.api.LightFx.LedStatus(), ParseLedStatus)
}

// ─── Sequence Control ───

func (c *CLI) cmdLfxSeqClear(args []string) {
	if !requireArgs(args, 1, "seq.clear <channel>") {
		return
	}
	ch := atoi(args[0])
	c.ack(c.api.LightFx.SeqClear(byte(ch)), fmt.Sprintf("Sequence %d cleared", ch))
}

func (c *CLI) cmdLfxSeqAdd(args []string) {
	if len(args) < 2 {
		printSeqAddUsage()
		return
	}

	ch := atoi(args[0])
	event := strings.ToLower(args[1])
	rest := args[2:]
	lfx := c.api.LightFx

	switch event {
	case "on":
		if len(rest) < 2 {
			PrintError("Usage: seq.add <ch> on <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(atoi(rest[0])), byte(atoi(rest[1]))
		c.ack(lfx.SeqAdd(byte(ch), lightfx.EvtOn, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: ON %dms at %d%%", ch, dur, bright))

	case "off":
		if len(rest) < 1 {
			PrintError("Usage: seq.add <ch> off <duration_ms>")
			return
		}
		dur := uint16(atoi(rest[0]))
		c.ack(lfx.SeqAdd(byte(ch), lightfx.EvtOff, dur, 0, 0, 0),
			fmt.Sprintf("Seq %d: OFF %dms", ch, dur))

	case "flash":
		if len(rest) < 3 {
			PrintError("Usage: seq.add <ch> flash <interval_ms> <duration_ms> <brightness> [duty]")
			return
		}
		interval, dur, bright := uint16(atoi(rest[0])), uint16(atoi(rest[1])), byte(atoi(rest[2]))
		duty := byte(50)
		if len(rest) > 3 {
			duty = byte(atoi(rest[3]))
		}
		c.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFlash, interval, dur, bright, duty),
			fmt.Sprintf("Seq %d: FLASH %dms for %dms, %d%% duty", ch, interval, dur, duty))

	case "fadein":
		if len(rest) < 2 {
			PrintError("Usage: seq.add <ch> fadein <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(atoi(rest[0])), byte(atoi(rest[1]))
		c.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFadeIn, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: FADE IN %dms to %d%%", ch, dur, bright))

	case "fadeout":
		if len(rest) < 2 {
			PrintError("Usage: seq.add <ch> fadeout <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(atoi(rest[0])), byte(atoi(rest[1]))
		c.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFadeOut, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: FADE OUT %dms from %d%%", ch, dur, bright))

	case "fading":
		if len(rest) < 2 {
			PrintError("Usage: seq.add <ch> fading <cycle_ms> <duration_ms> [min] [max]")
			return
		}
		cycle, dur := uint16(atoi(rest[0])), uint16(atoi(rest[1]))
		minB, maxB := byte(0), byte(100)
		if len(rest) > 2 {
			minB = byte(atoi(rest[2]))
		}
		if len(rest) > 3 {
			maxB = byte(atoi(rest[3]))
		}
		c.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFading, cycle, dur, minB, maxB),
			fmt.Sprintf("Seq %d: FADING cycle %dms for %dms (%d-%d%%)", ch, cycle, dur, minB, maxB))

	case "beacon":
		if len(rest) < 2 {
			PrintError("Usage: seq.add <ch> beacon <cycle_ms> <duration_ms> [flash_pct] [max]")
			return
		}
		cycle, dur := uint16(atoi(rest[0])), uint16(atoi(rest[1]))
		flashPct, maxB := byte(15), byte(100)
		if len(rest) > 2 {
			flashPct = byte(atoi(rest[2]))
		}
		if len(rest) > 3 {
			maxB = byte(atoi(rest[3]))
		}
		c.ack(lfx.SeqAdd(byte(ch), lightfx.EvtBeacon, cycle, dur, flashPct, maxB),
			fmt.Sprintf("Seq %d: BEACON cycle %dms for %dms, flash %d%% peak %d%%", ch, cycle, dur, flashPct, maxB))

	default:
		PrintError("Unknown event: %s", event)
		printSeqAddUsage()
	}
}

func printSeqAddUsage() {
	PrintInfo("Usage: seq.add <ch> <event> <params...>")
	PrintInfo("Events:")
	fmt.Println("  on      <duration_ms> <brightness>")
	fmt.Println("  off     <duration_ms>")
	fmt.Println("  flash   <interval_ms> <duration_ms> <brightness> [duty]")
	fmt.Println("  fadein  <duration_ms> <brightness>")
	fmt.Println("  fadeout <duration_ms> <brightness>")
	fmt.Println("  fading  <cycle_ms> <duration_ms> [min] [max]")
	fmt.Println("  beacon  <cycle_ms> <duration_ms> [flash_pct] [max]")
}

func (c *CLI) cmdLfxSeqStart(args []string) {
	if !requireArgs(args, 1, "seq.start <channel> [loops]") {
		return
	}
	ch := atoi(args[0])
	loops := 0
	if len(args) > 1 {
		loops = atoi(args[1])
	}
	c.ack(c.api.LightFx.SeqStart(byte(ch), uint16(loops)), fmt.Sprintf("Sequence %d started", ch))
}

func (c *CLI) cmdLfxSeqStop(args []string) {
	if !requireArgs(args, 1, "seq.stop <channel>") {
		return
	}
	ch := atoi(args[0])
	c.ack(c.api.LightFx.SeqStop(byte(ch)), fmt.Sprintf("Sequence %d stopped", ch))
}

func (c *CLI) cmdLfxSeqRestart(args []string) {
	if !requireArgs(args, 1, "seq.restart <channel>") {
		return
	}
	ch := atoi(args[0])
	c.ack(c.api.LightFx.SeqRestart(byte(ch)), fmt.Sprintf("Sequence %d restarted", ch))
}

func (c *CLI) cmdLfxSeqStatus(args []string) {
	if !requireArgs(args, 1, "seq.status <channel>") || !c.requireConn() {
		return
	}
	ch := atoi(args[0])
	c.query(c.api.LightFx.SeqStatus(byte(ch)), ParseLedSeqStatus)
}

func (c *CLI) cmdLfxSeqQueue(args []string) {
	if !requireArgs(args, 1, "seq.queue <channel>") || !c.requireConn() {
		return
	}
	ch := atoi(args[0])
	c.query(c.api.LightFx.SeqQueue(byte(ch)), ParseLedSeqQueue)
}

// ─── Master Brightness ───

func (c *CLI) cmdLfxMasterBright(args []string) {
	if !requireArgs(args, 1, "brightness <0-100>") {
		return
	}
	val := atoi(args[0])
	c.ack(c.api.LightFx.MasterBrightness(byte(val)), fmt.Sprintf("Master brightness → %d%%", val))
}

// ─── Servo Commands ───

func (c *CLI) cmdLfxServo(args []string) {
	c.servoSet(args, "servo set <id> <pulse_us>", c.api.LightFx.ServoSet)
}

func (c *CLI) cmdLfxServoConfig(args []string) {
	c.servoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec]", c.api.LightFx.ServoConfig)
}

// ─── Landing Light Commands ───

func (c *CLI) cmdLfxLandingBind(args []string) {
	if !requireArgs(args, 5, "landing.bind <slot> <servo> <led_ch> <deploy_us> <retract_us> [brightness]") {
		return
	}
	slot := byte(atoi(args[0]))
	servoID := byte(atoi(args[1]))
	ledCh := byte(atoi(args[2]))
	deploy := uint16(atoi(args[3]))
	retract := uint16(atoi(args[4]))
	bright := byte(100)
	if len(args) > 5 {
		bright = byte(atoi(args[5]))
	}
	c.ack(c.api.LightFx.LandingBind(slot, servoID, ledCh, deploy, retract, bright),
		fmt.Sprintf("Landing light %d: servo %d + LED %d, deploy %dµs, retract %dµs, bright %d%%",
			slot, servoID, ledCh, deploy, retract, bright))
}

func (c *CLI) cmdLfxLandingUnbind(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all slots"
	}
	c.ack(c.api.LightFx.LandingUnbind(slot), fmt.Sprintf("Landing light %s unbound", target))
}

func (c *CLI) cmdLfxLandingDeploy(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all"
	}
	c.ack(c.api.LightFx.LandingDeploy(slot), fmt.Sprintf("Landing light %s deploying", target))
}

func (c *CLI) cmdLfxLandingRetract(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all"
	}
	c.ack(c.api.LightFx.LandingRetract(slot), fmt.Sprintf("Landing light %s retracting", target))
}

// ─── Channel Management ───

func (c *CLI) cmdLfxReset(args []string) {
	ch := byte(0)
	if len(args) > 0 {
		ch = byte(atoi(args[0]))
	}
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	c.ack(c.api.LightFx.Reset(ch), fmt.Sprintf("%s reset", target))
}

func (c *CLI) cmdLfxEnable(args []string) {
	if !requireArgs(args, 1, "enable <ch> (1-8, 0=all)") {
		return
	}
	ch := byte(atoi(args[0]))
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	c.ack(c.api.LightFx.Enable(ch, true), fmt.Sprintf("%s enabled", target))
}

func (c *CLI) cmdLfxDisable(args []string) {
	if !requireArgs(args, 1, "disable <ch> (1-8, 0=all)") {
		return
	}
	ch := byte(atoi(args[0]))
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	c.ack(c.api.LightFx.Enable(ch, false), fmt.Sprintf("%s disabled", target))
}
