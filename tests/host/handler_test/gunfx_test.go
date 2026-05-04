package handler_test

// Unit tests for engine/handlers/gunfx.DecodeStatusBroadcast.
// Covers the mandatory 20-byte header and every optional extension:
// smoke error reasons (22+), overcurrent duty (24+), battery block (28+).

import (
	"encoding/binary"
	"scalefx/engine/handlers/gunfx"
	"testing"
)

// buildGunStatus composes a GunFX STATUS_BROADCAST payload. Extension blocks
// are appended only when the caller provides non-zero sentinel values via the
// ext* arguments — callers drive the size explicitly with payloadSize.
func buildGunStatus(payloadSize int, flags uint8) []byte {
	buf := make([]byte, payloadSize)
	buf[0] = flags
	buf[1] = 0x55 // fanSpeed
	binary.LittleEndian.PutUint16(buf[2:], 1234)   // fanOff_ms
	binary.LittleEndian.PutUint16(buf[4:], 1500)   // servo0
	binary.LittleEndian.PutUint16(buf[6:], 1600)   // servo1
	binary.LittleEndian.PutUint16(buf[8:], 1700)   // servo2
	binary.LittleEndian.PutUint16(buf[10:], 9000)  // rpm
	binary.LittleEndian.PutUint32(buf[12:], 42)    // shots
	binary.LittleEndian.PutUint32(buf[16:], 15000) // heater_ms
	return buf
}

func TestGunFX_DecodeStatusBroadcast_ShortReturnsNil(t *testing.T) {
	if gunfx.DecodeStatusBroadcast(make([]byte, 19)) != nil {
		t.Fatalf("DecodeStatusBroadcast(19B) expected nil")
	}
	if gunfx.DecodeStatusBroadcast(nil) != nil {
		t.Fatalf("DecodeStatusBroadcast(nil) expected nil")
	}
}

func TestGunFX_DecodeStatusBroadcast_MinimalFlags(t *testing.T) {
	// All flag bits set so the bit-mask parsing is exercised.
	buf := buildGunStatus(20, 0x3F)
	s := gunfx.DecodeStatusBroadcast(buf)
	if s == nil {
		t.Fatalf("expected decoded struct, got nil")
	}
	if !s.Firing || !s.FlashActive || !s.FlashFading || !s.HeaterOn || !s.FanOn || !s.FanSpindown {
		t.Errorf("flag bits not all true: %+v", s)
	}
	if s.FanSpeed != 0x55 {
		t.Errorf("FanSpeed = %d, want 0x55", s.FanSpeed)
	}
	if s.FanOff_ms != 1234 || s.Servo0_us != 1500 || s.Servo1_us != 1600 || s.Servo2_us != 1700 {
		t.Errorf("16-bit fields mis-decoded: %+v", s)
	}
	if s.RPM != 9000 || s.Shots != 42 || s.Heater_ms != 15000 {
		t.Errorf("rpm/shots/heater mis-decoded: %+v", s)
	}
	// Optional fields absent → duty defaults to 255, battery zero-valued.
	if s.HeaterDuty != 255 || s.FanDuty != 255 {
		t.Errorf("duty defaults = %d/%d, want 255/255", s.HeaterDuty, s.FanDuty)
	}
	if s.BatteryPresent || s.Battery_mV != 0 {
		t.Errorf("battery should be absent: %+v", s)
	}
}

func TestGunFX_DecodeStatusBroadcast_SmokeErrors(t *testing.T) {
	buf := buildGunStatus(22, 0)
	buf[20] = 0x01 // heater disconnected
	buf[21] = 0x04 // fan overcurrent
	s := gunfx.DecodeStatusBroadcast(buf)
	if s.HeaterError != 0x01 || s.HeaterErrorName != "heater disconnected" {
		t.Errorf("heater error mis-decoded: code=%d name=%q", s.HeaterError, s.HeaterErrorName)
	}
	if s.FanError != 0x04 || s.FanErrorName != "fan overcurrent" {
		t.Errorf("fan error mis-decoded: code=%d name=%q", s.FanError, s.FanErrorName)
	}
}

func TestGunFX_DecodeStatusBroadcast_SmokeErrorZeroLeavesNameBlank(t *testing.T) {
	buf := buildGunStatus(22, 0)
	buf[20] = 0
	buf[21] = 0
	s := gunfx.DecodeStatusBroadcast(buf)
	if s.HeaterErrorName != "" || s.FanErrorName != "" {
		t.Errorf("zero-code should yield empty name, got heater=%q fan=%q",
			s.HeaterErrorName, s.FanErrorName)
	}
}

func TestGunFX_DecodeStatusBroadcast_OvercurrentDuty(t *testing.T) {
	buf := buildGunStatus(24, 0)
	buf[20] = 0
	buf[21] = 0
	buf[22] = 80 // heater throttled to 80%
	buf[23] = 60 // fan throttled to 60%
	s := gunfx.DecodeStatusBroadcast(buf)
	if s.HeaterDuty != 80 || s.FanDuty != 60 {
		t.Errorf("duty mis-decoded: heater=%d fan=%d", s.HeaterDuty, s.FanDuty)
	}
}

func TestGunFX_DecodeStatusBroadcast_BatteryBlock(t *testing.T) {
	buf := buildGunStatus(28, 0)
	buf[20], buf[21] = 0, 0
	buf[22], buf[23] = 255, 255
	binary.LittleEndian.PutUint16(buf[24:], 7400) // 7.4 V
	buf[26] = 2                                   // 2S
	buf[27] = 78                                  // 78 %
	s := gunfx.DecodeStatusBroadcast(buf)
	if s.Battery_mV != 7400 || s.CellCount != 2 || s.BatteryPct != 78 || !s.BatteryPresent {
		t.Errorf("battery mis-decoded: %+v", s)
	}
}

func TestGunFX_DecodeStatusBroadcast_BatteryAbsentWhenZero(t *testing.T) {
	buf := buildGunStatus(28, 0)
	buf[22], buf[23] = 255, 255
	// Battery bytes all zero → BatteryPresent must be false.
	s := gunfx.DecodeStatusBroadcast(buf)
	if s.BatteryPresent {
		t.Errorf("BatteryPresent should be false when Battery_mV=0")
	}
}
