package handler_test

// Unit tests for engine/handlers/lightfx.
// Covers DecodeStatusBroadcast (LED brightness array, seqPlaying flags,
// servos, landing slots, master brightness, enabled flags, battery block)
// and DecodeLandingLightStatus (async event).

import (
	"encoding/binary"
	"scalefx/engine/handlers/lightfx"
	"testing"
)

func buildLightStatus(size int) []byte {
	buf := make([]byte, size)
	for i := 0; i < 8; i++ {
		buf[i] = uint8(10 + i*10) // 10..80 %
	}
	buf[8] = 0b01010101 // seqPlaying flags: 0,2,4,6
	binary.LittleEndian.PutUint16(buf[9:], 1500)
	binary.LittleEndian.PutUint16(buf[11:], 1600)
	binary.LittleEndian.PutUint16(buf[13:], 1700)
	return buf
}

func TestLightFX_DecodeStatusBroadcast_ShortReturnsNil(t *testing.T) {
	if lightfx.DecodeStatusBroadcast(make([]byte, 14)) != nil {
		t.Fatalf("14B payload must decode to nil")
	}
	if lightfx.DecodeStatusBroadcast(nil) != nil {
		t.Fatalf("nil payload must decode to nil")
	}
}

func TestLightFX_DecodeStatusBroadcast_Minimal(t *testing.T) {
	buf := buildLightStatus(15)
	s := lightfx.DecodeStatusBroadcast(buf)
	if s == nil {
		t.Fatalf("15B payload should decode")
	}
	for i := 0; i < 8; i++ {
		want := uint8(10 + i*10)
		if s.LedBrightness[i] != want {
			t.Errorf("LedBrightness[%d] = %d, want %d", i, s.LedBrightness[i], want)
		}
	}
	// Even indices playing, odd indices idle.
	for i := 0; i < 8; i++ {
		want := i%2 == 0
		if s.SeqPlaying[i] != want {
			t.Errorf("SeqPlaying[%d] = %v, want %v", i, s.SeqPlaying[i], want)
		}
	}
	if s.Servo0_us != 1500 || s.Servo1_us != 1600 || s.Servo2_us != 1700 {
		t.Errorf("servo mis-decoded: %+v", s)
	}
	// Extension defaults — master brightness 100, all channels enabled.
	if s.MasterBrightness != 100 {
		t.Errorf("MasterBrightness default = %d, want 100", s.MasterBrightness)
	}
	for i := 0; i < 8; i++ {
		if !s.Enabled[i] {
			t.Errorf("Enabled[%d] default should be true", i)
		}
	}
	if s.BatteryPresent {
		t.Errorf("battery should be absent for 15B payload")
	}
}

func TestLightFX_DecodeStatusBroadcast_LandingSlots(t *testing.T) {
	buf := buildLightStatus(18)
	buf[15] = 2 // slot 0 deployed → "DEP"
	buf[16] = 1 // slot 1 deploying → "DEPLOYING"
	buf[17] = 0 // slot 2 retracted → "RET"
	s := lightfx.DecodeStatusBroadcast(buf)
	if s.LandingSlots[0].Phase != 2 || s.LandingSlots[0].PhaseName != "DEP" {
		t.Errorf("slot 0 = %+v", s.LandingSlots[0])
	}
	if s.LandingSlots[1].Phase != 1 || s.LandingSlots[1].PhaseName != "DEPLOYING" {
		t.Errorf("slot 1 = %+v", s.LandingSlots[1])
	}
	if s.LandingSlots[2].Phase != 0 || s.LandingSlots[2].PhaseName != "RET" {
		t.Errorf("slot 2 = %+v", s.LandingSlots[2])
	}
}

func TestLightFX_DecodeStatusBroadcast_MasterBrightness(t *testing.T) {
	buf := buildLightStatus(19)
	buf[15], buf[16], buf[17] = 0, 0, 0
	buf[18] = 42
	s := lightfx.DecodeStatusBroadcast(buf)
	if s.MasterBrightness != 42 {
		t.Errorf("MasterBrightness = %d, want 42", s.MasterBrightness)
	}
}

func TestLightFX_DecodeStatusBroadcast_EnabledMask(t *testing.T) {
	buf := buildLightStatus(20)
	buf[18] = 100
	buf[19] = 0b00001111 // only first 4 channels enabled
	s := lightfx.DecodeStatusBroadcast(buf)
	for i := 0; i < 8; i++ {
		want := i < 4
		if s.Enabled[i] != want {
			t.Errorf("Enabled[%d] = %v, want %v", i, s.Enabled[i], want)
		}
	}
}

func TestLightFX_DecodeStatusBroadcast_Battery(t *testing.T) {
	buf := buildLightStatus(24)
	buf[18] = 100
	buf[19] = 0xFF
	binary.LittleEndian.PutUint16(buf[20:], 11100) // 11.1 V
	buf[22] = 3                                    // 3S
	buf[23] = 65                                   // 65 %
	s := lightfx.DecodeStatusBroadcast(buf)
	if s.Battery_mV != 11100 || s.CellCount != 3 || s.BatteryPct != 65 || !s.BatteryPresent {
		t.Errorf("battery mis-decoded: %+v", s)
	}
}

func TestLightFX_DecodeLandingLightStatus_Short(t *testing.T) {
	if lightfx.DecodeLandingLightStatus([]byte{0, 1}) != nil {
		t.Fatalf("2B payload must decode to nil")
	}
}

func TestLightFX_DecodeLandingLightStatus_Deployed(t *testing.T) {
	s := lightfx.DecodeLandingLightStatus([]byte{1, 2, 1})
	if s == nil {
		t.Fatalf("expected decoded struct")
	}
	if s.Slot != 1 || s.Phase != 2 || s.PhaseName != "deployed" || !s.Finished {
		t.Errorf("landing status mis-decoded: %+v", s)
	}
}

func TestLightFX_DecodeLandingLightStatus_NotFinished(t *testing.T) {
	s := lightfx.DecodeLandingLightStatus([]byte{0, 1, 0})
	if s.Finished {
		t.Errorf("Finished should be false for payload byte 0")
	}
	if s.PhaseName != "deploying" {
		t.Errorf("PhaseName = %q, want %q", s.PhaseName, "deploying")
	}
}
