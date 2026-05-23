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
	// Control packets relocated 0x84..0x8B → 0xDA..0xE1: the old block
	// collided with the hub's ExpanderService (0x80..0x87) + TopologyService
	// (0x88..0x8E), which dispatch earlier and ate AUDIO_STOP/QUEUE/STATUS_REQ.
	// Codec 0xAA..0xAB never collided. Mirrors audio_protocol.h.
	AudioPlay       protocol.PacketType = 0xDA
	AudioStop       protocol.PacketType = 0xDB
	AudioVolume     protocol.PacketType = 0xDC
	AudioFade       protocol.PacketType = 0xDD
	AudioQueue      protocol.PacketType = 0xDE
	AudioQueueClear protocol.PacketType = 0xDF
	AudioStatusReq  protocol.PacketType = 0xE0
	AudioStatusResp protocol.PacketType = 0xE1
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

// ─── Error codes (0xB0..0xBF per CLAUDE.md) ──────────────────────────

const (
	ErrAudioFailure   protocol.ErrorCode = 0xB0
	ErrInvalidChannel protocol.ErrorCode = 0xB1
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

// MixerFlags is the bitmask in AUDIO_STATUS_RESP.flags.
const (
	MixerFlagInitialized   byte = 1 << 0
	MixerFlagI2sRunning    byte = 1 << 1
	MixerFlagHasCodec      byte = 1 << 2
	MixerFlagHasRingStats  byte = 1 << 3
	MixerFlagHasBufferCaps byte = 1 << 4
)

// ChannelState is one entry in MixerState.Channels — populated for
// every bit set in the firmware's `activeMask`.
type ChannelState struct {
	Channel       byte   `json:"channel"`
	VolumePct     byte   `json:"volumePct"`
	Playing       bool   `json:"playing"`
	Looping       bool   `json:"looping"`
	LoopCount     uint16 `json:"loopCount"`
	RemainingMs   uint32 `json:"remainingMs"`
	QueueLen      byte   `json:"queueLen"`
	OutputMask    byte   `json:"outputMask"`
	WavRateHz     uint16 `json:"wavRateHz"`
	WavChannels   byte   `json:"wavChannels"`
	WavBits       byte   `json:"wavBits"`
	WavBufFillPct byte   `json:"wavBufFillPct"`
	Filename      string `json:"filename"`
}

// MixerState is the decoded AUDIO_STATUS_RESP — see firmware
// `audio_service.ipp::handleStatusReq()` for the v4 wire format.
type MixerState struct {
	MasterVolPct  byte           `json:"masterVolPct"`
	Flags         byte           `json:"flags"`
	SampleRateHz  uint16         `json:"sampleRateHz"`
	BitDepth      byte           `json:"bitDepth"`
	MaxChannels   byte           `json:"maxChannels"`
	CodecName     string         `json:"codecName"`
	RingFillPct   byte           `json:"ringFillPct"`
	RingAvailRead uint16         `json:"ringAvailRead"`
	RingAvailWrite uint16        `json:"ringAvailWrite"`
	Underruns     uint32         `json:"underruns"`
	ConsumeLoops  uint32         `json:"consumeLoops"`
	ConsumeFrames uint32         `json:"consumeFrames"`
	WavBufFrames  uint16         `json:"wavBufFrames"`
	RingFrames    uint16         `json:"ringFrames"`
	ActiveMask    byte           `json:"activeMask"`
	Channels      []ChannelState `json:"channels"`
	Raw           []byte         `json:"-"`
}

// Initialized reports the corresponding flag.
func (m MixerState) Initialized() bool { return m.Flags&MixerFlagInitialized != 0 }

// I2SRunning reports the corresponding flag.
func (m MixerState) I2SRunning() bool { return m.Flags&MixerFlagI2sRunning != 0 }

// DecodeAudioStatus parses the v4 wire format from
// `audio_service.ipp::handleStatusReq()`.  Trailing fields are
// optional per Rule 11 (firmware may extend); decoder stops cleanly
// at whichever byte boundary the payload runs out of.
func DecodeAudioStatus(p []byte) MixerState {
	m := MixerState{Raw: append([]byte(nil), p...)}
	if len(p) < 7 {
		return m
	}
	m.MasterVolPct = p[0]
	m.Flags = p[1]
	m.SampleRateHz = binary.LittleEndian.Uint16(p[2:4])
	m.BitDepth = p[4]
	m.MaxChannels = p[5]
	codecLen := int(p[6])
	off := 7
	if off+codecLen > len(p) {
		return m
	}
	m.CodecName = string(p[off : off+codecLen])
	off += codecLen

	if m.Flags&MixerFlagHasRingStats != 0 {
		if off+13 > len(p) {
			return m
		}
		m.RingFillPct = p[off]
		m.RingAvailRead = binary.LittleEndian.Uint16(p[off+1 : off+3])
		m.RingAvailWrite = binary.LittleEndian.Uint16(p[off+3 : off+5])
		m.Underruns = binary.LittleEndian.Uint32(p[off+5 : off+9])
		m.ConsumeLoops = binary.LittleEndian.Uint32(p[off+9 : off+13])
		// consumeFrames sometimes overlaps the next field on truncated
		// payloads — read it only if we have a full u32 to consume.
		if off+17 > len(p) {
			off += 13
		} else {
			m.ConsumeFrames = binary.LittleEndian.Uint32(p[off+13 : off+17])
			off += 17
		}
	}
	if m.Flags&MixerFlagHasBufferCaps != 0 {
		if off+4 > len(p) {
			return m
		}
		m.WavBufFrames = binary.LittleEndian.Uint16(p[off : off+2])
		m.RingFrames = binary.LittleEndian.Uint16(p[off+2 : off+4])
		off += 4
	}
	if off >= len(p) {
		return m
	}
	m.ActiveMask = p[off]
	off++

	for ch := byte(0); ch < 8; ch++ {
		if m.ActiveMask&(1<<ch) == 0 {
			continue
		}
		if off+15 > len(p) {
			return m
		}
		c := ChannelState{
			Channel:     p[off],
			VolumePct:   p[off+1],
			Playing:     p[off+2] != 0,
			Looping:     p[off+3] != 0,
			LoopCount:   binary.LittleEndian.Uint16(p[off+4 : off+6]),
			RemainingMs: binary.LittleEndian.Uint32(p[off+6 : off+10]),
			QueueLen:    p[off+10],
			OutputMask:  p[off+11],
			WavRateHz:   binary.LittleEndian.Uint16(p[off+12 : off+14]),
			WavChannels: p[off+14],
		}
		off += 15
		if off+1 > len(p) {
			m.Channels = append(m.Channels, c)
			return m
		}
		c.WavBits = p[off]
		off++
		if off+1 > len(p) {
			m.Channels = append(m.Channels, c)
			return m
		}
		c.WavBufFillPct = p[off]
		off++
		if off >= len(p) {
			m.Channels = append(m.Channels, c)
			return m
		}
		fnLen := int(p[off])
		off++
		if off+fnLen > len(p) {
			m.Channels = append(m.Channels, c)
			return m
		}
		c.Filename = string(p[off : off+fnLen])
		off += fnLen
		m.Channels = append(m.Channels, c)
	}
	return m
}

// ─── Codec state ─────────────────────────────────────────────────────

// CodecState mirrors CODEC_STATUS_RESP — vendor-specific (TAS5825P) so
// the decode is best-effort and Raw remains the authoritative copy for
// callers that want bit-level access.
//
//	[codecType:u8][initialized:u8][i2cOk:u8][sdaPin:u8][sclPin:u8]
//	[supplyVoltage:u8][muted:u8][digitalVol:u8][deviceCtrl:u8][faultStatus:u8]
type CodecState struct {
	CodecType     byte   `json:"codecType"`
	Initialized   bool   `json:"initialized"`
	I2COk         bool   `json:"i2cOk"`
	SdaPin        byte   `json:"sdaPin"`
	SclPin        byte   `json:"sclPin"`
	SupplyVoltage byte   `json:"supplyVoltage"`
	Muted         bool   `json:"muted"`
	DigitalVol    byte   `json:"digitalVol"`
	DeviceCtrl    byte   `json:"deviceCtrl"`
	FaultStatus   byte   `json:"faultStatus"`
	Raw           []byte `json:"-"`
}

// DecodeCodecStatus parses the 10-byte CODEC_STATUS_RESP.
func DecodeCodecStatus(p []byte) CodecState {
	s := CodecState{Raw: append([]byte(nil), p...)}
	if len(p) >= 10 {
		s.CodecType = p[0]
		s.Initialized = p[1] != 0
		s.I2COk = p[2] != 0
		s.SdaPin = p[3]
		s.SclPin = p[4]
		s.SupplyVoltage = p[5]
		s.Muted = p[6] != 0
		s.DigitalVol = p[7]
		s.DeviceCtrl = p[8]
		s.FaultStatus = p[9]
	}
	return s
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
