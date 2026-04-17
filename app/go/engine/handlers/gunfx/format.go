package gunfx

// ScaleFX Engine - GunFX CLI console formatter.

import (
	"fmt"
	"scalefx/engine"
	"strings"
)

// FormatStatusBroadcast renders a decoded GunFX STATUS payload to the engine's
// console. Shared by the synchronous `status` response and by broadcast
// observers that opt in.
func (h *Handler) FormatStatusBroadcast(s *StatusBroadcast) {
	var stateParts []string
	if s.Firing {
		stateParts = append(stateParts, h.E.Out.C(engine.ColorRed, "FIRING"))
	}
	if s.FlashActive {
		stateParts = append(stateParts, "FLASH")
	}
	if s.FlashFading {
		stateParts = append(stateParts, "FADING")
	}
	if s.HeaterOn {
		stateParts = append(stateParts, h.E.Out.C(engine.ColorYellow, "HEATER"))
	}
	if s.FanOn {
		stateParts = append(stateParts, "FAN")
	}
	if s.FanSpindown {
		stateParts = append(stateParts, "SPINDOWN")
	}
	stateStr := "IDLE"
	if len(stateParts) > 0 {
		stateStr = strings.Join(stateParts, ", ")
	}

	h.E.Out.Printf("  ── GunFX ──────────────────────\n")
	h.E.Out.Printf("  State:     %s\n", stateStr)

	if s.Firing {
		h.E.Out.Printf("  Fire rate: %d RPM\n", s.RPM)
	}
	h.E.Out.Printf("  Shots:     %d\n", s.Shots)

	if s.FanOn || s.FanSpindown {
		fanInfo := fmt.Sprintf("speed=%d", s.FanSpeed)
		if s.FanSpindown && s.FanOff_ms > 0 {
			fanInfo += fmt.Sprintf(", off in %dms", s.FanOff_ms)
		}
		h.E.Out.Printf("  Fan:       %s\n", fanInfo)
	}

	if s.Heater_ms > 0 {
		heaterSec := float64(s.Heater_ms) / 1000.0
		h.E.Out.Printf("  Heater:    %.1fs total\n", heaterSec)
	}

	h.E.Out.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", s.Servo0_us, s.Servo1_us, s.Servo2_us)

	if s.HeaterError != 0 || s.FanError != 0 {
		h.E.Out.Printf("  ── Smoke Errors ──────────────\n")
		if s.HeaterError != 0 {
			h.E.Out.Printf("  Heater:    %s\n", h.E.Out.C(engine.ColorRed, s.HeaterErrorName))
		}
		if s.FanError != 0 {
			h.E.Out.Printf("  Fan:       %s\n", h.E.Out.C(engine.ColorRed, s.FanErrorName))
		}
	}

	if s.HeaterDuty < 255 || s.FanDuty < 255 {
		h.E.Out.Printf("  ── Overcurrent Throttle ──────\n")
		if s.HeaterDuty < 255 {
			pct := int(s.HeaterDuty) * 100 / 255
			h.E.Out.Printf("  Heater:    %s\n", h.E.Out.C(engine.ColorYellow,
				fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, s.HeaterDuty)))
		}
		if s.FanDuty < 255 {
			pct := int(s.FanDuty) * 100 / 255
			h.E.Out.Printf("  Fan:       %s\n", h.E.Out.C(engine.ColorYellow,
				fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, s.FanDuty)))
		}
	}

	if s.BatteryPresent {
		batteryV := float64(s.Battery_mV) / 1000.0
		battParts := []string{fmt.Sprintf("%.2fV (%dmV)", batteryV, s.Battery_mV)}
		if s.CellCount > 0 {
			battParts = append(battParts, fmt.Sprintf("%dS", s.CellCount))
		}
		if s.BatteryPct > 0 {
			pctColor := engine.ColorGreen
			if s.BatteryPct <= 10 {
				pctColor = engine.ColorRed
			} else if s.BatteryPct <= 30 {
				pctColor = engine.ColorYellow
			}
			battParts = append(battParts, h.E.Out.C(pctColor, fmt.Sprintf("%d%%", s.BatteryPct)))
		}
		h.E.Out.Printf("  Battery:   %s\n", strings.Join(battParts, ", "))
	}
}
