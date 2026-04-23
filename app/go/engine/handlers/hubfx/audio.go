package hubfx

// Audio mixer + DAC codec commands: play/stop/volume/fade/queue across the
// HubFX stereo channels and the on-board codec status query.

import (
	"fmt"
	"scalefx/engine"
	hfxp "scalefx/protocol/hubfx"
	"strings"
)

func (h *Handler) cmdAudioPlay(args []string) {
	if !h.E.RequireArgs(args, 2, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]") {
		return
	}
	ch := byte(engine.Atoi(args[0]))
	path := args[1]
	vol := byte(100)
	output := byte(hfxp.AudioOutputAll)
	loopMode := byte(hfxp.AudioLoopNone)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		switch arg {
		case "ch1":
			output = hfxp.AudioOutputCh1
		case "ch2":
			output = hfxp.AudioOutputCh2
		case "all":
			output = hfxp.AudioOutputAll
		case "loop":
			if i+1 < len(args) {
				i++
				if strings.ToLower(args[i]) == "inf" {
					loopMode = hfxp.AudioLoopInfinite
				} else {
					loopMode = hfxp.AudioLoopFinite
					loopCount = uint16(engine.Atoi(args[i]))
				}
			} else {
				loopMode = hfxp.AudioLoopInfinite
			}
		default:
			if n := engine.Atoi(arg); n > 0 {
				vol = byte(n)
			}
		}
		i++
	}

	outputName := ""
	switch output {
	case hfxp.AudioOutputCh1:
		outputName = " [CH1]"
	case hfxp.AudioOutputCh2:
		outputName = " [CH2]"
	}
	loopStr := ""
	if loopMode == hfxp.AudioLoopInfinite {
		loopStr = " (loop inf)"
	} else if loopMode == hfxp.AudioLoopFinite {
		loopStr = fmt.Sprintf(" (loop x%d)", loopCount)
	}
	h.E.Ack(h.E.API.HubFx.AudioPlay(ch, vol, output, loopMode, loopCount, path),
		fmt.Sprintf("Play ch%d: %s vol=%d%%%s%s", ch, path, vol, outputName, loopStr))
}

func (h *Handler) cmdAudioStop(args []string) {
	ch := byte(hfxp.AudioChAll)
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = hfxp.AudioChAll
		} else {
			ch = byte(engine.Atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != hfxp.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	h.E.Ack(h.E.API.HubFx.AudioStop(ch), fmt.Sprintf("Audio stop %s", target))
}

func (h *Handler) cmdAudioVol(args []string) {
	if !h.E.RequireArgs(args, 2, "audio.volume <ch|master> <volume>") {
		return
	}
	ch := byte(0)
	if strings.ToLower(args[0]) == "master" {
		ch = hfxp.AudioChAll
	} else {
		ch = byte(engine.Atoi(args[0]))
	}
	vol := byte(engine.Atoi(args[1]))
	target := "master"
	if ch != hfxp.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	h.E.Ack(h.E.API.HubFx.AudioVolume(ch, vol), fmt.Sprintf("Volume %s → %d%%", target, vol))
}

func (h *Handler) cmdAudioFade(args []string) {
	if !h.E.RequireArgs(args, 1, "audio.fade <ch>") {
		return
	}
	h.E.Ack(h.E.API.HubFx.AudioFade(byte(engine.Atoi(args[0]))), fmt.Sprintf("Fade out ch%s", args[0]))
}

func (h *Handler) cmdAudioQueue(args []string) {
	if !h.E.RequireArgs(args, 2, "audio.queue <ch> <path> [vol] [loop N]") {
		return
	}
	ch := byte(engine.Atoi(args[0]))
	path := args[1]
	vol := byte(100)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		if arg == "loop" && i+1 < len(args) {
			i++
			loopCount = uint16(engine.Atoi(args[i]))
		} else if n := engine.Atoi(arg); n > 0 {
			vol = byte(n)
		}
		i++
	}

	loopBehavior := byte(hfxp.AudioQueueFinishLoop)
	h.E.Ack(h.E.API.HubFx.AudioQueue(ch, vol, loopCount, loopBehavior, path),
		fmt.Sprintf("Queue ch%d: %s vol=%d%%", ch, path, vol))
}

func (h *Handler) cmdAudioClear(args []string) {
	ch := byte(hfxp.AudioChAll)
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = hfxp.AudioChAll
		} else {
			ch = byte(engine.Atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != hfxp.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	h.E.Ack(h.E.API.HubFx.AudioQueueClear(ch), fmt.Sprintf("Queue cleared %s", target))
}

func (h *Handler) cmdAudioStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.AudioStatus(), h.parseAudioStatus)
}

func (h *Handler) cmdCodecStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.CodecStatus(), h.parseCodecStatus)
}
