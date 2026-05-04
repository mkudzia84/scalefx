package gearcontrol

// ScaleFX Engine - GearControl CLI console formatters.
// Every formatter is a method on Handler so it can be appended to the
// matching Observers[T] in Register() — the CLI and the Studio terminal
// pane both receive console output through the same listener chain.

import (
	"fmt"
	"scalefx/engine"
	gcp "scalefx/protocol/gearcontrol"
	"strings"
)

var gearDisplayNames = [3]string{"Nose", "Left Main", "Right Main"}

var stateColors = map[byte]engine.Color{
	0: engine.ColorReset, 1: engine.ColorGreen, 2: engine.ColorCyan,
	3: engine.ColorYellow, 4: engine.ColorYellow, 5: engine.ColorRed,
	6: engine.ColorMagenta,
}

var doorStateColors = map[byte]engine.Color{
	0: engine.ColorYellow, 1: engine.ColorCyan, 2: engine.ColorGreen,
	3: engine.ColorYellow, 4: engine.ColorYellow,
}

// FormatStatusBroadcast renders a decoded STATUS_BROADCAST (used both by the
// synchronous `status` response and by broadcast observers that opt in).
func (h *Handler) FormatStatusBroadcast(s *StatusBroadcast) {
	h.E.Out.Printf("  ── GearControl ────────────────\n")

	for i := 0; i < 3; i++ {
		g := s.Gears[i]
		sColor := stateColors[g.State]

		var tags []string
		if !g.Enabled {
			tags = append(tags, h.E.Out.C(engine.ColorYellow, "DISABLED"))
		}
		if g.State == 5 && g.ErrorReason != 0 {
			tags = append(tags, h.E.Out.C(engine.ColorRed, g.ErrorReasonName))
		}
		tagStr := ""
		if len(tags) > 0 {
			tagStr = "  [" + strings.Join(tags, ", ") + "]"
		}

		stallStr := fmt.Sprintf("stall=%dmA", g.StallThreshold_mA)
		if g.StallThreshold_mA == 0 {
			stallStr = h.E.Out.C(engine.ColorYellow, "uncalibrated")
		}

		dModeName := strings.ToLower(gcp.DoorModeName(g.DoorPreMode))
		pdModeName := "skip"
		if g.DoorPostMode != 0 {
			pdModeName = strings.ToLower(gcp.DoorModeName(g.DoorPostMode))
		}

		h.E.Out.Printf("  %10s: %s%s\n", gearDisplayNames[i],
			h.E.Out.C(sColor, g.StateName), tagStr)
		h.E.Out.Printf("             motor=%dmA  shunt=%.1fmV  %s\n",
			g.Current_mA, g.ShuntVoltage_mV, stallStr)

		yawStr := ""
		if g.HasYaw {
			yawStr = "  yaw"
		}
		if g.DoorPreMode != 0 {
			dStateColor := doorStateColors[g.DoorState]
			h.E.Out.Printf("             doors=[%dµs, %dµs]  %s  pre=%s  post=%s%s\n",
				g.Door0_us, g.Door1_us,
				h.E.Out.C(dStateColor, g.DoorStateName),
				dModeName, pdModeName, yawStr)
		} else {
			h.E.Out.Printf("             doors=none%s\n", yawStr)
		}
	}

	h.E.Out.Printf("  ── Global ─────────────────────\n")
	h.E.Out.Printf("  Yaw:       %dµs\n", s.Yaw_us)

	if s.Shunt_mOhm > 0 {
		shuntOhm := float64(s.Shunt_mOhm) / 1000.0
		maxCurrent := 81.92 / shuntOhm
		h.E.Out.Printf("  Shunt:     %dmΩ (%.3fΩ)  max=%.0fmA\n",
			s.Shunt_mOhm, shuntOhm, maxCurrent)
	}

	{
		batteryV := float64(s.Battery.Voltage_mV) / 1000.0
		parts := []string{fmt.Sprintf("%.1fV (%dmV)", batteryV, s.Battery.Voltage_mV)}
		if s.Battery.AutoDeploy {
			parts = append(parts, h.E.Out.C(engine.ColorCyan, "auto-deploy"))
		}
		if s.Battery.LowVoltage {
			parts = append(parts, h.E.Out.C(engine.ColorRed, "LOW VOLTAGE"))
		}
		h.E.Out.Printf("  Battery:   %s\n", strings.Join(parts, ", "))
	}

	var ledParts []string
	for gi := 0; gi < 3; gi++ {
		abbr := string(gearDisplayNames[gi][0])
		depOn := s.LEDs&(1<<(gi*2)) != 0
		retOn := s.LEDs&(1<<(gi*2+1)) != 0
		switch {
		case depOn && retOn:
			ledParts = append(ledParts, h.E.Out.C(engine.ColorYellow, abbr+":both"))
		case depOn:
			ledParts = append(ledParts, h.E.Out.C(engine.ColorGreen, abbr+":dep"))
		case retOn:
			ledParts = append(ledParts, h.E.Out.C(engine.ColorCyan, abbr+":ret"))
		default:
			ledParts = append(ledParts, abbr+":off")
		}
	}
	if s.LEDs&(1<<6) != 0 {
		ledParts = append(ledParts, h.E.Out.C(engine.ColorGreen, "CONN"))
	} else {
		ledParts = append(ledParts, "conn")
	}
	if s.LEDs&(1<<7) != 0 {
		ledParts = append(ledParts, h.E.Out.C(engine.ColorRed, "ERR"))
	} else {
		ledParts = append(ledParts, "err")
	}
	h.E.Out.Printf("  LEDs:      [%s]\n", strings.Join(ledParts, ", "))

	if s.GearInputEnabled || s.GearInput_us > 0 {
		state := "—"
		if s.GearInput_us > 0 {
			if s.GearInputCommandDeploy {
				state = h.E.Out.C(engine.ColorGreen, "deploy")
			} else {
				state = h.E.Out.C(engine.ColorCyan, "retract")
			}
		}
		h.E.Out.Printf("  GearInput: %dµs (thr=%dµs) → %s\n",
			s.GearInput_us, s.GearInputThreshold_us, state)
	}

	// Per-servo verbose block (mirrors LightFX format). Array index i → servoId i+1.
	// Labels: 1-6 = Nose-A/B, LeftMain-A/B, RightMain-A/B; 7 = Yaw.
	if len(s.Servos) == 7 {
		labels := [7]string{"Nose-A", "Nose-B", "Left-A", "Left-B", "Right-A", "Right-B", "Yaw"}
		for i, c := range s.Servos {
			rev := ""
			if c.Reversed {
				rev = " rev"
			}
			h.E.Out.Printf("    srv%d %-6s: min=%dµs max=%dµs target=%dµs | speed=%d accel=%d decel=%d%s\n",
				i+1, labels[i], c.Min_us, c.Max_us, c.Target_us,
				c.Speed, c.Accel, c.Decel, rev)
		}
	}
}

// FormatCalibStatus is the CLI formatter for GEAR_CALIB_STATUS async events.
func (h *Handler) FormatCalibStatus(c *CalibStatus) {
	gearName := engine.GearIDName(c.GearID)
	phaseName := c.PhaseName
	if phaseName == "" {
		phaseName = fmt.Sprintf("?(%d)", c.Phase)
	}

	if c.Finished {
		switch c.Phase {
		case 7: // ERROR
			reason := ""
			if c.ErrorReason > 0 {
				reason = " — " + c.ErrorReasonName
			}
			h.E.Out.Printf("  %s %-10s calib %s%s\n",
				h.E.Out.C(engine.ColorRed, "✗"), gearName,
				h.E.Out.C(engine.ColorRed, "ERROR"), reason)
		case 8: // CANCELLED
			h.E.Out.Printf("  %s %-10s calib %s\n",
				h.E.Out.C(engine.ColorYellow, "⚠"), gearName,
				h.E.Out.C(engine.ColorYellow, "cancelled"))
		default: // COMPLETE
			result := fmt.Sprintf("stall=%dmA", c.Stall_mA)
			if c.Peak_mA > 0 {
				result += fmt.Sprintf(" peak=%dmA", c.Peak_mA)
			}
			h.E.Out.Printf("  %s %-10s calib complete (%s)\n",
				h.E.Out.C(engine.ColorGreen, "✓"), gearName, result)
		}
		return
	}

	phaseColor := engine.ColorReset
	switch c.Phase {
	case 1, 3, 5:
		phaseColor = engine.ColorCyan
	case 9, 10:
		phaseColor = engine.ColorYellow
	}

	info := []string{fmt.Sprintf("%dmA", c.Current_mA)}
	if c.Peak_mA > 0 {
		info = append(info, fmt.Sprintf("peak %dmA", c.Peak_mA))
	}
	h.E.Out.Printf("  %s %-10s calib %s (%s)\n",
		h.E.Out.C(engine.ColorMagenta, "◆"), gearName,
		h.E.Out.C(phaseColor, phaseName),
		strings.Join(info, ", "))
}

// FormatSeqStatus is the CLI formatter for GEAR_SEQ_STATUS async events.
func (h *Handler) FormatSeqStatus(s *SeqStatus) {
	gearName := engine.GearIDName(s.GearID)
	action := "retract"
	actionColor := engine.ColorCyan
	if s.Deploying {
		action = "deploy"
		actionColor = engine.ColorGreen
	}
	elapsedSec := float64(s.Elapsed_ms) / 1000.0

	if s.Finished {
		if s.Phase == 4 { // SEQ_ERROR
			h.E.Out.Printf("  %s %-10s %s %s in %.1fs\n",
				h.E.Out.C(engine.ColorRed, "✗"), gearName,
				h.E.Out.C(engine.ColorRed, "ERROR"), action, elapsedSec)
		} else {
			h.E.Out.Printf("  %s %-10s %s complete (%.1fs)\n",
				h.E.Out.C(engine.ColorGreen, "✓"), gearName,
				h.E.Out.C(actionColor, action), elapsedSec)
		}
		return
	}

	phaseColor := engine.ColorYellow
	switch s.Phase {
	case 2:
		phaseColor = engine.ColorCyan
	case 5:
		phaseColor = engine.ColorGray
	}
	h.E.Out.Printf("  %s %-10s %s %s (%.1fs)\n",
		h.E.Out.C(engine.ColorMagenta, "▸"), gearName,
		h.E.Out.C(phaseColor, s.PhaseName), action, elapsedSec)
}

// FormatDoorStatus is the CLI formatter for GEAR_DOOR_STATUS async events.
func (h *Handler) FormatDoorStatus(d *DoorStatus) {
	gearName := engine.GearIDName(d.GearID)

	stateColor := engine.ColorYellow
	switch d.State {
	case 1:
		stateColor = engine.ColorCyan
	case 2:
		stateColor = engine.ColorGreen
	}

	posInfo := ""
	if d.HasDoor1 {
		posInfo = fmt.Sprintf(" (%dµs, %dµs)", d.Door0_us, d.Door1_us)
	} else if d.HasDoor0 {
		posInfo = fmt.Sprintf(" (%dµs)", d.Door0_us)
	}

	h.E.Out.Printf("  %s %-10s doors %s%s\n",
		h.E.Out.C(engine.ColorGray, "◇"), gearName,
		h.E.Out.C(stateColor, d.StateName), posInfo)
}
