package lightfx

// ScaleFX Engine - LightFX query-response parsers.
// Broadcast + async-event paths are wired inline in handler.go Register().
// The parsers below are CLI-only display helpers invoked from cmdLedStatus /
// cmdSeqStatus / cmdSeqQueue — they render typed query responses and stay
// here because they aren't fired through the observer chain.

import (
	"fmt"
	"scalefx/engine"
	"scalefx/protocol"
)

// LedSeqEventName returns the protocol event-type label for LED sequences.
func LedSeqEventName(etype byte) string {
	names := []string{"ON", "OFF", "FLASH", "FADE_IN", "FADE_OUT", "FADING", "BEACON"}
	if int(etype) < len(names) {
		return names[etype]
	}
	return fmt.Sprintf("UNKNOWN(0x%02X)", etype)
}

// parseLedStatus renders LED_STATUS_RESP (reply to `led.status`).
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
		h.E.Out.Printf("  CH%d: %s %3d%% | Seq: %s (%d events)\n",
			ch, bar, brightness, seqIcon, seqCount)
	}
}

// parseLedSeqStatus renders LED_SEQ_STATUS_RESP (reply to `seq.status`).
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

	status, statusColor := "STOPPED", engine.ColorYellow
	if playing {
		status, statusColor = "PLAYING", engine.ColorGreen
	}

	h.E.Out.Printf("  ── LED %d Sequence Status ──\n", ch)
	h.E.Out.Printf("  Status:      %s\n", h.E.Out.C(statusColor, status))
	h.E.Out.Printf("  Events:      %d\n", count)
	h.E.Out.Printf("  Current:     %d\n", index)
	h.E.Out.Printf("  Loop Count:  %d\n", loops)
	h.E.Out.Printf("  Brightness:  %d%%\n", brightness)
}

// parseLedSeqQueue renders LED_SEQ_QUEUE_RESP (reply to `seq.queue`).
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
		h.E.Out.Printf("  [%d] %-8s: %dms (param=%d)%s\n",
			i, LedSeqEventName(etype), duration, param1, marker)
	}
}
