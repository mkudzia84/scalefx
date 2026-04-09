package gunfx

// ScaleFX Engine - GunFX Command Handler + Parsers
// Commands and status parsing for GunFX weapon effects controller.

import (
	"fmt"
	"scalefx/engine"
	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	gfxp "scalefx/protocol/gunfx"
	"strings"
)

// Handler groups all GunFX commands and parsers.
type Handler struct {
	E *engine.Engine
}

// Register adds the GunFX command group and status parser to the engine.
func Register(eng *engine.Engine) {
	h := &Handler{E: eng}
	eng.RegisterStatusParser(pcore.CtrlGunFX, h.parseGunFXStatus)
	eng.AddGroup(h.commands())
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

// ─── GunFX Status Parser ───

func (h *Handler) parseGunFXStatus(data []byte) {
	if len(data) < 20 {
		h.E.Out.Printf("  GunFX: (incomplete: %d bytes)\n", len(data))
		return
	}

	flags := data[0]
	firing := flags&0x01 != 0
	flashActive := flags&0x02 != 0
	flashFading := flags&0x04 != 0
	heaterOn := flags&0x08 != 0
	fanOn := flags&0x10 != 0
	fanSpindown := flags&0x20 != 0

	fanSpeed := data[1]
	fanOffMs := protocol.ReadU16LE(data, 2)
	servo0 := protocol.ReadU16LE(data, 4)
	servo1 := protocol.ReadU16LE(data, 6)
	servo2 := protocol.ReadU16LE(data, 8)
	rpm := protocol.ReadU16LE(data, 10)
	shots := protocol.ReadU32LE(data, 12)
	heaterMs := protocol.ReadU32LE(data, 16)

	var stateParts []string
	if firing {
		stateParts = append(stateParts, h.E.Out.C(engine.ColorRed, "FIRING"))
	}
	if flashActive {
		stateParts = append(stateParts, "FLASH")
	}
	if flashFading {
		stateParts = append(stateParts, "FADING")
	}
	if heaterOn {
		stateParts = append(stateParts, h.E.Out.C(engine.ColorYellow, "HEATER"))
	}
	if fanOn {
		stateParts = append(stateParts, "FAN")
	}
	if fanSpindown {
		stateParts = append(stateParts, "SPINDOWN")
	}
	stateStr := "IDLE"
	if len(stateParts) > 0 {
		stateStr = strings.Join(stateParts, ", ")
	}

	h.E.Out.Printf("  ── GunFX ──────────────────────\n")
	h.E.Out.Printf("  State:     %s\n", stateStr)

	if firing {
		h.E.Out.Printf("  Fire rate: %d RPM\n", rpm)
	}
	h.E.Out.Printf("  Shots:     %d\n", shots)

	if fanOn || fanSpindown {
		fanInfo := fmt.Sprintf("speed=%d", fanSpeed)
		if fanSpindown && fanOffMs > 0 {
			fanInfo += fmt.Sprintf(", off in %dms", fanOffMs)
		}
		h.E.Out.Printf("  Fan:       %s\n", fanInfo)
	}

	if heaterMs > 0 {
		heaterSec := float64(heaterMs) / 1000.0
		h.E.Out.Printf("  Heater:    %.1fs total\n", heaterSec)
	}

	h.E.Out.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	if len(data) >= 22 {
		htrErr := data[20]
		fanErr := data[21]
		if htrErr != 0 || fanErr != 0 {
			h.E.Out.Printf("  ── Smoke Errors ──────────────\n")
			if htrErr != 0 {
				h.E.Out.Printf("  Heater:    %s\n", h.E.Out.C(engine.ColorRed, gfxp.SmokeErrorReasonName(htrErr)))
			}
			if fanErr != 0 {
				h.E.Out.Printf("  Fan:       %s\n", h.E.Out.C(engine.ColorRed, gfxp.SmokeErrorReasonName(fanErr)))
			}
		}
	}

	if len(data) >= 24 {
		htrDuty := data[22]
		fanDuty := data[23]
		if htrDuty < 255 || fanDuty < 255 {
			h.E.Out.Printf("  ── Overcurrent Throttle ──────\n")
			if htrDuty < 255 {
				pct := int(htrDuty) * 100 / 255
				h.E.Out.Printf("  Heater:    %s\n", h.E.Out.C(engine.ColorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, htrDuty)))
			}
			if fanDuty < 255 {
				pct := int(fanDuty) * 100 / 255
				h.E.Out.Printf("  Fan:       %s\n", h.E.Out.C(engine.ColorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, fanDuty)))
			}
		}
	}

	if len(data) >= 28 {
		batteryMV := protocol.ReadU16LE(data, 24)
		cellCount := data[26]
		batteryPct := data[27]

		if batteryMV > 0 {
			batteryV := float64(batteryMV) / 1000.0
			battParts := []string{fmt.Sprintf("%.2fV (%dmV)", batteryV, batteryMV)}
			if cellCount > 0 {
				battParts = append(battParts, fmt.Sprintf("%dS", cellCount))
			}
			if batteryPct > 0 {
				pctColor := engine.ColorGreen
				if batteryPct <= 10 {
					pctColor = engine.ColorRed
				} else if batteryPct <= 30 {
					pctColor = engine.ColorYellow
				}
				battParts = append(battParts, h.E.Out.C(pctColor, fmt.Sprintf("%d%%", batteryPct)))
			}
			h.E.Out.Printf("  Battery:   %s\n", strings.Join(battParts, ", "))
		} else {
			h.E.Out.Printf("  Battery:   %s\n", h.E.Out.C(engine.ColorYellow, "not detected"))
		}
	}
}
