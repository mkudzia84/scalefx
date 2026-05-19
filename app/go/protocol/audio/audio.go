// Package audio mirrors serial/audio/audio_protocol.h — the hub-side
// audio subsystem (AudioServicePolicy on the firmware) wire commands.
package audio

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol"
)

// ─── Packet types ─────────────────────────────────────────────────────

const (
	AudioPlay       protocol.PacketType = 0x84
	AudioStop       protocol.PacketType = 0x85
	AudioVolume     protocol.PacketType = 0x86
	AudioFade       protocol.PacketType = 0x87
	AudioQueue      protocol.PacketType = 0x88
	AudioQueueClear protocol.PacketType = 0x89
	AudioStatusReq  protocol.PacketType = 0x8A
	AudioStatusResp protocol.PacketType = 0x8B
	CodecStatusReq  protocol.PacketType = 0xAA
	CodecStatusResp protocol.PacketType = 0xAB
)

// ─── Wire constants ───────────────────────────────────────────────────

const (
	OutputCh1 byte = 0x01
	OutputCh2 byte = 0x02
	OutputAll byte = OutputCh1 | OutputCh2

	LoopNone     byte = 0
	LoopFinite   byte = 1
	LoopInfinite byte = 2

	QueueFinishLoop byte = 0
	QueueStopNow    byte = 1

	ChAll       byte = 0xFF
	MaxChannels byte = 8
)

// ─── Error codes ──────────────────────────────────────────────────────

const (
	ErrAudioFailure   protocol.ErrorCode = 0x85
	ErrInvalidChannel protocol.ErrorCode = 0x89
)

// ─── Command builders ────────────────────────────────────────────────

// PlayOptions configures an AUDIO_PLAY command.
type PlayOptions struct {
	Channel   byte
	Volume    byte // 0..100
	Output    byte // OutputCh1 / OutputCh2 / OutputAll
	LoopMode  byte // LoopNone / LoopFinite / LoopInfinite
	LoopCount uint16
	Path      string
}

// CmdPlay builds an AUDIO_PLAY packet.
func CmdPlay(opt PlayOptions) []byte {
	if opt.Output == 0 {
		opt.Output = OutputAll
	}
	if opt.Volume == 0 {
		opt.Volume = 100
	}
	pathBytes := []byte(opt.Path)
	payload := make([]byte, 0, 7+len(pathBytes))
	payload = append(payload, opt.Channel, opt.Volume, opt.Output, opt.LoopMode)
	payload = append(payload, protocol.U16LE(opt.LoopCount)...)
	payload = append(payload, byte(len(pathBytes)))
	payload = append(payload, pathBytes...)
	return protocol.BuildPacket(AudioPlay, payload, 0)
}

// CmdStop stops the specified channel (`ChAll` = stop everything).
func CmdStop(channel byte) []byte {
	return protocol.BuildPacket(AudioStop, []byte{channel}, 0)
}

// CmdVolume sets per-channel volume.  Use `ChAll` for master volume.
func CmdVolume(channel, vol byte) []byte {
	return protocol.BuildPacket(AudioVolume, []byte{channel, vol}, 0)
}

// CmdFade fade-stops the specified channel.
func CmdFade(channel byte) []byte {
	return protocol.BuildPacket(AudioFade, []byte{channel}, 0)
}

// QueueOptions configures an AUDIO_QUEUE command.
type QueueOptions struct {
	Channel   byte
	Volume    byte
	LoopCount uint16
	Behavior  byte // QueueFinishLoop / QueueStopNow
	Path      string
}

// CmdQueue builds an AUDIO_QUEUE packet.
func CmdQueue(opt QueueOptions) []byte {
	if opt.Volume == 0 {
		opt.Volume = 100
	}
	pathBytes := []byte(opt.Path)
	payload := make([]byte, 0, 6+len(pathBytes))
	payload = append(payload, opt.Channel, opt.Volume)
	payload = append(payload, protocol.U16LE(opt.LoopCount)...)
	payload = append(payload, opt.Behavior)
	payload = append(payload, byte(len(pathBytes)))
	payload = append(payload, pathBytes...)
	return protocol.BuildPacket(AudioQueue, payload, 0)
}

// CmdQueueClear clears the queue on the specified channel.
func CmdQueueClear(channel byte) []byte {
	return protocol.BuildPacket(AudioQueueClear, []byte{channel}, 0)
}

// CmdStatusReq requests AUDIO_STATUS_RESP.
func CmdStatusReq() []byte { return protocol.BuildPacket(AudioStatusReq, nil, 0) }

// CmdCodecStatusReq requests CODEC_STATUS_RESP.
func CmdCodecStatusReq() []byte { return protocol.BuildPacket(CodecStatusReq, nil, 0) }

// ─── Decoded status types ────────────────────────────────────────────

// MixerState captures the high-level mixer summary that fits in the
// AUDIO_STATUS_RESP "header" (one fixed-size block followed by per-channel
// rows of variable length).  The exact shape is best-effort — newer
// firmware revisions append fields, so we surface what we can and stash
// the raw payload for callers that need bit-level access.
type MixerState struct {
	Raw []byte `json:"-"`
}

// DecodeAudioStatus is currently a stub passthrough — keep the raw
// payload so consumers can decode it against the live firmware schema.
// We don't lock the shape in this package because audio_protocol.h
// doesn't yet declare a canonical AUDIO_STATUS_RESP byte layout
// (firmware uses a delegate-rendered blob).
func DecodeAudioStatus(p []byte) MixerState {
	return MixerState{Raw: append([]byte(nil), p...)}
}

// CodecState — same caveat as MixerState.  CODEC_STATUS_RESP is a
// vendor-specific blob (TAS5825P registers); callers decode as needed.
type CodecState struct {
	Raw []byte `json:"-"`
}

func DecodeCodecStatus(p []byte) CodecState {
	return CodecState{Raw: append([]byte(nil), p...)}
}

// Compile-time reference so vet doesn't drop binary import on minimal builds.
var _ = binary.LittleEndian

// ─── Validation helpers ──────────────────────────────────────────────

// ValidatePath returns nil iff `p` is a non-empty filename ≤ 255 bytes.
func ValidatePath(p string) error {
	switch {
	case p == "":
		return fmt.Errorf("audio path must not be empty")
	case len(p) > 255:
		return fmt.Errorf("audio path too long (%d > 255)", len(p))
	}
	return nil
}

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		AudioPlay:       "AUDIO_PLAY",
		AudioStop:       "AUDIO_STOP",
		AudioVolume:     "AUDIO_VOLUME",
		AudioFade:       "AUDIO_FADE",
		AudioQueue:      "AUDIO_QUEUE",
		AudioQueueClear: "AUDIO_QUEUE_CLEAR",
		AudioStatusReq:  "AUDIO_STATUS_REQ",
		AudioStatusResp: "AUDIO_STATUS_RESP",
		CodecStatusReq:  "CODEC_STATUS_REQ",
		CodecStatusResp: "CODEC_STATUS_RESP",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrAudioFailure:   "AUDIO_FAILURE",
		ErrInvalidChannel: "AUDIO_INVALID_CHANNEL",
	})
}
