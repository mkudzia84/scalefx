package engine

// ScaleFX Engine - LightFX Response Parsers

import (
	"fmt"
	"scalefx/protocol"
	"scalefx/protocol/lightfx"
	"strings"
)

func (e *Engine) parseLightFXStatus(data []byte) {
	if len(data) < 15 {
		e.Out.Printf("  LightFX: (incomplete: %d bytes)\n", len(data))
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

	e.Out.Printf("  ── LightFX ────────────────────\n")

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
		e.Out.Printf("  LEDs:      %s\n", strings.Join(ledParts, ", "))
	} else {
		e.Out.Printf("  LEDs:      all off\n")
	}

	if masterBrightness < 100 {
		e.Out.Printf("  Master:    %d%%\n", masterBrightness)
	}

	e.Out.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	if len(llStates) > 0 {
		var llParts []string
		for i, s := range llStates {
			llParts = append(llParts, fmt.Sprintf("slot%d=%s", i+1, s))
		}
		e.Out.Printf("  Lights:    %s\n", strings.Join(llParts, ", "))
	}

	if len(data) >= 24 {
		batMV := protocol.ReadU16LE(data, 20)
		cellCount := data[22]
		batPct := data[23]
		batV := float64(batMV) / 1000.0
		e.Out.Printf("  Battery:   %.2fV (%d%%, %dS)\n", batV, batPct, cellCount)
	}
}

// ParseLandingLightStatus parses LANDING_LIGHT_STATUS async payload.
func (e *Engine) ParseLandingLightStatus(payload []byte) {
	if len(payload) < 3 {
		if len(payload) > 0 {
			e.Out.Printf("  LandingLightStatus: (incomplete: %d bytes)\n", len(payload))
		}
		return
	}
	slot := payload[0]
	phase := payload[1]
	finished := payload[2] != 0

	phaseName := lightfx.LandingLightPhaseName(phase)

	phaseColor := ColorCyan
	switch phase {
	case 2:
		phaseColor = ColorGreen
	case 0:
		phaseColor = ColorYellow
	}

	if finished {
		e.Out.Printf("  %s Light %d    %s complete\n",
			e.Out.C(ColorGreen, "✓"), slot, e.Out.C(phaseColor, phaseName))
	} else {
		e.Out.Printf("  %s Light %d    %s\n",
			e.Out.C(ColorBlue, "▸"), slot, e.Out.C(phaseColor, phaseName))
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

// ParseLedStatus parses LED_STATUS_RESP payload.
func (e *Engine) ParseLedStatus(payload []byte) {
	if len(payload) < 4 {
		e.Out.Println("  (empty LED status)")
		return
	}
	e.Out.Printf("  ── LED Channel Status ──\n")
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
		e.Out.Printf("  CH%d: %s %3d%% | Seq: %s (%d events)\n", ch, bar, brightness, seqIcon, seqCount)
	}
}

// ParseLedSeqStatus parses LED_SEQ_STATUS_RESP.
func (e *Engine) ParseLedSeqStatus(payload []byte) {
	if len(payload) < 8 {
		e.Out.Println("  (invalid sequence status)")
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
	statusColor := ColorYellow
	if playing {
		status = "PLAYING"
		statusColor = ColorGreen
	}

	e.Out.Printf("  ── LED %d Sequence Status ──\n", ch)
	e.Out.Printf("  Status:      %s\n", e.Out.C(statusColor, status))
	e.Out.Printf("  Events:      %d\n", count)
	e.Out.Printf("  Current:     %d\n", index)
	e.Out.Printf("  Loop Count:  %d\n", loops)
	e.Out.Printf("  Brightness:  %d%%\n", brightness)
}

// ParseLedSeqQueue parses LED_SEQ_QUEUE_RESP.
func (e *Engine) ParseLedSeqQueue(payload []byte) {
	if len(payload) < 5 {
		e.Out.Println("  (invalid sequence queue)")
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
	e.Out.Printf("  ── LED %d Sequence Queue (%s, %d events, brightness %d%%) ──\n",
		ch, status, count, brightness)

	if count == 0 {
		e.Out.Println("  (empty)")
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
		e.Out.Printf("  [%d] %-8s: %dms (param=%d)%s\n", i, LedSeqEventName(etype), duration, param1, marker)
	}
}
