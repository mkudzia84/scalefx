package lightfx

// ScaleFX Engine - LightFX CLI console formatters.

import (
	"fmt"
	"scalefx/engine"
	"strings"
)

// FormatStatusBroadcast renders a decoded LightFX STATUS payload. Shared by
// the synchronous `status` response and opt-in broadcast observers.
func (h *Handler) FormatStatusBroadcast(s *StatusBroadcast) {
	h.E.Out.Printf("  ── LightFX ────────────────────\n")

	var ledParts []string
	for i := 0; i < 8; i++ {
		ch := i + 1
		bri := s.LedBrightness[i]
		seq := s.SeqPlaying[i]
		if !s.Enabled[i] {
			ledParts = append(ledParts, fmt.Sprintf("ch%d=%d[DIS]", ch, bri))
		} else if bri > 0 || seq {
			seqMark := ""
			if seq {
				seqMark = "▶"
			}
			ledParts = append(ledParts, fmt.Sprintf("ch%d=%d%s", ch, bri, seqMark))
		}
	}
	if len(ledParts) > 0 {
		h.E.Out.Printf("  LEDs:      %s\n", strings.Join(ledParts, ", "))
	} else {
		h.E.Out.Printf("  LEDs:      all off\n")
	}

	if s.MasterBrightness < 100 {
		h.E.Out.Printf("  Master:    %d%%\n", s.MasterBrightness)
	}

	h.E.Out.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", s.Servo0_us, s.Servo1_us, s.Servo2_us)

	var llParts []string
	for i := 0; i < 3; i++ {
		name := s.LandingSlots[i].PhaseName
		if name == "" {
			name = fmt.Sprintf("?(%d)", s.LandingSlots[i].Phase)
		}
		llParts = append(llParts, fmt.Sprintf("slot%d=%s", i+1, name))
	}
	h.E.Out.Printf("  Lights:    %s\n", strings.Join(llParts, ", "))

	if s.BatteryPresent {
		batV := float64(s.Battery_mV) / 1000.0
		h.E.Out.Printf("  Battery:   %.2fV (%d%%, %dS)\n", batV, s.BatteryPct, s.CellCount)
	}
}

// FormatLandingLightStatus is the CLI formatter for LANDING_LIGHT_STATUS.
func (h *Handler) FormatLandingLightStatus(ll *LandingLightStatus) {
	phaseColor := engine.ColorCyan
	switch ll.Phase {
	case 2:
		phaseColor = engine.ColorGreen
	case 0:
		phaseColor = engine.ColorYellow
	}

	if ll.Finished {
		h.E.Out.Printf("  %s Light %d    %s complete\n",
			h.E.Out.C(engine.ColorGreen, "✓"), ll.Slot,
			h.E.Out.C(phaseColor, ll.PhaseName))
	} else {
		h.E.Out.Printf("  %s Light %d    %s\n",
			h.E.Out.C(engine.ColorBlue, "▸"), ll.Slot,
			h.E.Out.C(phaseColor, ll.PhaseName))
	}
}
