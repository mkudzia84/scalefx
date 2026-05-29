package audio_protocol_test

// Unit tests for the audio protocol (controllers/lib/sfx_audio/server/
// audio_service.h + audio_protocol.h ↔ app/go/protocol/audio/audio.go).
//
// Locks in:
//   - Audio packet-type byte assignments (0xDA..0xE1 + codec 0xAA/0xAB
//     + preload diag 0xE6/0xE7)
//   - Output-mask enum (L=0x01, R=0x02, stereo=0x03 — matches
//     speaker_routing.ts in Studio per Rule 47)
//   - Loop-mode enum (None=0, Finite=1, Infinite=2)
//   - Queue stop-mode enum (FinishLoop=0, StopNow=1)
//   - CmdPlay payload [ch][vol][output][loopMode][loopCount:u16LE]
//                     [pathLen:u8][path]
//   - CmdStop / CmdVolume / CmdFade single-byte payloads
//   - ChAll convention (0xFF = all channels)
//
// Regression target: the byte allocation table in CLAUDE.md
// ("Audio control 0xDA–0xE1 · Audio preload diag 0xE6–0xE7").  If any
// packet byte drifts the firmware dispatcher silently fails to claim
// or routes to the wrong handler.

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/audio"
)

// ─── Packet-type byte assignments ──────────────────────────────────────

func TestAudioPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"AUDIO_PLAY", audio.AudioPlay, 0xDA},
		{"AUDIO_STOP", audio.AudioStop, 0xDB},
		{"AUDIO_VOLUME", audio.AudioVolume, 0xDC},
		{"AUDIO_FADE", audio.AudioFade, 0xDD},
		{"AUDIO_QUEUE", audio.AudioQueue, 0xDE},
		{"AUDIO_QUEUE_CLEAR", audio.AudioQueueClear, 0xDF},
		{"AUDIO_STATUS_REQ", audio.AudioStatusReq, 0xE0},
		{"AUDIO_PRELOAD_STATUS_REQ", audio.AudioPreloadStatusReq, 0xE6},
		{"AUDIO_PRELOAD_STATUS_RESP", audio.AudioPreloadStatusResp, 0xE7},
		{"CODEC_STATUS_REQ", audio.CodecStatusReq, 0xAA},
		{"CODEC_STATUS_RESP", audio.CodecStatusResp, 0xAB},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X — CLAUDE.md byte allocation map drift",
				tc.name, byte(tc.got), tc.want)
		}
	}
}

// ─── Audio packet types don't collide with neighbouring blocks ─────────

func TestAudioPacketTypesInAllocationRange(t *testing.T) {
	// Per CLAUDE.md: AUDIO control = 0xDA..0xE1, preload diag = 0xE6..0xE7.
	// Verify each AUDIO_* type falls in those windows so a future
	// rename doesn't silently squat in (say) the GunFx 0xE2..0xE5 range
	// where AUDIO_PRELOAD_STATUS_* originally collided (2026-05-28).
	for _, p := range []struct {
		name string
		got  protocol.PacketType
		min  byte
		max  byte
	}{
		{"AUDIO_PLAY", audio.AudioPlay, 0xDA, 0xE1},
		{"AUDIO_STOP", audio.AudioStop, 0xDA, 0xE1},
		{"AUDIO_VOLUME", audio.AudioVolume, 0xDA, 0xE1},
		{"AUDIO_FADE", audio.AudioFade, 0xDA, 0xE1},
		{"AUDIO_QUEUE", audio.AudioQueue, 0xDA, 0xE1},
		{"AUDIO_QUEUE_CLEAR", audio.AudioQueueClear, 0xDA, 0xE1},
		{"AUDIO_STATUS_REQ", audio.AudioStatusReq, 0xDA, 0xE1},
		{"AUDIO_PRELOAD_STATUS_REQ", audio.AudioPreloadStatusReq, 0xE6, 0xE7},
		{"AUDIO_PRELOAD_STATUS_RESP", audio.AudioPreloadStatusResp, 0xE6, 0xE7},
	} {
		b := byte(p.got)
		if b < p.min || b > p.max {
			t.Errorf("%s = 0x%02X outside allocation range [0x%02X..0x%02X]",
				p.name, b, p.min, p.max)
		}
	}
}

// ─── Output-mask enum (matches Studio's speaker_routing.ts) ────────────

func TestOutputMaskValues(t *testing.T) {
	// Rule 47: MASK_LEFT=0x01, MASK_RIGHT=0x02, MASK_STEREO=0x03.
	if audio.OutputCh1 != 0x01 {
		t.Errorf("OutputCh1 = 0x%02X, want 0x01", audio.OutputCh1)
	}
	if audio.OutputCh2 != 0x02 {
		t.Errorf("OutputCh2 = 0x%02X, want 0x02", audio.OutputCh2)
	}
	if audio.OutputAll != 0x03 {
		t.Errorf("OutputAll = 0x%02X, want 0x03 (stereo)", audio.OutputAll)
	}
	if audio.OutputAll != (audio.OutputCh1 | audio.OutputCh2) {
		t.Error("OutputAll must equal OutputCh1|OutputCh2")
	}
}

// ─── Loop-mode enum ───────────────────────────────────────────────────

func TestLoopModeValues(t *testing.T) {
	if audio.LoopNone != 0 || audio.LoopFinite != 1 || audio.LoopInfinite != 2 {
		t.Errorf("loop mode values: None=%d Finite=%d Infinite=%d, want 0/1/2",
			audio.LoopNone, audio.LoopFinite, audio.LoopInfinite)
	}
}

// ─── Queue stop-mode enum ──────────────────────────────────────────────

func TestQueueStopModeValues(t *testing.T) {
	if audio.QueueFinishLoop != 0 || audio.QueueStopNow != 1 {
		t.Errorf("queue stop values: FinishLoop=%d StopNow=%d, want 0/1",
			audio.QueueFinishLoop, audio.QueueStopNow)
	}
}

// ─── ChAll convention ──────────────────────────────────────────────────

func TestChAllBroadcast(t *testing.T) {
	if audio.ChAll != 0xFF {
		t.Errorf("ChAll = 0x%02X, want 0xFF (broadcast)", audio.ChAll)
	}
	if audio.MaxChannels != 8 {
		t.Errorf("MaxChannels = %d, want 8", audio.MaxChannels)
	}
}

// ─── CmdPlay wire format ───────────────────────────────────────────────

func TestCmdPlayPayload(t *testing.T) {
	opt := audio.PlayOptions{
		Channel:   3,
		Volume:    80,
		Output:    audio.OutputCh2,
		LoopMode:  audio.LoopFinite,
		LoopCount: 5,
		Path:      "/sounds/test.mp3",
	}
	raw := audio.CmdPlay(opt)
	ptype, _, payload, ok := protocol.ParsePacket(raw)
	if !ok {
		t.Fatal("ParsePacket failed")
	}
	if ptype != audio.AudioPlay {
		t.Errorf("type = 0x%02X, want 0x%02X", byte(ptype), byte(audio.AudioPlay))
	}

	// Wire: [ch][vol][output][loopMode][loopCount:u16LE][pathLen:u8][path]
	wantLen := 4 + 2 + 1 + len(opt.Path)
	if len(payload) != wantLen {
		t.Fatalf("payload length = %d, want %d", len(payload), wantLen)
	}
	if payload[0] != opt.Channel {
		t.Errorf("channel = %d, want %d", payload[0], opt.Channel)
	}
	if payload[1] != opt.Volume {
		t.Errorf("volume = %d, want %d", payload[1], opt.Volume)
	}
	if payload[2] != opt.Output {
		t.Errorf("output = 0x%02X, want 0x%02X", payload[2], opt.Output)
	}
	if payload[3] != opt.LoopMode {
		t.Errorf("loopMode = %d, want %d", payload[3], opt.LoopMode)
	}
	if got := protocol.ReadU16LE(payload, 4); got != opt.LoopCount {
		t.Errorf("loopCount = %d, want %d", got, opt.LoopCount)
	}
	if int(payload[6]) != len(opt.Path) {
		t.Errorf("pathLen = %d, want %d", payload[6], len(opt.Path))
	}
	if got := string(payload[7:]); got != opt.Path {
		t.Errorf("path = %q, want %q", got, opt.Path)
	}
}

func TestCmdPlayDefaultsVolume100AndOutputAll(t *testing.T) {
	// Volume 0 and Output 0 should be replaced by the defaults — see
	// CmdPlay's zero-value handling.  Locks the convention so a future
	// refactor can't silently change the default behaviour.
	raw := audio.CmdPlay(audio.PlayOptions{
		Channel: 0,
		Path:    "/x.mp3",
	})
	_, _, payload, _ := protocol.ParsePacket(raw)
	if payload[1] != 100 {
		t.Errorf("default volume = %d, want 100", payload[1])
	}
	if payload[2] != audio.OutputAll {
		t.Errorf("default output = 0x%02X, want 0x%02X (OutputAll)",
			payload[2], audio.OutputAll)
	}
}

// ─── CmdStop / CmdVolume / CmdFade ─────────────────────────────────────

func TestCmdStopPayload(t *testing.T) {
	raw := audio.CmdStop(2)
	ptype, _, payload, _ := protocol.ParsePacket(raw)
	if ptype != audio.AudioStop || len(payload) != 1 || payload[0] != 2 {
		t.Errorf("CmdStop(2): type=0x%02X payload=%v, want 0xDB [2]",
			byte(ptype), payload)
	}
}

func TestCmdStopAllUsesChAll(t *testing.T) {
	raw := audio.CmdStop(audio.ChAll)
	_, _, payload, _ := protocol.ParsePacket(raw)
	if payload[0] != 0xFF {
		t.Errorf("CmdStop(ChAll) payload[0] = 0x%02X, want 0xFF", payload[0])
	}
}

func TestCmdVolumePayload(t *testing.T) {
	raw := audio.CmdVolume(4, 75)
	ptype, _, payload, _ := protocol.ParsePacket(raw)
	if ptype != audio.AudioVolume || len(payload) != 2 || payload[0] != 4 || payload[1] != 75 {
		t.Errorf("CmdVolume(4, 75): type=0x%02X payload=%v, want 0xDC [4 75]",
			byte(ptype), payload)
	}
}

func TestCmdFadePayload(t *testing.T) {
	raw := audio.CmdFade(1)
	ptype, _, payload, _ := protocol.ParsePacket(raw)
	if ptype != audio.AudioFade || len(payload) != 1 || payload[0] != 1 {
		t.Errorf("CmdFade(1): type=0x%02X payload=%v, want 0xDD [1]",
			byte(ptype), payload)
	}
}

// ─── Status request packets (zero-payload queries) ─────────────────────

func TestStatusRequestPacketsHaveNoPayload(t *testing.T) {
	for _, tc := range []struct {
		name string
		raw  []byte
		want protocol.PacketType
	}{
		{"AUDIO_STATUS_REQ", audio.CmdStatusReq(), audio.AudioStatusReq},
		{"CODEC_STATUS_REQ", audio.CmdCodecStatusReq(), audio.CodecStatusReq},
		{"PRELOAD_STATUS_REQ", audio.CmdPreloadStatusReq(), audio.AudioPreloadStatusReq},
	} {
		ptype, _, payload, ok := protocol.ParsePacket(tc.raw)
		if !ok {
			t.Errorf("%s: ParsePacket failed", tc.name)
			continue
		}
		if ptype != tc.want {
			t.Errorf("%s: type = 0x%02X, want 0x%02X",
				tc.name, byte(ptype), byte(tc.want))
		}
		if len(payload) != 0 {
			t.Errorf("%s: payload length = %d, want 0", tc.name, len(payload))
		}
	}
}

// ─── Audio error codes ─────────────────────────────────────────────────

func TestAudioErrorCodes(t *testing.T) {
	// Per CLAUDE.md 2026-05-23 sweep, AudioError = 0xB0..0xBF.
	if byte(audio.ErrAudioFailure) != 0xB0 {
		t.Errorf("ErrAudioFailure = 0x%02X, want 0xB0", audio.ErrAudioFailure)
	}
	if byte(audio.ErrInvalidChannel) != 0xB1 {
		t.Errorf("ErrInvalidChannel = 0x%02X, want 0xB1", audio.ErrInvalidChannel)
	}
}
