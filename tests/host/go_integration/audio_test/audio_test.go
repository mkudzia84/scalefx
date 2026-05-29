package audio_test

// Integration tests for the audio subsystem on a real HubFX.
//
// Audio sits across the heaviest dual-core path on the device (mixer
// on Core 1, decoder + asset cache on Core 0).  These tests verify:
//   - AUDIO_STATUS_RESP decodes into the expected shape (sample rate,
//     codec name, channel descriptors)
//   - CODEC_STATUS_RESP returns a state when the TAS5825x is healthy
//   - Master volume round-trips (set then observe via Status)
//   - StopAll() doesn't error even when no channel is playing
//
// We deliberately don't play long files here — that's covered indirectly
// by the upload_test suite (which suspends the mixer during upload).
// Honours Rule 51 / shared-client TestMain.

import (
	"os"
	"testing"
	"time"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/tests/host/ports"
)

func TestMain(m *testing.M) {
	os.Exit(ports.RunWithSharedClient(m, "audio_test"))
}

// requireAudio skips the test if the connected board doesn't advertise
// the AUDIO capability bit — keeps the suite usable on future boards
// that link the same protocol but omit the mixer (e.g. an audio-less
// HubFX rev).
func requireAudio(t *testing.T) *client.Client {
	t.Helper()
	c := ports.RequireSharedClient(t)
	id := ports.SharedIdentity()
	if !core.HasCapability(id.Capabilities, core.CapAudio) {
		t.Skipf("device %s doesn't advertise AUDIO capability (0x%08X)",
			id.DeviceName, id.Capabilities)
	}
	return c
}

// ─── AUDIO_STATUS_RESP shape ──────────────────────────────────────────

func TestAudioStatusReturnsValid(t *testing.T) {
	c := requireAudio(t)
	s, err := c.Audio.Status()
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if s.SampleRateHz == 0 {
		t.Error("SampleRateHz = 0 — mixer not initialised?")
	}
	if s.BitDepth == 0 {
		t.Error("BitDepth = 0 — should be 16 for the int16 Q15 kernel")
	}
	if s.MaxChannels == 0 {
		t.Error("MaxChannels = 0 — AUDIO_MAX_CHANNELS not advertised")
	}
	// HubFX runs at 48 kHz / 16-bit on the int16 mixer kernel (Phase 5).
	if s.SampleRateHz != 48000 {
		t.Errorf("SampleRateHz = %d, want 48000 (HubFX int16 kernel)", s.SampleRateHz)
	}
	if s.BitDepth != 16 {
		t.Errorf("BitDepth = %d, want 16", s.BitDepth)
	}
	if s.MaxChannels < 6 {
		t.Errorf("MaxChannels = %d, want >= 6 (HubFX supports 8)", s.MaxChannels)
	}
	t.Logf("Audio: %dHz/%dbit codec=%q ch=%d underruns=%d",
		s.SampleRateHz, s.BitDepth, s.CodecName, s.MaxChannels, s.Underruns)
}

// ─── CODEC_STATUS ─────────────────────────────────────────────────────

func TestCodecStatusReturnsState(t *testing.T) {
	c := requireAudio(t)
	cs, err := c.Audio.CodecStatus()
	if err != nil {
		t.Fatalf("CodecStatus: %v", err)
	}
	if !cs.Initialized {
		t.Error("CodecState.Initialized = false — TAS5825x didn't reach PLAY")
	}
	if !cs.I2COk {
		t.Error("CodecState.I2COk = false — codec I2C is wedged")
	}
	// SupplyVoltage byte mirrors `audio.codec_supply` in /hubfx.yaml
	// (12v / 24v).  A value of 0 just means the YAML used the default
	// (the firmware constant in audio_codec.h applies).  Not an error.
	// What we DO care about is that the field decoded into a valid byte
	// range — the decoder shouldn't return garbage.
	if cs.SupplyVoltage > 100 {
		t.Errorf("SupplyVoltage = %d — out of valid range [0..100]", cs.SupplyVoltage)
	}
	t.Logf("Codec: type=0x%02X init=%v i2c=%v supply=%d dvol=0x%02X",
		cs.CodecType, cs.Initialized, cs.I2COk, cs.SupplyVoltage, cs.DigitalVol)
}

// ─── StopAll: should be a no-op when nothing is playing ──────────────

func TestStopAllOnIdleMixerIsHarmless(t *testing.T) {
	c := requireAudio(t)
	if err := c.Audio.StopAll(); err != nil {
		t.Fatalf("StopAll on idle mixer: %v", err)
	}
}

// ─── Master volume round-trip ─────────────────────────────────────────

// Set the master volume to a known value, then read STATUS back.  The
// firmware echoes the master volume in MasterVolPct so we can verify
// the wire path end-to-end.  Restores the original value at the end.
func TestMasterVolumeRoundTrip(t *testing.T) {
	c := requireAudio(t)

	// Capture original.
	orig, err := c.Audio.Status()
	if err != nil {
		t.Fatalf("Status (snapshot): %v", err)
	}
	t.Cleanup(func() {
		_ = c.Audio.MasterVolume(orig.MasterVolPct)
	})

	const want = byte(42)
	if err := c.Audio.MasterVolume(want); err != nil {
		t.Fatalf("MasterVolume(%d): %v", want, err)
	}

	// Give firmware a tick to apply.
	time.Sleep(50 * time.Millisecond)
	got, err := c.Audio.Status()
	if err != nil {
		t.Fatalf("Status (after set): %v", err)
	}
	if got.MasterVolPct != want {
		t.Errorf("after MasterVolume(%d): Status.MasterVolPct = %d, want %d",
			want, got.MasterVolPct, want)
	}
}
