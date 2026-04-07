package main

// ScaleFX CLI - LightFX Response Parsers

import (
	"fmt"
	"scalefx/protocol"
	"scalefx/protocol/lightfx"
	"strings"
)

func parseLightFXStatus(data []byte) {
	// Wire format (24 bytes):
	//   [ledBrightness:u8×8][ledSeqFlags:u8]
	//   [servo0:u16][servo1:u16][servo2:u16]
	//   [landingLightStates:u8×3]
	//   [masterBrightness_pct:u8]
	//   [ledEnabledFlags:u8]
	//   [batteryV_mV:u16LE][cellCount:u8][batteryPct:u8]
	if len(data) < 15 {
		fmt.Printf("  LightFX: (incomplete: %d bytes)\n", len(data))
		return
	}

	// LED channels
	ledBrightness := make([]byte, 8)
	copy(ledBrightness, data[:8])
	seqFlags := data[8]

	// Servos
	servo0 := protocol.ReadU16LE(data, 9)
	servo1 := protocol.ReadU16LE(data, 11)
	servo2 := protocol.ReadU16LE(data, 13)

	// Landing light states (optional)
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

	// Master brightness (optional)
	masterBrightness := byte(100)
	if len(data) >= 19 {
		masterBrightness = data[18]
	}

	// Enabled flags (optional)
	enabledFlags := byte(0xFF)
	if len(data) >= 20 {
		enabledFlags = data[19]
	}

	fmt.Printf("  ── LightFX ────────────────────\n")

	// LED status (compact format with enabled/disabled indicators)
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
		fmt.Printf("  LEDs:      %s\n", strings.Join(ledParts, ", "))
	} else {
		fmt.Printf("  LEDs:      all off\n")
	}

	// Master brightness (only show if not 100%)
	if masterBrightness < 100 {
		fmt.Printf("  Master:    %d%%\n", masterBrightness)
	}

	// Servos
	fmt.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	// Landing lights
	if len(llStates) > 0 {
		var llParts []string
		for i, s := range llStates {
			llParts = append(llParts, fmt.Sprintf("slot%d=%s", i+1, s))
		}
		fmt.Printf("  Lights:    %s\n", strings.Join(llParts, ", "))
	}

	// Battery (optional, bytes 20-23)
	if len(data) >= 24 {
		batMV := protocol.ReadU16LE(data, 20)
		cellCount := data[22]
		batPct := data[23]
		batV := float64(batMV) / 1000.0
		fmt.Printf("  Battery:   %.2fV (%d%%, %dS)\n", batV, batPct, cellCount)
	}
}

// ParseLandingLightStatus parses LANDING_LIGHT_STATUS async payload.
// Wire format (3 bytes): [slot:u8][phase:u8][finished:u8]
func ParseLandingLightStatus(payload []byte) {
	if len(payload) < 3 {
		if len(payload) > 0 {
			fmt.Printf("  LandingLightStatus: (incomplete: %d bytes)\n", len(payload))
		}
		return
	}
	slot := payload[0]
	phase := payload[1]
	finished := payload[2] != 0

	phaseName := lightfx.LandingLightPhaseName(phase)

	// Color based on state
	phaseColor := colorCyan
	switch phase {
	case 2: // DEPLOYED
		phaseColor = colorGreen
	case 0: // RETRACTED
		phaseColor = colorYellow
	}

	if finished {
		fmt.Printf("  %s✓%s Light %d    %s%s%s complete\n",
			colorGreen, colorReset, slot, phaseColor, phaseName, colorReset)
	} else {
		fmt.Printf("  %s▸%s Light %d    %s%s%s\n",
			colorBlue, colorReset, slot, phaseColor, phaseName, colorReset)
	}
}

// ─── LightFX Response Parsers ───

// LedSeqEventName returns event type name.
func LedSeqEventName(etype byte) string {
	names := []string{"ON", "OFF", "FLASH", "FADE_IN", "FADE_OUT", "FADING", "BEACON"}
	if int(etype) < len(names) {
		return names[etype]
	}
	return fmt.Sprintf("UNKNOWN(0x%02X)", etype)
}

// ParseLedStatus parses LED_STATUS_RESP payload: [ch:u8][brightness:u8][seq_playing:u8][seq_count:u8] per channel.
func ParseLedStatus(payload []byte) {
	if len(payload) < 4 {
		fmt.Println("  (empty LED status)")
		return
	}
	fmt.Printf("  ── LED Channel Status ──\n")
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
		fmt.Printf("  CH%d: %s %3d%% | Seq: %s (%d events)\n", ch, bar, brightness, seqIcon, seqCount)
	}
}

// ParseLedSeqStatus parses LED_SEQ_STATUS_RESP: [ch:u8][playing:u8][count:u8][index:u8][loops:u32LE][brightness:u8].
func ParseLedSeqStatus(payload []byte) {
	if len(payload) < 8 {
		fmt.Println("  (invalid sequence status)")
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
	statusColor := colorYellow
	if playing {
		status = "PLAYING"
		statusColor = colorGreen
	}

	fmt.Printf("  ── LED %d Sequence Status ──\n", ch)
	fmt.Printf("  Status:      %s%s%s\n", statusColor, status, colorReset)
	fmt.Printf("  Events:      %d\n", count)
	fmt.Printf("  Current:     %d\n", index)
	fmt.Printf("  Loop Count:  %d\n", loops)
	fmt.Printf("  Brightness:  %d%%\n", brightness)
}

// ParseLedSeqQueue parses LED_SEQ_QUEUE_RESP: [ch:u8][count:u8][index:u8][playing:u8][brightness:u8] + events.
func ParseLedSeqQueue(payload []byte) {
	if len(payload) < 5 {
		fmt.Println("  (invalid sequence queue)")
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
	fmt.Printf("  ── LED %d Sequence Queue (%s, %d events, brightness %d%%) ──\n",
		ch, status, count, brightness)

	if count == 0 {
		fmt.Println("  (empty)")
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
		fmt.Printf("  [%d] %-8s: %dms (param=%d)%s\n", i, LedSeqEventName(etype), duration, param1, marker)
	}
}
