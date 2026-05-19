package client

import (
	"scalefx/protocol/audio"
)

// Audio exposes the hub-side audio subsystem (AudioServicePolicy on the
// firmware) — channel playback, volume / fade, queue, status.
type Audio struct{ c *Client }

// PlayOptions configures an Audio.Play call.
type PlayOptions = audio.PlayOptions

// QueueOptions configures an Audio.Queue call.
type QueueOptions = audio.QueueOptions

// Re-exported wire constants so CLI callers don't pull the audio
// protocol package directly.
const (
	OutputCh1 = audio.OutputCh1
	OutputCh2 = audio.OutputCh2
	OutputAll = audio.OutputAll

	LoopNone     = audio.LoopNone
	LoopFinite   = audio.LoopFinite
	LoopInfinite = audio.LoopInfinite

	QueueFinishLoop = audio.QueueFinishLoop
	QueueStopNow    = audio.QueueStopNow

	ChAll = audio.ChAll
)

// Play starts WAV playback on `opt.Channel`.  `opt.Path` is the file
// path on the hub's storage (absolute, e.g. `/sfx/engine.wav`).
func (a *Audio) Play(opt PlayOptions) error {
	if err := audio.ValidatePath(opt.Path); err != nil {
		return err
	}
	return a.c.sendExpectACK(audio.CmdPlay(opt))
}

// Stop stops `channel` (or everything if channel == ChAll).
func (a *Audio) Stop(channel byte) error {
	return a.c.sendExpectACK(audio.CmdStop(channel))
}

// StopAll stops every active channel.
func (a *Audio) StopAll() error { return a.Stop(ChAll) }

// Volume sets per-channel volume (0..100).  channel == ChAll → master.
func (a *Audio) Volume(channel, vol byte) error {
	return a.c.sendExpectACK(audio.CmdVolume(channel, vol))
}

// MasterVolume sets the global master volume (0..100).
func (a *Audio) MasterVolume(vol byte) error { return a.Volume(ChAll, vol) }

// Fade fade-stops the specified channel.
func (a *Audio) Fade(channel byte) error {
	return a.c.sendExpectACK(audio.CmdFade(channel))
}

// Queue queues a follow-on sound on a channel.  Behavior controls
// whether the currently-playing item is allowed to finish its loop
// (QueueFinishLoop) or is cut off (QueueStopNow).
func (a *Audio) Queue(opt QueueOptions) error {
	if err := audio.ValidatePath(opt.Path); err != nil {
		return err
	}
	return a.c.sendExpectACK(audio.CmdQueue(opt))
}

// QueueClear empties the queue on the given channel.
func (a *Audio) QueueClear(channel byte) error {
	return a.c.sendExpectACK(audio.CmdQueueClear(channel))
}

// MixerState is the decoded AUDIO_STATUS_RESP — currently a raw blob.
// See audio_protocol.h notes on why a fixed shape isn't carved in
// stone here.
type MixerState = audio.MixerState

// Status returns the current mixer state.
func (a *Audio) Status() (MixerState, error) {
	resp, err := a.c.sendForResp(audio.CmdStatusReq(), audio.AudioStatusResp)
	if err != nil {
		return MixerState{}, err
	}
	return audio.DecodeAudioStatus(resp.Payload), nil
}

// CodecState is the decoded CODEC_STATUS_RESP — raw, vendor-specific.
type CodecState = audio.CodecState

// CodecStatus returns the codec hardware state.
func (a *Audio) CodecStatus() (CodecState, error) {
	resp, err := a.c.sendForResp(audio.CmdCodecStatusReq(), audio.CodecStatusResp)
	if err != nil {
		return CodecState{}, err
	}
	return audio.DecodeCodecStatus(resp.Payload), nil
}
