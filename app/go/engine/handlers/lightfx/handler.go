package lightfx

// ScaleFX Engine - LightFX Command Handler
// Commands for LightFX lighting effects controller.

import (
	"fmt"
	"scalefx/engine"
	pcore "scalefx/protocol/core"
	lfxp "scalefx/protocol/lightfx"
	"strings"
)

// Handler groups all LightFX commands, decoders, and parsers.
//
// Broadcast observers are silent by default (Studio opts in). The
// LandingLightStatus observer set is pre-seeded with the CLI formatter so
// the user sees landing progress as it happens.
type Handler struct {
	E *engine.Engine

	OnStatusBroadcast    engine.Observers[StatusBroadcast]
	OnLandingLightStatus engine.Observers[LandingLightStatus]
}

// Register adds the LightFX command group, status parser, and async handlers
// to the engine. Returns the Handler so external consumers can install
// listeners via .Add().
func Register(eng *engine.Engine) *Handler {
	h := &Handler{E: eng}
	h.OnLandingLightStatus.Add(h.FormatLandingLightStatus)

	eng.RegisterStatusParser(pcore.CtrlLightFX, func(data []byte) {
		if s := DecodeStatusBroadcast(data); s != nil {
			h.FormatStatusBroadcast(s)
		} else {
			h.E.Out.Printf("  LightFX: (incomplete: %d bytes)\n", len(data))
		}
	})
	eng.RegisterStatusBroadcastParser(pcore.CtrlLightFX, func(data []byte) {
		if h.OnStatusBroadcast.Len() == 0 {
			return
		}
		if s := DecodeStatusBroadcast(data); s != nil {
			h.OnStatusBroadcast.Fire(s)
		}
	})
	eng.RegisterAsyncHandler(lfxp.LandingLightStatus, func(p []byte) {
		h.OnLandingLightStatus.Dispatch(h.E.Out, "LandingLightStatus", p, DecodeLandingLightStatus)
	})
	eng.AddGroup(h.commands())
	return h
}

func (h *Handler) commands() *engine.CmdGroup {
	g := &engine.CmdGroup{
		Name:       "LightFX",
		Controller: pcore.CtrlLightFX,
		Color:      engine.ColorBlue,
		Commands: map[string]engine.CmdEntry{
			"led":             {h.cmdLed, "led <ch> <brightness>", "Set LED brightness (0-100%)", true},
			"led.off":         {h.cmdLedOff, "led.off [ch]", "Turn off LED (0=all)", true},
			"led.status":      {h.cmdLedStatus, "led.status", "Show all LED channel statuses", true},
			"seq.add":         {h.cmdSeqAdd, "seq.add <ch> <event> <params...>", "Add sequence event", true},
			"seq.clear":       {h.cmdSeqClear, "seq.clear <ch>", "Clear sequence", true},
			"seq.start":       {h.cmdSeqStart, "seq.start <ch> [loops]", "Start sequence", true},
			"seq.stop":        {h.cmdSeqStop, "seq.stop <ch>", "Stop sequence", true},
			"seq.restart":     {h.cmdSeqRestart, "seq.restart <ch>", "Restart sequence from beginning", true},
			"seq.status":      {h.cmdSeqStatus, "seq.status <ch>", "Show sequence status", true},
			"seq.queue":       {h.cmdSeqQueue, "seq.queue <ch>", "List sequence event queue", true},
			"brightness":      {h.cmdMasterBright, "brightness <0-100>", "Set master LED brightness", true},
			"servo":           {h.cmdServo, "servo set <id> <pulse_us>", "Set servo position", true},
			"servo.config":    {h.cmdServoConfig, "servo.config <id> <min> <max> [spd] [acc] [dec] [rev]", "Configure servo", true},
			"landing.bind":    {h.cmdLandingBind, "landing.bind <slot> <servo> <channels> [bright]", "Bind landing group (servo=0 for none, channels: 5 or 5,6,7)", true},
			"landing.unbind":  {h.cmdLandingUnbind, "landing.unbind [slot]", "Unbind landing light (0=all)", true},
			"landing.deploy":  {h.cmdLandingDeploy, "landing.deploy [slot]", "Deploy landing light (0=all)", true},
			"landing.retract": {h.cmdLandingRetract, "landing.retract [slot]", "Retract landing light (0=all)", true},
			"reset":           {h.cmdReset, "reset [ch]", "Reset LED channel (0=all)", true},
			"enable":          {h.cmdEnable, "enable <ch>", "Enable LED channel (0=all)", true},
			"disable":         {h.cmdDisable, "disable <ch>", "Disable LED channel (0=all)", true},
		},
	}
	return g
}

// ─── LED Direct Control ───

func (h *Handler) cmdLed(args []string) {
	if !h.E.RequireArgs(args, 2, "led <channel> <brightness>") {
		return
	}
	ch, bright := engine.Atoi(args[0]), engine.Atoi(args[1])
	h.E.Ack(h.E.API.LightFx.LedSet(byte(ch), byte(bright)), fmt.Sprintf("LED %d → %d%%", ch, bright))
}

func (h *Handler) cmdLedOff(args []string) {
	ch := 0
	if len(args) > 0 {
		ch = engine.Atoi(args[0])
	}
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	h.E.Ack(h.E.API.LightFx.LedOff(byte(ch)), fmt.Sprintf("%s OFF", target))
}

func (h *Handler) cmdLedStatus(_ []string) {
	if !h.E.RequireConn() {
		return
	}
	h.E.Query(h.E.API.LightFx.LedStatus(), h.parseLedStatus)
}

// ─── Sequence Control ───

func (h *Handler) cmdSeqClear(args []string) {
	if !h.E.RequireArgs(args, 1, "seq.clear <channel>") {
		return
	}
	ch := engine.Atoi(args[0])
	h.E.Ack(h.E.API.LightFx.SeqClear(byte(ch)), fmt.Sprintf("Sequence %d cleared", ch))
}

func (h *Handler) cmdSeqAdd(args []string) {
	if len(args) < 2 {
		h.printSeqAddUsage()
		return
	}

	ch := engine.Atoi(args[0])
	event := strings.ToLower(args[1])
	rest := args[2:]
	lfx := h.E.API.LightFx

	switch event {
	case "on":
		if len(rest) < 2 {
			h.E.Out.Error("Usage: seq.add <ch> on <duration_ms> <brightness> [pwm_duty]")
			return
		}
		dur, bright := uint16(engine.Atoi(rest[0])), byte(engine.Atoi(rest[1]))
		pwmDuty := uint16(0) // 0 = no power save, 1-100 = duty percent
		if len(rest) > 2 {
			pwmDuty = uint16(engine.Atoi(rest[2]))
		}
		h.E.Ack(lfx.SeqAdd(byte(ch), lfxp.EvtOn, dur, pwmDuty, bright, 0),
			fmt.Sprintf("Seq %d: ON %dms at %d%%", ch, dur, bright))

	case "off":
		if len(rest) < 1 {
			h.E.Out.Error("Usage: seq.add <ch> off <duration_ms>")
			return
		}
		dur := uint16(engine.Atoi(rest[0]))
		h.E.Ack(lfx.SeqAdd(byte(ch), lfxp.EvtOff, dur, 0, 0, 0),
			fmt.Sprintf("Seq %d: OFF %dms", ch, dur))

	case "flash":
		if len(rest) < 3 {
			h.E.Out.Error("Usage: seq.add <ch> flash <interval_ms> <duration_ms> <brightness> [duty]")
			return
		}
		interval, dur, bright := uint16(engine.Atoi(rest[0])), uint16(engine.Atoi(rest[1])), byte(engine.Atoi(rest[2]))
		duty := byte(50)
		if len(rest) > 3 {
			duty = byte(engine.Atoi(rest[3]))
		}
		h.E.Ack(lfx.SeqAdd(byte(ch), lfxp.EvtFlash, interval, dur, bright, duty),
			fmt.Sprintf("Seq %d: FLASH %dms for %dms, %d%% duty", ch, interval, dur, duty))

	case "fadein":
		if len(rest) < 2 {
			h.E.Out.Error("Usage: seq.add <ch> fadein <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(engine.Atoi(rest[0])), byte(engine.Atoi(rest[1]))
		h.E.Ack(lfx.SeqAdd(byte(ch), lfxp.EvtFadeIn, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: FADE IN %dms to %d%%", ch, dur, bright))

	case "fadeout":
		if len(rest) < 2 {
			h.E.Out.Error("Usage: seq.add <ch> fadeout <duration_ms> <brightness>")
			return
		}
		dur, bright := uint16(engine.Atoi(rest[0])), byte(engine.Atoi(rest[1]))
		h.E.Ack(lfx.SeqAdd(byte(ch), lfxp.EvtFadeOut, dur, 0, bright, 0),
			fmt.Sprintf("Seq %d: FADE OUT %dms from %d%%", ch, dur, bright))

	case "fading":
		if len(rest) < 2 {
			h.E.Out.Error("Usage: seq.add <ch> fading <cycle_ms> <duration_ms> [min] [max]")
			return
		}
		cycle, dur := uint16(engine.Atoi(rest[0])), uint16(engine.Atoi(rest[1]))
		minB, maxB := byte(0), byte(100)
		if len(rest) > 2 {
			minB = byte(engine.Atoi(rest[2]))
		}
		if len(rest) > 3 {
			maxB = byte(engine.Atoi(rest[3]))
		}
		h.E.Ack(lfx.SeqAdd(byte(ch), lfxp.EvtFading, cycle, dur, minB, maxB),
			fmt.Sprintf("Seq %d: FADING cycle %dms for %dms (%d-%d%%)", ch, cycle, dur, minB, maxB))

	case "beacon":
		if len(rest) < 2 {
			h.E.Out.Error("Usage: seq.add <ch> beacon <cycle_ms> <duration_ms> [flash_pct] [max] [min]")
			return
		}
		cycle, dur := uint16(engine.Atoi(rest[0])), uint16(engine.Atoi(rest[1]))
		flashPct, maxB, minB := byte(15), byte(100), byte(0)
		if len(rest) > 2 {
			flashPct = byte(engine.Atoi(rest[2]))
		}
		if len(rest) > 3 {
			maxB = byte(engine.Atoi(rest[3]))
		}
		if len(rest) > 4 {
			minB = byte(engine.Atoi(rest[4]))
		}
		h.E.Ack(lfx.SeqAdd(byte(ch), lfxp.EvtBeacon, cycle, dur, flashPct, maxB, minB),
			fmt.Sprintf("Seq %d: BEACON cycle %dms for %dms, flash %d%% %d-%d%%", ch, cycle, dur, flashPct, minB, maxB))

	default:
		h.E.Out.Error("Unknown event: %s", event)
		h.printSeqAddUsage()
	}
}

func (h *Handler) printSeqAddUsage() {
	h.E.Out.Info("Usage: seq.add <ch> <event> <params...>")
	h.E.Out.Info("Events:")
	h.E.Out.Printf("  on      <duration_ms> <brightness> [pwm_duty]\n")
	h.E.Out.Printf("  off     <duration_ms>\n")
	h.E.Out.Printf("  flash   <interval_ms> <duration_ms> <brightness> [duty]\n")
	h.E.Out.Printf("  fadein  <duration_ms> <brightness>\n")
	h.E.Out.Printf("  fadeout <duration_ms> <brightness>\n")
	h.E.Out.Printf("  fading  <cycle_ms> <duration_ms> [min] [max]\n")
	h.E.Out.Printf("  beacon  <cycle_ms> <duration_ms> [flash_pct] [max] [min]\n")
	h.E.Out.Printf("\nNote: duration=0 means infinite. For ON/OFF this prevents looping.\n")
}

func (h *Handler) cmdSeqStart(args []string) {
	if !h.E.RequireArgs(args, 1, "seq.start <channel> [loops]") {
		return
	}
	ch := engine.Atoi(args[0])
	loops := 0
	if len(args) > 1 {
		loops = engine.Atoi(args[1])
	}
	h.E.Ack(h.E.API.LightFx.SeqStart(byte(ch), uint16(loops)), fmt.Sprintf("Sequence %d started", ch))
}

func (h *Handler) cmdSeqStop(args []string) {
	if !h.E.RequireArgs(args, 1, "seq.stop <channel>") {
		return
	}
	ch := engine.Atoi(args[0])
	h.E.Ack(h.E.API.LightFx.SeqStop(byte(ch)), fmt.Sprintf("Sequence %d stopped", ch))
}

func (h *Handler) cmdSeqRestart(args []string) {
	if !h.E.RequireArgs(args, 1, "seq.restart <channel>") {
		return
	}
	ch := engine.Atoi(args[0])
	h.E.Ack(h.E.API.LightFx.SeqRestart(byte(ch)), fmt.Sprintf("Sequence %d restarted", ch))
}

func (h *Handler) cmdSeqStatus(args []string) {
	if !h.E.RequireArgs(args, 1, "seq.status <channel>") || !h.E.RequireConn() {
		return
	}
	ch := engine.Atoi(args[0])
	h.E.Query(h.E.API.LightFx.SeqStatus(byte(ch)), h.parseLedSeqStatus)
}

func (h *Handler) cmdSeqQueue(args []string) {
	if !h.E.RequireArgs(args, 1, "seq.queue <channel>") || !h.E.RequireConn() {
		return
	}
	ch := engine.Atoi(args[0])
	h.E.Query(h.E.API.LightFx.SeqQueue(byte(ch)), h.parseLedSeqQueue)
}

// ─── Master Brightness ───

func (h *Handler) cmdMasterBright(args []string) {
	if !h.E.RequireArgs(args, 1, "brightness <0-100>") {
		return
	}
	val := engine.Atoi(args[0])
	h.E.Ack(h.E.API.LightFx.MasterBrightness(byte(val)), fmt.Sprintf("Master brightness → %d%%", val))
}

// ─── Servo Commands ───

func (h *Handler) cmdServo(args []string) {
	h.E.ServoSet(args, "servo set <id> <pulse_us>", h.E.API.LightFx.ServoSet)
}

func (h *Handler) cmdServoConfig(args []string) {
	h.E.ServoConfig(args, "servo.config <id> <min> <max> [spd] [acc] [dec] [rev]", h.E.API.LightFx.ServoConfig)
}

// ─── Landing Light Commands ───

func (h *Handler) cmdLandingBind(args []string) {
	if !h.E.RequireArgs(args, 3, "landing.bind <slot> <servo> <channels> [brightness]\n  servo=0 for no servo, channels: single (5) or comma-list (5,6,7)") {
		return
	}
	slot := byte(engine.Atoi(args[0]))
	servoID := byte(engine.Atoi(args[1]))
	bright := byte(100)
	if len(args) > 3 {
		bright = byte(engine.Atoi(args[3]))
	}

	// Parse channel list into bitmask (e.g. "5,6,7" -> bit4|bit5|bit6 = 0x70)
	var mask byte
	parts := strings.Split(args[2], ",")
	for _, p := range parts {
		p = strings.TrimSpace(p)
		ch := engine.Atoi(p)
		if ch < 1 || ch > 8 {
			h.E.Out.Error("Invalid channel: %s (must be 1-8)", p)
			return
		}
		mask |= 1 << (ch - 1)
	}
	if mask == 0 {
		h.E.Out.Error("No valid channels specified")
		return
	}

	servoStr := fmt.Sprintf("servo %d", servoID)
	if servoID == 0 {
		servoStr = "no servo"
	}
	h.E.Ack(h.E.API.LightFx.LandingBind(slot, servoID, mask, bright),
		fmt.Sprintf("Landing group %d: %s + channels %s (mask 0x%02X), bright %d%%",
			slot, servoStr, args[2], mask, bright))
}

func (h *Handler) cmdLandingUnbind(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(engine.Atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all slots"
	}
	h.E.Ack(h.E.API.LightFx.LandingUnbind(slot), fmt.Sprintf("Landing light %s unbound", target))
}

func (h *Handler) cmdLandingDeploy(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(engine.Atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all"
	}
	h.E.Ack(h.E.API.LightFx.LandingDeploy(slot), fmt.Sprintf("Landing light %s deploying", target))
}

func (h *Handler) cmdLandingRetract(args []string) {
	slot := byte(0)
	if len(args) > 0 {
		slot = byte(engine.Atoi(args[0]))
	}
	target := fmt.Sprintf("slot %d", slot)
	if slot == 0 {
		target = "all"
	}
	h.E.Ack(h.E.API.LightFx.LandingRetract(slot), fmt.Sprintf("Landing light %s retracting", target))
}

// ─── Channel Management ───

func (h *Handler) cmdReset(args []string) {
	ch := byte(0)
	if len(args) > 0 {
		ch = byte(engine.Atoi(args[0]))
	}
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	h.E.Ack(h.E.API.LightFx.Reset(ch), fmt.Sprintf("%s reset", target))
}

func (h *Handler) cmdEnable(args []string) {
	if !h.E.RequireArgs(args, 1, "enable <ch> (1-8, 0=all)") {
		return
	}
	ch := byte(engine.Atoi(args[0]))
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	h.E.Ack(h.E.API.LightFx.Enable(ch, true), fmt.Sprintf("%s enabled", target))
}

func (h *Handler) cmdDisable(args []string) {
	if !h.E.RequireArgs(args, 1, "disable <ch> (1-8, 0=all)") {
		return
	}
	ch := byte(engine.Atoi(args[0]))
	target := fmt.Sprintf("LED %d", ch)
	if ch == 0 {
		target = "All LEDs"
	}
	h.E.Ack(h.E.API.LightFx.Enable(ch, false), fmt.Sprintf("%s disabled", target))
}


