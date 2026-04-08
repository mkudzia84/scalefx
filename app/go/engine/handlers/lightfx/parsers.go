package lightfx

// ScaleFX Engine - LightFX Response Parsers

import (
	"fmt"
	"scalefx/engine"
	"scalefx/protocol"
	lfxp "scalefx/protocol/lightfx"
	"strings"
)

func (h *Handler) parseLightFXStatus(data []byte) {
	if len(data) < 15 {
		h.E.Out.Printf("  LightFX: (incomplete: %d bytes)\n", len(data))
		return
	}

	ledBrightness := make([]byte, 8)
	copy(ledBrightness, data[:8])
	seqFlags := data[8]

	servo0 := protocol.ReadU16LE(data, 9)
	servo1 := protocol.ReadU16LE(data, 11)
	servo2 := protocol.ReadU16LE(data, 13)

	llPhaseNames := map[byte]string{0: "RET", 1: "DEPLOYING", 2: "DEP", 3: "RETRACTING"}
	var llStates []string
	if len(data) >= 18 {
		for i := 0; i < 3; i++ {
			phase := data[15+i]
			name := llPhaseNames[phase]
			if name == "" {
				name = fmt.Sprintf("?(%d)", phase)
			}
			llStates = append(llStates, name)
		}
	}

	masterBrightness := byte(100)
	if len(data) >= 19 {
		masterBrightness = data[18]
	}

	enabledFlags := byte(0xFF)
	if len(data) >= 20 {
		enabledFlags = data[19]
	}

	h.E.Out.Printf("  ── LightFX ────────────────────\n")

	var ledParts []string
	for i := 0; i < 8; i++ {
		ch := i + 1
		bri := ledBrightness[i]
		seq := seqFlags&(1<<i) != 0
		enabled := enabledFlags&(1<<i) != 0
		if !enabled {
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

	if masterBrightness < 100 {
		h.E.Out.Printf("  Master:    %d%%\n", masterBrightness)
	}

	h.E.Out.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	if len(llStates) > 0 {
		var llParts []string
		for i, s := range llStates {
			llParts = append(llParts, fmt.Sprintf("slot%d=%s", i+1, s))
		}
		h.E.Out.Printf("  Lights:    %s\n", strings.Join(llParts, ", "))
	}

	if len(data) >= 24 {
		batMV := protocol.ReadU16LE(data, 20)
		cellCount := data[22]
		batPct := data[23]
		batV := float64(batMV) / 1000.0
		h.E.Out.Printf("  Battery:   %.2fV (%d%%, %dS)\n", batV, batPct, cellCount)
	}
}

// parseLandingLightStatus parses LANDING_LIGHT_STATUS async payload.
func (h *Handler) parseLandingLightStatus(payload []byte) {
	if len(payload) < 3 {
		if len(payload) > 0 {
			h.E.Out.Printf("  LandingLightStatus: (incomplete: %d bytes)\n", len(payload))
		}
		return
	}
	slot := payload[0]
	phase := payload[1]
	finished := payload[2] != 0

	phaseName := lfxp.LandingLightPhaseName(phase)

	phaseColor := engine.ColorCyan
	switch phase {
	case 2:
		phaseColor = engine.ColorGreen
	case 0:
		phaseColor = engine.ColorYellow
	}

	if finished {
		h.E.Out.Printf("  %s Light %d    %s complete\n",
			h.E.Out.C(engine.ColorGreen, "✓"), slot, h.E.Out.C(phaseColor, phaseName))
	} else {
		h.E.Out.Printf("  %s Light %d    %s\n",
			h.E.Out.C(engine.ColorBlue, "▸"), slot, h.E.Out.C(phaseColor, phaseName))
	}
}

// LedSeqEventName returns event type name.
func LedSeqEventName(etype byte) string {
	names := []string{"ON", "OFF", "FLASH", "FADE_IN", "FADE_OUT", "FADING", "BEACON"}
	if int(etype) < len(names) {
		return names[etype]
	}
	return fmt.Sprintf("UNKNOWN(0x%02X)", etype)
}

// parseLedStatus parses LED_STATUS_RESP payload.
func (h *Handler) parseLedStatus(payload []byte) {
	if len(payload) < 4 {
		h.E.Out.Println("  (empty LED status)")
		return
	}
	h.E.Out.Printf("  ── LED Channel Status ──\n")
	for i := 0; i+4 <= len(payload); i += 4 {
		ch := payload[i]
		brightness := payload[i+1]
		seqPlaying := payload[i+2] != 0
		seqCount := payload[i+3]

		filled := int(brightness) * 8 / 100
		if brightness > 0 && filled == 0 {
			filled = 1
		}
		bar := ""
		for j := 0; j < 8; j++ {
			if j < filled {
				bar += "█"
			} else {
				bar += "░"
			}
		}
		seqIcon := "■"
		if seqPlaying {
			seqIcon = "▶"
		}
		h.E.Out.Printf("  CH%d: %s %3d%% | Seq: %s (%d events)\n", ch, bar, brightness, seqIcon, seqCount)
	}
}

// parseLedSeqStatus parses LED_SEQ_STATUS_RESP.
func (h *Handler) parseLedSeqStatus(payload []byte) {
	if len(payload) < 8 {
		h.E.Out.Println("  (invalid sequence status)")
		return
	}
	ch := payload[0]
	playing := payload[1] != 0
	count := payload[2]
	index := payload[3]
	loops := protocol.ReadU32LE(payload, 4)
	brightness := byte(0)
	if len(payload) >= 9 {
		brightness = payload[8]
	}

	status := "STOPPED"
	statusColor := engine.ColorYellow
	if playing {
		status = "PLAYING"
		statusColor = engine.ColorGreen
	}

	h.E.Out.Printf("  ── LED %d Sequence Status ──\n", ch)
	h.E.Out.Printf("  Status:      %s\n", h.E.Out.C(statusColor, status))
	h.E.Out.Printf("  Events:      %d\n", count)
	h.E.Out.Printf("  Current:     %d\n", index)
	h.E.Out.Printf("  Loop Count:  %d\n", loops)
	h.E.Out.Printf("  Brightness:  %d%%\n", brightness)
}

// parseLedSeqQueue parses LED_SEQ_QUEUE_RESP.
func (h *Handler) parseLedSeqQueue(payload []byte) {
	if len(payload) < 5 {
		h.E.Out.Println("  (invalid sequence queue)")
		return
	}
	ch := payload[0]
	count := payload[1]
	index := payload[2]
	playing := payload[3] != 0
	brightness := payload[4]

	status := "STOPPED"
	if playing {
		status = "PLAYING"
	}
	h.E.Out.Printf("  ── LED %d Sequence Queue (%s, %d events, brightness %d%%) ──\n",
		ch, status, count, brightness)

	if count == 0 {
		h.E.Out.Println("  (empty)")
		return
	}

	for i := 0; i < int(count); i++ {
		offset := 5 + (i * 4)
		if offset+4 > len(payload) {
			break
		}
		etype := payload[offset]
		duration := protocol.ReadU16LE(payload, offset+1)
		param1 := payload[offset+3]
		marker := ""
		if byte(i) == index {
			marker = " ← current"
		}
		h.E.Out.Printf("  [%d] %-8s: %dms (param=%d)%s\n", i, LedSeqEventName(etype), duration, param1, marker)
	}
}
