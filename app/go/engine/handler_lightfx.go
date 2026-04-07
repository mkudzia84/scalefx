package engine

// ScaleFX Engine - LightFX Command Handler
// Commands for LightFX lighting effects controller.

import (
	"fmt"
	"scalefx/protocol/core"
	"scalefx/protocol/lightfx"
	"strings"
)

func (e *Engine) lightfxCommands() *CmdGroup {
	return &CmdGroup{
		Name:       "LightFX",
		Controller: core.CtrlLightFX,
		Color:      ColorBlue,
		Commands: map[string]CmdEntry{
			"led":             {e.cmdLfxLed, "led <ch> <brightness>", "Set LED brightness (0-100%)", true},
			"led.off":         {e.cmdLfxLedOff, "led.off [ch]", "Turn off LED (0=all)", true},
			"led.status":      {e.cmdLfxLedStatus, "led.status", "Show all LED channel statuses", true},
			"seq.add":         {e.cmdLfxSeqAdd, "seq.add <ch> <event> <params...>", "Add sequence event", true},
			"seq.clear":       {e.cmdLfxSeqClear, "seq.clear <ch>", "Clear sequence", true},
			"seq.start":       {e.cmdLfxSeqStart, "seq.start <ch> [loops]", "Start sequence", true},
			"seq.stop":        {e.cmdLfxSeqStop, "seq.stop <ch>", "Stop sequence", true},
			"seq.restart":     {e.cmdLfxSeqRestart, "seq.restart <ch>", "Restart sequence from beginning", true},
			"seq.status":      {e.cmdLfxSeqStatus, "seq.status <ch>", "Show sequence status", true},
			"seq.queue":       {e.cmdLfxSeqQueue, "seq.queue <ch>", "List sequence event queue", true},
			"brightness":      {e.cmdLfxMasterBright, "brightness <0-100>", "Set master LED brightness", true},
			"servo":           {e.cmdLfxServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config":    {e.cmdLfxServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec]", "Configure servo", true},
			"landing.bind":    {e.cmdLfxLandingBind, "landing.bind <slot> <servo> <led> <deploy_us> <retract_us> [bright]", "Bind landing light", true},
			"landing.unbind":  {e.cmdLfxLandingUnbind, "landing.unbind [slot]", "Unbind landing light (0=all)", true},
			"landing.deploy":  {e.cmdLfxLandingDeploy, "landing.deploy [slot]", "Deploy landing light (0=all)", true},
			"landing.retract": {e.cmdLfxLandingRetract, "landing.retract [slot]", "Retract landing light (0=all)", true},
			"reset":           {e.cmdLfxReset, "reset [ch]", "Reset LED channel (0=all)", true},
			"enable":          {e.cmdLfxEnable, "enable <ch>", "Enable LED channel (0=all)", true},
			"disable":         {e.cmdLfxDisable, "disable <ch>", "Disable LED channel (0=all)", true},
		},
	}
}

// ─── LED Direct Control ───

func (e *Engine) cmdLfxLed(args []string) {
	if !e.requireArgs(args, 2, "led <channel> <brightness>") {
		return
	}
	ch, bright := Atoi(args[0]), Atoi(args[1])
	e.ack(e.API.LightFx.LedSet(byte(ch), byte(bright)), fmt.Sprintf("LED %d → %d%%", ch, bright))
}

func (e *Engine) cmdLfxLedOff(args []string) {
	ch := 0
	if len(args) > 0 {
		ch = Atoi(args[0])
	}
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	e.ack(e.API.LightFx.LedOff(byte(ch)), fmt.Sprintf("%s OFF", target))
}

func (e *Engine) cmdLfxLedStatus(_ []string) {
	if !e.requireConn() {
		return
	}
	e.query(e.API.LightFx.LedStatus(), e.ParseLedStatus)
}

// ─── Sequence Control ───

func (e *Engine) cmdLfxSeqClear(args []string) {
	if !e.requireArgs(args, 1, "seq.clear <channel>") {
		return
	}
	ch := Atoi(args[0])
	e.ack(e.API.LightFx.SeqClear(byte(ch)), fmt.Sprintf("Sequence %d cleared", ch))
}

func (e *Engine) cmdLfxSeqAdd(args []string) {
	if len(args) < 2 {
		e.printSeqAddUsage()
		return
	}

	ch := Atoi(args[0])
	event := strings.ToLower(args[1])
	rest := args[2:]
	lfx := e.API.LightFx

	switch event {
	case "on":
		if len(rest) < 2 {
			e.Out.Error("Usage: seq.add <ch> on <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(Atoi(rest[0])), byte(Atoi(rest[1]))
		e.ack(lfx.SeqAdd(byte(ch), lightfx.EvtOn, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: ON %dms at %d%%", ch, dur, bright))

	case "off":
		if len(rest) < 1 {
			e.Out.Error("Usage: seq.add <ch> off <duration_ms>")
			return
		}
		dur := uint16(Atoi(rest[0]))
		e.ack(lfx.SeqAdd(byte(ch), lightfx.EvtOff, dur, 0, 0, 0),
			fmt.Sprintf("Seq %d: OFF %dms", ch, dur))

	case "flash":
		if len(rest) < 3 {
			e.Out.Error("Usage: seq.add <ch> flash <interval_ms> <duration_ms> <brightness> [duty]")
			return
		}
		interval, dur, bright := uint16(Atoi(rest[0])), uint16(Atoi(rest[1])), byte(Atoi(rest[2]))
		duty := byte(50)
		if len(rest) > 3 {
			duty = byte(Atoi(rest[3]))
		}
		e.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFlash, interval, dur, bright, duty),
			fmt.Sprintf("Seq %d: FLASH %dms for %dms, %d%% duty", ch, interval, dur, duty))

	case "fadein":
		if len(rest) < 2 {
			e.Out.Error("Usage: seq.add <ch> fadein <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(Atoi(rest[0])), byte(Atoi(rest[1]))
		e.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFadeIn, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: FADE IN %dms to %d%%", ch, dur, bright))

	case "fadeout":
		if len(rest) < 2 {
			e.Out.Error("Usage: seq.add <ch> fadeout <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(Atoi(rest[0])), byte(Atoi(rest[1]))
		e.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFadeOut, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: FADE OUT %dms from %d%%", ch, dur, bright))

	case "fading":
		if len(rest) < 2 {
			e.Out.Error("Usage: seq.add <ch> fading <cycle_ms> <duration_ms> [min] [max]")
			return
		}
		cycle, dur := uint16(Atoi(rest[0])), uint16(Atoi(rest[1]))
		minB, maxB := byte(0), byte(100)
		if len(rest) > 2 {
			minB = byte(Atoi(rest[2]))
		}
		if len(rest) > 3 {
			maxB = byte(Atoi(rest[3]))
		}
		e.ack(lfx.SeqAdd(byte(ch), lightfx.EvtFading, cycle, dur, minB, maxB),
			fmt.Sprintf("Seq %d: FADING cycle %dms for %dms (%d-%d%%)", ch, cycle, dur, minB, maxB))

	case "beacon":
		if len(rest) < 2 {
			e.Out.Error("Usage: seq.add <ch> beacon <cycle_ms> <duration_ms> [flash_pct] [max]")
			return
		}
		cycle, dur := uint16(Atoi(rest[0])), uint16(Atoi(rest[1]))
		flashPct, maxB := byte(15), byte(100)
		if len(rest) > 2 {
			flashPct = byte(Atoi(rest[2]))
		}
		if len(rest) > 3 {
			maxB = byte(Atoi(rest[3]))
		}
		e.ack(lfx.SeqAdd(byte(ch), lightfx.EvtBeacon, cycle, dur, flashPct, maxB),
			fmt.Sprintf("Seq %d: BEACON cycle %dms for %dms, flash %d%% peak %d%%", ch, cycle, dur, flashPct, maxB))

	default:
		e.Out.Error("Unknown event: %s", event)
		e.printSeqAddUsage()
	}
}

func (e *Engine) printSeqAddUsage() {
	e.Out.Info("Usage: seq.add <ch> <event> <params...>")
	e.Out.Info("Events:")
	e.Out.Printf("  on      <duration_ms> <brightness>\n")
	e.Out.Printf("  off     <duration_ms>\n")
	e.Out.Printf("  flash   <interval_ms> <duration_ms> <brightness> [duty]\n")
	e.Out.Printf("  fadein  <duration_ms> <brightness>\n")
	e.Out.Printf("  fadeout <duration_ms> <brightness>\n")
	e.Out.Printf("  fading  <cycle_ms> <duration_ms> [min] [max]\n")
	e.Out.Printf("  beacon  <cycle_ms> <duration_ms> [flash_pct] [max]\n")
}

func (e *Engine) cmdLfxSeqStart(args []string) {
	if !e.requireArgs(args, 1, "seq.start <channel> [loops]") {
		return
	}
	ch := Atoi(args[0])
	loops := 0
	if len(args) > 1 {
		loops = Atoi(args[1])
	}
	e.ack(e.API.LightFx.SeqStart(byte(ch), uint16(loops)), fmt.Sprintf("Sequence %d started", ch))
}

func (e *Engine) cmdLfxSeqStop(args []string) {
	if !e.requireArgs(args, 1, "seq.stop <channel>") {
		return
	}
	ch := Atoi(args[0])
	e.ack(e.API.LightFx.SeqStop(byte(ch)), fmt.Sprintf("Sequence %d stopped", ch))
}

func (e *Engine) cmdLfxSeqRestart(args []string) {
	if !e.requireArgs(args, 1, "seq.restart <channel>") {
		return
	}
	ch := Atoi(args[0])
	e.ack(e.API.LightFx.SeqRestart(byte(ch)), fmt.Sprintf("Sequence %d restarted", ch))
}

func (e *Engine) cmdLfxSeqStatus(args []string) {
	if !e.requireArgs(args, 1, "seq.status <channel>") || !e.requireConn() {
		return
	}
	ch := Atoi(args[0])
	e.query(e.API.LightFx.SeqStatus(byte(ch)), e.ParseLedSeqStatus)
}

func (e *Engine) cmdLfxSeqQueue(args []string) {
	if !e.requireArgs(args, 1, "seq.queue <channel>") || !e.requireConn() {
		return
	}
	ch := Atoi(args[0])
	e.query(e.API.LightFx.SeqQueue(byte(ch)), e.ParseLedSeqQueue)
}

// ─── Master Brightness ───

func (e *Engine) cmdLfxMasterBright(args []string) {
	if !e.requireArgs(args, 1, "brightness <0-100>") {
		return
	}
	val := Atoi(args[0])
	e.ack(e.API.LightFx.MasterBrightness(byte(val)), fmt.Sprintf("Master brightness → %d%%", val))
}

// ─── Servo Commands ───

func (e *Engine) cmdLfxServo(args []string) {
	e.servoSet(args, "servo set <id> <pulse_us>", e.API.LightFx.ServoSet)
}

func (e *Engine) cmdLfxServoConfig(args []string) {
	e.servoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec]", e.API.LightFx.ServoConfig)
}

// ─── Landing Light Commands ───

func (e *Engine) cmdLfxLandingBind(args []string) {
	if !e.requireArgs(args, 5, "landing.bind <slot> <servo> <led_ch> <deploy_us> <retract_us> [brightness]") {
		return
	}
	slot := byte(Atoi(args[0]))
	servoID := byte(Atoi(args[1]))
	ledCh := byte(Atoi(args[2]))
	deploy := uint16(Atoi(args[3]))
	retract := uint16(Atoi(args[4]))
	bright := byte(100)
	if len(args) > 5 {
		bright = byte(Atoi(args[5]))
	}
	e.ack(e.API.LightFx.LandingBind(slot, servoID, ledCh, deploy, retract, bright),
		fmt.Sprintf("Landing light %d: servo %d + LED %d, deploy %dµs, retract %dµs, bright %d%%",
			slot, servoID, ledCh, deploy, retract, bright))
}

func (e *Engine) cmdLfxLandingUnbind(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(Atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all slots"
	}
	e.ack(e.API.LightFx.LandingUnbind(slot), fmt.Sprintf("Landing light %s unbound", target))
}

func (e *Engine) cmdLfxLandingDeploy(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(Atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all"
	}
	e.ack(e.API.LightFx.LandingDeploy(slot), fmt.Sprintf("Landing light %s deploying", target))
}

func (e *Engine) cmdLfxLandingRetract(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(Atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all"
	}
	e.ack(e.API.LightFx.LandingRetract(slot), fmt.Sprintf("Landing light %s retracting", target))
}

// ─── Channel Management ───

func (e *Engine) cmdLfxReset(args []string) {
	ch := byte(0)
	if len(args) > 0 {
		ch = byte(Atoi(args[0]))
	}
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	e.ack(e.API.LightFx.Reset(ch), fmt.Sprintf("%s reset", target))
}

func (e *Engine) cmdLfxEnable(args []string) {
	if !e.requireArgs(args, 1, "enable <ch> (1-8, 0=all)") {
		return
	}
	ch := byte(Atoi(args[0]))
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	e.ack(e.API.LightFx.Enable(ch, true), fmt.Sprintf("%s enabled", target))
}

func (e *Engine) cmdLfxDisable(args []string) {
	if !e.requireArgs(args, 1, "disable <ch> (1-8, 0=all)") {
		return
	}
	ch := byte(Atoi(args[0]))
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	e.ack(e.API.LightFx.Enable(ch, false), fmt.Sprintf("%s disabled", target))
}
