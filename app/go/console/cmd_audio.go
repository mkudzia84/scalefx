package console

import (
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"

	"scalefx/protocol/audio"
	"scalefx/protocol/core"
)

func init() {
	register(&command{Name: "play", Usage: "play <ch> <path> [vol] [output(1|2|all)] [loops(0=inf|N)]", Help: "start playback on a mixer channel", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdPlay})
	register(&command{Name: "audio-stop", Usage: "audio-stop [ch|all]", Help: "stop one channel or all", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdAudioStop})
	register(&command{Name: "volume", Usage: "volume <ch|master> <0-100>", Help: "set channel or master volume", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdVolume})
	register(&command{Name: "fade", Usage: "fade <ch>", Help: "fade-stop a channel", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdFade})
	register(&command{Name: "queue", Usage: "queue <ch> <path> [now|finish]", Help: "queue a follow-on sound", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdQueue})
	register(&command{Name: "queue-clear", Usage: "queue-clear <ch|all>", Help: "clear queue on a channel", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdQueueClear})
	register(&command{Name: "audio-status", Usage: "audio-status", Help: "raw AUDIO_STATUS_RESP payload", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdAudioStatus})
	register(&command{Name: "codec-status", Usage: "codec-status", Help: "raw CODEC_STATUS_RESP payload", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdCodecStatus})
	register(&command{Name: "audio-preloads", Usage: "audio-preloads", Help: "PSRAM asset cache: per-path residency + owners", Category: catAudio, RequiresConn: true, RequiresCap: core.CapAudio, Run: cmdAudioPreloads})
}

func cmdPlay(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: play <ch> <path> [vol] [output(1|2|all)]")
	}
	ch, err := parseU8(args[0])
	if err != nil {
		return err
	}
	opt := audio.PlayOptions{Channel: ch, Path: args[1], Volume: 100, Output: audio.OutputAll}
	if len(args) >= 3 {
		v, err := parseU8(args[2])
		if err != nil {
			return err
		}
		opt.Volume = v
	}
	if len(args) >= 4 {
		switch args[3] {
		case "1":
			opt.Output = audio.OutputCh1
		case "2":
			opt.Output = audio.OutputCh2
		case "all":
			opt.Output = audio.OutputAll
		default:
			return fmt.Errorf("output must be 1|2|all")
		}
	}
	// Optional loop arg: "0" or "inf" = infinite loop, N>0 = play N times total.
	// Maps to audio.PlayOptions.LoopMode + LoopCount on the wire.
	loopTag := ""
	if len(args) >= 5 {
		if args[4] == "inf" || args[4] == "0" {
			opt.LoopMode = audio.LoopInfinite
			loopTag = "  loop=inf"
		} else {
			v, err := strconv.ParseUint(args[4], 0, 16)
			if err != nil {
				return fmt.Errorf("loops: %v", err)
			}
			opt.LoopMode = audio.LoopFinite
			opt.LoopCount = uint16(v)
			loopTag = fmt.Sprintf("  loop=%d", opt.LoopCount)
		}
	}
	if err := a.c.Audio.Play(opt); err != nil {
		return err
	}
	Ok("ch%d ▶ %s  vol=%d out=%s%s", ch, Quote(opt.Path), opt.Volume, outputName(opt.Output), loopTag)
	return nil
}

func cmdAudioStop(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	ch := audio.ChAll
	if len(args) >= 1 && args[0] != "all" {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		ch = v
	}
	if err := a.c.Audio.Stop(ch); err != nil {
		return err
	}
	if ch == audio.ChAll {
		Ok("stopped all channels")
	} else {
		Ok("stopped ch%d", ch)
	}
	return nil
}

func cmdVolume(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 2 {
		return fmt.Errorf("usage: volume <ch|master> <0-100>")
	}
	ch := audio.ChAll
	label := "master"
	if args[0] != "master" {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		ch = v
		label = fmt.Sprintf("ch%d", ch)
	}
	vol, err := parseU8(args[1])
	if err != nil {
		return err
	}
	if err := a.c.Audio.Volume(ch, vol); err != nil {
		return err
	}
	Ok("%s volume → %d%%", label, vol)
	return nil
}

func cmdFade(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: fade <ch>")
	}
	ch, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Audio.Fade(ch); err != nil {
		return err
	}
	Ok("ch%d fading out", ch)
	return nil
}

func cmdQueue(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: queue <ch> <path> [now|finish]")
	}
	ch, err := parseU8(args[0])
	if err != nil {
		return err
	}
	opt := audio.QueueOptions{Channel: ch, Path: args[1], Volume: 100, Behavior: audio.QueueFinishLoop}
	if len(args) >= 3 {
		switch args[2] {
		case "now":
			opt.Behavior = audio.QueueStopNow
		case "finish":
			opt.Behavior = audio.QueueFinishLoop
		default:
			return fmt.Errorf("behavior must be now|finish")
		}
	}
	if err := a.c.Audio.Queue(opt); err != nil {
		return err
	}
	Ok("ch%d queued %s (%s)", ch, Quote(opt.Path), queueBehaviorName(opt.Behavior))
	return nil
}

func cmdQueueClear(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	ch := audio.ChAll
	if len(args) >= 1 && args[0] != "all" {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		ch = v
	}
	if err := a.c.Audio.QueueClear(ch); err != nil {
		return err
	}
	if ch == audio.ChAll {
		Ok("queue cleared on every channel")
	} else {
		Ok("ch%d queue cleared", ch)
	}
	return nil
}

func cmdAudioStatus(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Audio.Status()
	if err != nil {
		return err
	}
	Hdr("audio mixer")
	KVf("format", "%d Hz · %d-bit · %d ch max",
		s.SampleRateHz, s.BitDepth, s.MaxChannels)
	KV("codec", cCyan(s.CodecName))
	KVf("master vol", "%d%%", s.MasterVolPct)
	flagsLine := []string{}
	if s.Initialized() {
		flagsLine = append(flagsLine, cGreen("init"))
	} else {
		flagsLine = append(flagsLine, cRed("not-init"))
	}
	if s.I2SRunning() {
		flagsLine = append(flagsLine, cGreen("i2s-running"))
	} else {
		flagsLine = append(flagsLine, cYellow("i2s-stopped"))
	}
	KV("flags", strings.Join(flagsLine, cDim(" · ")))

	KVf("ring fill", "%d%%  (avail R=%d  W=%d frames, cap=%d)",
		s.RingFillPct, s.RingAvailRead, s.RingAvailWrite, s.RingFrames)
	KVf("underruns", "%d  consume loops=%d  frames=%d",
		s.Underruns, s.ConsumeLoops, s.ConsumeFrames)
	KVf("wav buffer", "%d frames per channel", s.WavBufFrames)

	if len(s.Channels) == 0 {
		KV("active", cDim("(no channels playing)"))
		return nil
	}
	KVf("active mask", "0x%02X  (%d ch)", s.ActiveMask, len(s.Channels))
	for _, c := range s.Channels {
		state := Phase("idle")
		if c.Playing {
			state = Phase("playing")
		}
		loopTag := ""
		if c.Looping {
			loopTag = cDim(fmt.Sprintf("  loop=%d", c.LoopCount))
		}
		fmt.Fprintf(out, "  ch%-2d %s  vol=%d%%  out=%s  remain=%s%s  wav=%dHz/%dch/%db  fill=%d%%  %s\n",
			c.Channel,
			state,
			c.VolumePct,
			outputName(c.OutputMask),
			humanDurationMs(c.RemainingMs),
			loopTag,
			c.WavRateHz, c.WavChannels, c.WavBits,
			c.WavBufFillPct,
			cBold(c.Filename))
	}
	return nil
}

func cmdAudioPreloads(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Audio.PreloadStatus()
	if err != nil {
		return err
	}
	Hdr("asset preload cache")
	KVf("budget", "%s / %s  (%d%%)",
		humanBytes(uint64(s.ResidentBytes)),
		humanBytes(uint64(s.BudgetBytes)),
		percent(s.ResidentBytes, s.BudgetBytes))
	KVf("entries", "%d  (ready=%d  loading=%d  failed=%d  pinned=%d)",
		len(s.Entries), s.Ready, s.Loading, s.Failed, s.Pinned)
	if len(s.Entries) == 0 {
		KV("cache", cDim("(empty — no assets registered)"))
		return nil
	}
	Hdr("entries")
	for _, e := range s.Entries {
		owners := strings.Join(e.Owners, ",")
		if owners == "" {
			owners = cDim("lru")
		}
		colour := cDim
		switch e.Status {
		case audio.PreloadReady:
			colour = cGreen
		case audio.PreloadLoading:
			colour = cYellow
		case audio.PreloadFailed:
			colour = cRed
		}
		fmt.Fprintf(out, "  %s %s  %-40s  %s/%s  (%d%%)  [%s]\n",
			colour(fmt.Sprintf("%-7s", e.StatusName)),
			cDim(fmt.Sprintf("%-3s", e.FormatName)),
			e.Path,
			humanBytes(uint64(e.LoadedBytes)),
			humanBytes(uint64(e.TotalBytes)),
			e.PercentLoaded(),
			owners)
	}
	return nil
}

// percent computes (num * 100 / den), clamped to [0,100].
func percent(num, den uint32) int {
	if den == 0 {
		return 0
	}
	p := int(uint64(num) * 100 / uint64(den))
	if p > 100 {
		return 100
	}
	return p
}

func cmdCodecStatus(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Audio.CodecStatus()
	if err != nil {
		return err
	}
	Hdr("codec")
	KVf("type", "0x%02X", s.CodecType)
	KV("initialised", Bool(s.Initialized))
	KV("I²C", Bool(s.I2COk))
	KVf("pins", "SDA=GPIO%d  SCL=GPIO%d", s.SdaPin, s.SclPin)
	KVf("supply", "code=%d", s.SupplyVoltage)
	KV("muted", Bool(s.Muted))
	KVf("digital vol", "0x%02X", s.DigitalVol)
	KVf("device ctrl", "0x%02X", s.DeviceCtrl)
	KVf("faults", "0x%02X", s.FaultStatus)
	return nil
}

// Compile-time keep-alive — hex.EncodeToString is still used elsewhere.
var _ = hex.EncodeToString

func outputName(mask byte) string {
	switch mask {
	case audio.OutputCh1:
		return "1"
	case audio.OutputCh2:
		return "2"
	case audio.OutputAll:
		return "all"
	}
	return fmt.Sprintf("0x%02X", mask)
}

func queueBehaviorName(b byte) string {
	if b == audio.QueueStopNow {
		return "stop now"
	}
	return "finish current loop"
}
