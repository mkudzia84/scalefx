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
	AudioStatusReq         protocol.PacketType = 0xE0
	AudioStatusResp        protocol.PacketType = 0xE1
	// 0xE2..0xE5 are claimed by GunFX manual override + verbose status —
	// preload diag sits at 0xE6/0xE7 (next free past the gunfx block).
	AudioPreloadStatusReq  protocol.PacketType = 0xE6
	AudioPreloadStatusResp protocol.PacketType = 0xE7
	CodecStatusReq         protocol.PacketType = 0xAA
	CodecStatusResp        protocol.PacketType = 0xAB
)

// PreloadStatus mirrors AudioPreload::STATUS_* on the firmware side.
type PreloadStatus byte

const (
	PreloadNotPresent PreloadStatus = 0
	PreloadLoading    PreloadStatus = 1
	PreloadReady      PreloadStatus = 2
	PreloadFailed     PreloadStatus = 3
)

// String renders the human-readable label used in the CLI.
func (s PreloadStatus) String() string {
	switch s {
	case PreloadNotPresent:
		return "absent"
	case PreloadLoading:
		return "loading"
	case PreloadReady:
		return "ready"
	case PreloadFailed:
		return "failed"
	}
	return "?"
}

// AssetFormat mirrors AssetFormat on the firmware side.
type AssetFormat byte

const (
	AssetUnknown AssetFormat = 0
	AssetWav     AssetFormat = 1
	AssetMp3     AssetFormat = 2
)

// String returns "wav" / "mp3" / "?".
func (f AssetFormat) String() string {
	switch f {
	case AssetWav:
		return "wav"
	case AssetMp3:
		return "mp3"
	}
	return "?"
}

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

// CmdPreloadStatusReq requests AUDIO_PRELOAD_STATUS_RESP.
func CmdPreloadStatusReq() []byte { return protocol.BuildPacket(AudioPreloadStatusReq, nil, 0) }

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
//	[supplyMode:u8][muted:u8][digitalVol:u8][deviceCtrl:u8][faultStatus:u8]
//	[codecNameLen:u8][codecName:str]
//	+ power-telemetry tail (append-only, firmware 2.42.0):
//	[pvdd_mV:u16LE][againReg:u8][dieId:u8][outPeak:u16LE]
//
// SupplyMode always reads 4 ("auto") since 2.42.0 — the manual
// codec_supply config was retired; the codec measures its own PVDD rail
// (reported in PvddMv) and auto-picks the analog gain (AgainReg, in
// −0.5 dB steps below the 29.5 Vpeak full-scale reference).
type CodecState struct {
	CodecType   byte   `json:"codecType"`
	Initialized bool   `json:"initialized"`
	I2COk       bool   `json:"i2cOk"`
	SdaPin      byte   `json:"sdaPin"`
	SclPin      byte   `json:"sclPin"`
	SupplyMode  byte   `json:"supplyMode"`
	Muted       bool   `json:"muted"`
	DigitalVol  byte   `json:"digitalVol"`
	DeviceCtrl  byte   `json:"deviceCtrl"`
	FaultStatus byte   `json:"faultStatus"`
	CodecName   string `json:"codecName"`
	HasPower    bool   `json:"hasPower"` // power-telemetry tail present
	PvddMv      uint16 `json:"pvddMv"`   // measured amp rail
	AgainReg    byte   `json:"againReg"` // auto-chosen analog gain step
	DieId       byte   `json:"dieId"`    // 0x95 = TAS5825M silicon
	OutPeak     uint16 `json:"outPeak"`  // mixed-output peak since last query (0..32767)
	Raw         []byte `json:"-"`
}

// SupplyModeAuto is the only supply-mode code emitted since fw 2.42.0.
const SupplyModeAuto byte = 4

// DecodeCodecStatus parses CODEC_STATUS_RESP (fixed head + name +
// optional power-telemetry tail; the decode is length-guarded so every
// firmware generation parses).
func DecodeCodecStatus(p []byte) CodecState {
	s := CodecState{Raw: append([]byte(nil), p...)}
	if len(p) < 10 {
		return s
	}
	s.CodecType = p[0]
	s.Initialized = p[1] != 0
	s.I2COk = p[2] != 0
	s.SdaPin = p[3]
	s.SclPin = p[4]
	s.SupplyMode = p[5]
	s.Muted = p[6] != 0
	s.DigitalVol = p[7]
	s.DeviceCtrl = p[8]
	s.FaultStatus = p[9]
	if len(p) < 11 {
		return s
	}
	nameLen := int(p[10])
	nameEnd := 11 + nameLen
	if nameEnd > len(p) {
		return s
	}
	s.CodecName = string(p[11:nameEnd])
	if len(p) >= nameEnd+6 {
		s.HasPower = true
		s.PvddMv = binary.LittleEndian.Uint16(p[nameEnd:])
		s.AgainReg = p[nameEnd+2]
		s.DieId = p[nameEnd+3]
		s.OutPeak = binary.LittleEndian.Uint16(p[nameEnd+4:])
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

// ─── Preload status ──────────────────────────────────────────────────

// PreloadEntry is one record in AUDIO_PRELOAD_STATUS_RESP.
type PreloadEntry struct {
	Path        string        `json:"path"`
	TotalBytes  uint32        `json:"totalBytes"`
	LoadedBytes uint32        `json:"loadedBytes"`
	Status      PreloadStatus `json:"status"`       // 0..3
	StatusName  string        `json:"statusName"`   // "ready" / "loading" / …
	Format      AssetFormat   `json:"format"`       // 1=wav 2=mp3
	FormatName  string        `json:"formatName"`   // "wav" / "mp3"
	Owners      []string      `json:"owners"`       // group names ("alerts", …)
}

// PercentLoaded returns LoadedBytes×100/TotalBytes, clamped to [0,100].
func (e PreloadEntry) PercentLoaded() int {
	if e.TotalBytes == 0 {
		return 0
	}
	p := int(uint64(e.LoadedBytes) * 100 / uint64(e.TotalBytes))
	if p > 100 {
		return 100
	}
	return p
}

// PreloadState is the decoded AUDIO_PRELOAD_STATUS_RESP.
type PreloadState struct {
	ResidentBytes uint32         `json:"residentBytes"`
	BudgetBytes   uint32         `json:"budgetBytes"`
	Ready         uint16         `json:"ready"`
	Loading       uint16         `json:"loading"`
	Failed        uint16         `json:"failed"`
	Pinned        uint16         `json:"pinned"`
	Entries       []PreloadEntry `json:"entries"`
	Raw           []byte         `json:"-"`
}

// DecodePreloadStatus parses the variable-length payload.  Header is
// 16 bytes (4 u32 / u16 fields) then a list of per-path records.
// Stops cleanly at any underflow — the firmware truncates if the COBS
// frame would overflow, and the header counters always reflect the
// TOTAL state (so the client can know whether records were truncated).
func DecodePreloadStatus(p []byte) PreloadState {
	s := PreloadState{Raw: append([]byte(nil), p...)}
	if len(p) < 16 {
		return s
	}
	s.ResidentBytes = binary.LittleEndian.Uint32(p[0:4])
	s.BudgetBytes   = binary.LittleEndian.Uint32(p[4:8])
	s.Ready         = binary.LittleEndian.Uint16(p[8:10])
	s.Loading       = binary.LittleEndian.Uint16(p[10:12])
	s.Failed        = binary.LittleEndian.Uint16(p[12:14])
	s.Pinned        = binary.LittleEndian.Uint16(p[14:16])

	pos := 16
	for pos < len(p) {
		// [pathLen:u8][path:N][totalBytes:u32][loadedBytes:u32]
		// [status:u8][format:u8][ownerCount:u8][owners…]
		if pos+1 > len(p) {
			break
		}
		pathLen := int(p[pos])
		pos++
		if pos+pathLen+4+4+1+1+1 > len(p) {
			break
		}
		entry := PreloadEntry{
			Path:        string(p[pos : pos+pathLen]),
		}
		pos += pathLen
		entry.TotalBytes = binary.LittleEndian.Uint32(p[pos:])
		pos += 4
		entry.LoadedBytes = binary.LittleEndian.Uint32(p[pos:])
		pos += 4
		entry.Status = PreloadStatus(p[pos])
		entry.StatusName = entry.Status.String()
		pos++
		entry.Format = AssetFormat(p[pos])
		entry.FormatName = entry.Format.String()
		pos++
		ownerCount := int(p[pos])
		pos++
		for i := 0; i < ownerCount; i++ {
			if pos+1 > len(p) {
				break
			}
			n := int(p[pos])
			pos++
			if pos+n > len(p) {
				break
			}
			entry.Owners = append(entry.Owners, string(p[pos:pos+n]))
			pos += n
		}
		s.Entries = append(s.Entries, entry)
	}
	return s
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
		AudioStatusReq:         "AUDIO_STATUS_REQ",
		AudioStatusResp:        "AUDIO_STATUS_RESP",
		AudioPreloadStatusReq:  "AUDIO_PRELOAD_STATUS_REQ",
		AudioPreloadStatusResp: "AUDIO_PRELOAD_STATUS_RESP",
		CodecStatusReq:         "CODEC_STATUS_REQ",
		CodecStatusResp:        "CODEC_STATUS_RESP",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrAudioFailure:   "AUDIO_FAILURE",
		ErrInvalidChannel: "AUDIO_INVALID_CHANNEL",
	})
}
