package handler_test

// Unit tests for engine/handlers/hubfx.DecodeStatusBroadcast.
// Covers the 2-byte minimum, the 6-byte extension with Loop1Count, and the
// full 19-byte extended block (PCAL6416A, 6×INA226 voltages, TAS5825M).

import (
	"encoding/binary"
	"scalefx/engine/handlers/hubfx"
	"testing"
)

func TestHubFX_DecodeStatusBroadcast_ShortReturnsNil(t *testing.T) {
	if hubfx.DecodeStatusBroadcast(nil) != nil {
		t.Fatalf("nil payload must decode to nil")
	}
	if hubfx.DecodeStatusBroadcast([]byte{0x1F}) != nil {
		t.Fatalf("1B payload must decode to nil")
	}
}

func TestHubFX_DecodeStatusBroadcast_Minimal(t *testing.T) {
	// flags: core1+audio+flash+usb+sd all ready; slaveMask: all three slaves.
	s := hubfx.DecodeStatusBroadcast([]byte{0x1F, 0x07})
	if s == nil {
		t.Fatalf("2B payload should decode")
	}
	if !s.Core1Ready || !s.AudioInit || !s.FlashReady || !s.UsbReady || !s.SdReady {
		t.Errorf("flag bits mis-decoded: %+v", s)
	}
	if s.SlaveMask != 0x07 || !s.GunFxReady || !s.LightFxReady || !s.GearCtrlReady {
		t.Errorf("slave mask mis-decoded: %+v", s)
	}
	if s.Loop1Count != 0 {
		t.Errorf("Loop1Count should default to 0 for 2B payload, got %d", s.Loop1Count)
	}
	if s.I2C.Present {
		t.Errorf("I2C block should be absent for 2B payload")
	}
}

func TestHubFX_DecodeStatusBroadcast_PartialFlags(t *testing.T) {
	// Only core1Ready + gunfx slave.
	s := hubfx.DecodeStatusBroadcast([]byte{0x01, 0x01})
	if !s.Core1Ready || s.AudioInit || s.FlashReady || s.UsbReady || s.SdReady {
		t.Errorf("core-only flags mis-decoded: %+v", s)
	}
	if !s.GunFxReady || s.LightFxReady || s.GearCtrlReady {
		t.Errorf("gunfx-only slave mask mis-decoded: %+v", s)
	}
}

func TestHubFX_DecodeStatusBroadcast_Loop1Count(t *testing.T) {
	buf := make([]byte, 6)
	buf[0] = 0x01
	buf[1] = 0x00
	binary.LittleEndian.PutUint32(buf[2:], 0xDEADBEEF)
	s := hubfx.DecodeStatusBroadcast(buf)
	if s.Loop1Count != 0xDEADBEEF {
		t.Errorf("Loop1Count = %#x, want 0xDEADBEEF", s.Loop1Count)
	}
	// Sub-19B still means I2C block absent.
	if s.I2C.Present {
		t.Errorf("I2C block should be absent for 6B payload")
	}
}

func TestHubFX_DecodeStatusBroadcast_I2CIncompleteIgnored(t *testing.T) {
	// 7 bytes ≥ 6 but < 19 — I2C block not activated.
	buf := make([]byte, 18)
	buf[0], buf[1] = 0x01, 0x00
	binary.LittleEndian.PutUint32(buf[2:], 1)
	buf[6] = 0xFF // would-be mask, but too short to parse INA voltages
	s := hubfx.DecodeStatusBroadcast(buf)
	if s.I2C.Present {
		t.Errorf("I2C block must not activate below 19 bytes; got %+v", s.I2C)
	}
	if s.I2C.Mask != 0 || s.I2C.DetectedCount != 0 {
		t.Errorf("I2C fields must stay zero for partial payload: %+v", s.I2C)
	}
}

func TestHubFX_DecodeStatusBroadcast_I2CFull(t *testing.T) {
	buf := make([]byte, 19)
	buf[0], buf[1] = 0x1F, 0x07
	binary.LittleEndian.PutUint32(buf[2:], 12345)
	// mask: PCAL (bit 0) + INA226 index 0/2/5 (bits 1,3,6) + TAS (bit 7) = 0x8B | 0x48 = 0xCB
	buf[6] = 0b11001011
	binary.LittleEndian.PutUint16(buf[7:], 7200)   // INA226[0]
	binary.LittleEndian.PutUint16(buf[9:], 0)      // INA226[1] absent
	binary.LittleEndian.PutUint16(buf[11:], 11100) // INA226[2]
	binary.LittleEndian.PutUint16(buf[13:], 0)     // INA226[3] absent
	binary.LittleEndian.PutUint16(buf[15:], 0)     // INA226[4] absent
	binary.LittleEndian.PutUint16(buf[17:], 14800) // INA226[5]
	s := hubfx.DecodeStatusBroadcast(buf)
	if !s.I2C.Present {
		t.Fatalf("I2C block should be present for 19B payload")
	}
	if s.I2C.Mask != 0b11001011 {
		t.Errorf("I2C.Mask = %08b, want 11001011", s.I2C.Mask)
	}
	if !s.I2C.PCALPresent {
		t.Errorf("PCAL expected present (bit 0)")
	}
	if !s.I2C.TASPresent {
		t.Errorf("TAS5825M expected present (bit 7)")
	}
	// Present bits: 0,1,3,6,7 → 5 devices.
	if s.I2C.DetectedCount != 5 {
		t.Errorf("DetectedCount = %d, want 5", s.I2C.DetectedCount)
	}
	wantPresent := [6]bool{true, false, true, false, false, true}
	wantV := [6]uint16{7200, 0, 11100, 0, 0, 14800}
	for i := 0; i < 6; i++ {
		if s.I2C.INA226[i].Present != wantPresent[i] {
			t.Errorf("INA226[%d].Present = %v, want %v", i, s.I2C.INA226[i].Present, wantPresent[i])
		}
		if s.I2C.INA226[i].Voltage_mV != wantV[i] {
			t.Errorf("INA226[%d].Voltage_mV = %d, want %d", i, s.I2C.INA226[i].Voltage_mV, wantV[i])
		}
	}
}
