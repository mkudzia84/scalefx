package handler_test

// Unit tests for gearcontrol decoders. Wire format mirrors the C++ firmware
// in controllers/gearcontrol/pico/src/gearcontrol_pico.ino.

import (
	"scalefx/engine/handlers/gearcontrol"
	"scalefx/protocol"
	"testing"
)

// buildGearStatusFull composes a 53-byte StatusBroadcast payload covering
// all optional extensions (error reasons, shunt mOhm, door modes, config
// flags, door state).
func buildGearStatusFull() []byte {
	buf := make([]byte, 53)

	// 3 gears × 11 bytes of per-gear fixed state (bytes 0-32).
	for i := 0; i < 3; i++ {
		off := i * 11
		buf[off] = byte(1 + i) // state = DEPLOYED, RETRACTED, DEPLOYING
		copy(buf[off+1:], protocol.U16LE(uint16(100+50*i)))   // current_mA
		copy(buf[off+3:], protocol.U16LE(uint16(1500+i*100))) // door0_us
		copy(buf[off+5:], protocol.U16LE(uint16(1600+i*100))) // door1_us
		copy(buf[off+7:], protocol.U16LE(uint16(800)))        // stall threshold
		copy(buf[off+9:], protocol.U16LE(uint16(20+i*5)))     // shunt voltage 10uV
	}

	copy(buf[33:], protocol.U16LE(1500)) // yaw_us
	buf[35] = 0x07                       // LED flags
	copy(buf[36:], protocol.U16LE(7800)) // battery mV
	buf[38] = 0x01                       // battery flags: autoDeploy (monitor always on)

	// Error reasons per gear (bytes 39-41).
	buf[39] = 0x00 // no error
	buf[40] = 0x05 // motor disconnected
	buf[41] = 0x02 // motor stall

	copy(buf[42:], protocol.U16LE(10)) // shunt 10 mOhm

	// Packed door modes (bytes 44-46): low nibble pre, high nibble post.
	buf[44] = 0x00                  // pre=0, post=0
	buf[45] = 0x02 | (0x01 << 4)    // pre=DUAL_SYNC, post=SINGLE
	buf[46] = 0x03 | (0x04 << 4)    // pre=DUAL_DELAY, post=DUAL_SEQ

	// Config flags (bytes 47-49): bit7 enabled, bit0 hasYaw.
	buf[47] = 0x80 | 0x01
	buf[48] = 0x80
	buf[49] = 0x00 // disabled
	// Door states (bytes 50-52).
	buf[50] = 1 // closed
	buf[51] = 2 // open
	buf[52] = 3 // opening
	return buf
}

func TestDecodeGearStatusBroadcast_Full(t *testing.T) {
	s := gearcontrol.DecodeStatusBroadcast(buildGearStatusFull())
	if s == nil {
		t.Fatal("DecodeStatusBroadcast returned nil for full payload")
	}

	if s.Gears[0].State != 1 || s.Gears[0].StateName != "DEPLOYED" {
		t.Errorf("gear 0 state = %d (%q), want 1 (DEPLOYED)", s.Gears[0].State, s.Gears[0].StateName)
	}
	if s.Gears[1].StateName != "RETRACTED" {
		t.Errorf("gear 1 state name = %q, want RETRACTED", s.Gears[1].StateName)
	}
	if s.Gears[2].StateName != "DEPLOYING" {
		t.Errorf("gear 2 state name = %q, want DEPLOYING", s.Gears[2].StateName)
	}

	if s.Gears[0].Current_mA != 100 || s.Gears[1].Current_mA != 150 {
		t.Errorf("current_mA 0/1 = %d/%d, want 100/150", s.Gears[0].Current_mA, s.Gears[1].Current_mA)
	}
	if s.Gears[0].Door0_us != 1500 || s.Gears[0].Door1_us != 1600 {
		t.Errorf("gear 0 door us = %d/%d, want 1500/1600", s.Gears[0].Door0_us, s.Gears[0].Door1_us)
	}

	// Shunt voltage: stored as 10uV units, exposed as mV (×0.01).
	if s.Gears[0].ShuntVoltage_10uV != 20 {
		t.Errorf("gear 0 shunt 10uV = %d, want 20", s.Gears[0].ShuntVoltage_10uV)
	}
	if s.Gears[0].ShuntVoltage_mV != 0.2 {
		t.Errorf("gear 0 shunt mV = %v, want 0.2", s.Gears[0].ShuntVoltage_mV)
	}

	if s.Yaw_us != 1500 {
		t.Errorf("yaw_us = %d, want 1500", s.Yaw_us)
	}
	if s.LEDs != 0x07 {
		t.Errorf("LEDs = 0x%02X, want 0x07", s.LEDs)
	}

	// Battery: autoDeploy bit 0x01 (monitor is always on).
	if s.Battery.Voltage_mV != 7800 {
		t.Errorf("battery mV = %d, want 7800", s.Battery.Voltage_mV)
	}
	if !s.Battery.AutoDeploy {
		t.Error("battery AutoDeploy should be true")
	}
	if s.Battery.LowVoltage {
		t.Error("battery LowVoltage should be false")
	}

	// Error reasons — per-gear.
	if s.Gears[0].ErrorReason != 0 || s.Gears[0].ErrorReasonName != "" {
		t.Errorf("gear 0 error = %d (%q), want 0", s.Gears[0].ErrorReason, s.Gears[0].ErrorReasonName)
	}
	if s.Gears[1].ErrorReason != 0x05 || s.Gears[1].ErrorReasonName != "motor disconnected" {
		t.Errorf("gear 1 error = 0x%02X (%q), want 0x05 (motor disconnected)",
			s.Gears[1].ErrorReason, s.Gears[1].ErrorReasonName)
	}
	if s.Gears[2].ErrorReason != 0x02 || s.Gears[2].ErrorReasonName != "motor stall" {
		t.Errorf("gear 2 error = 0x%02X (%q), want 0x02 (motor stall)",
			s.Gears[2].ErrorReason, s.Gears[2].ErrorReasonName)
	}

	if s.Shunt_mOhm != 10 {
		t.Errorf("shunt mOhm = %d, want 10", s.Shunt_mOhm)
	}

	// Door modes — low nibble pre, high nibble post.
	if s.Gears[1].DoorPreMode != 0x02 || s.Gears[1].DoorPostMode != 0x01 {
		t.Errorf("gear 1 door modes = %d/%d, want 2/1", s.Gears[1].DoorPreMode, s.Gears[1].DoorPostMode)
	}
	if s.Gears[2].DoorPreMode != 0x03 || s.Gears[2].DoorPostMode != 0x04 {
		t.Errorf("gear 2 door modes = %d/%d, want 3/4", s.Gears[2].DoorPreMode, s.Gears[2].DoorPostMode)
	}

	// Config flags: Enabled is bit 7, HasYaw is bit 0.
	if !s.Gears[0].Enabled || !s.Gears[0].HasYaw {
		t.Errorf("gear 0 flags: Enabled=%v HasYaw=%v, want true/true", s.Gears[0].Enabled, s.Gears[0].HasYaw)
	}
	if !s.Gears[1].Enabled || s.Gears[1].HasYaw {
		t.Errorf("gear 1 flags: Enabled=%v HasYaw=%v, want true/false", s.Gears[1].Enabled, s.Gears[1].HasYaw)
	}
	if s.Gears[2].Enabled {
		t.Error("gear 2 Enabled should be false (config flags bit7 clear)")
	}

	// Door states.
	if s.Gears[0].DoorStateName != "closed" || s.Gears[1].DoorStateName != "open" || s.Gears[2].DoorStateName != "opening" {
		t.Errorf("door state names = %q/%q/%q",
			s.Gears[0].DoorStateName, s.Gears[1].DoorStateName, s.Gears[2].DoorStateName)
	}
}

func TestDecodeGearStatusBroadcast_RejectsShort(t *testing.T) {
	if got := gearcontrol.DecodeStatusBroadcast(make([]byte, 38)); got != nil {
		t.Error("DecodeStatusBroadcast on 38-byte payload should return nil")
	}
}

func TestDecodeGearStatusBroadcast_MinimalLength(t *testing.T) {
	// 39 bytes is the minimum; only basic per-gear + yaw/leds/battery fields
	// are populated. Optional extensions stay at zero values.
	buf := make([]byte, 39)
	buf[0] = 6                            // gear 0 CALIBRATING
	copy(buf[33:], protocol.U16LE(1234))  // yaw
	copy(buf[36:], protocol.U16LE(11100)) // battery
	buf[38] = 0x00                         // no flags set

	s := gearcontrol.DecodeStatusBroadcast(buf)
	if s == nil {
		t.Fatal("DecodeStatusBroadcast returned nil for 39-byte payload")
	}
	if s.Gears[0].StateName != "CALIBRATING" {
		t.Errorf("gear 0 state name = %q, want CALIBRATING", s.Gears[0].StateName)
	}
	if s.Yaw_us != 1234 {
		t.Errorf("yaw_us = %d, want 1234", s.Yaw_us)
	}
	if s.Battery.Voltage_mV != 11100 {
		t.Errorf("battery voltage = %d, want 11100", s.Battery.Voltage_mV)
	}
	// Optional extensions must be zero.
	if s.Shunt_mOhm != 0 || s.Gears[1].ErrorReason != 0 {
		t.Errorf("optional extensions leaked: shunt=%d err=%d", s.Shunt_mOhm, s.Gears[1].ErrorReason)
	}
}

func TestDecodeCalibStatus_WithError(t *testing.T) {
	// Wire: [gearId:u8][phase:u8][current_mA:u16][peak_mA:u16][stall_mA:u16][finished:u8][errorReason:u8]
	payload := []byte{
		1,                                // gearId = 1
		7,                                // phase ERROR
		0xE8, 0x03,                       // current 1000
		0xDC, 0x05,                       // peak 1500
		0xD0, 0x07,                       // stall 2000
		1,                                // finished
		0x05,                             // errorReason = motor disconnected
	}
	c := gearcontrol.DecodeCalibStatus(payload)
	if c == nil {
		t.Fatal("DecodeCalibStatus returned nil")
	}
	if c.GearID != 1 || c.Phase != 7 || c.PhaseName != "ERROR" {
		t.Errorf("got {id=%d phase=%d phaseName=%q}, want {1 7 ERROR}",
			c.GearID, c.Phase, c.PhaseName)
	}
	if c.Current_mA != 1000 || c.Peak_mA != 1500 || c.Stall_mA != 2000 {
		t.Errorf("currents = %d/%d/%d, want 1000/1500/2000",
			c.Current_mA, c.Peak_mA, c.Stall_mA)
	}
	if !c.Finished {
		t.Error("Finished should be true")
	}
	if c.ErrorReason != 0x05 || c.ErrorReasonName != "motor disconnected" {
		t.Errorf("error = 0x%02X (%q), want 0x05 (motor disconnected)", c.ErrorReason, c.ErrorReasonName)
	}
}

func TestDecodeCalibStatus_Short(t *testing.T) {
	if got := gearcontrol.DecodeCalibStatus(make([]byte, 8)); got != nil {
		t.Error("DecodeCalibStatus on 8-byte payload should return nil")
	}
}

func TestDecodeCalibStatus_NoErrorReason(t *testing.T) {
	// 9 bytes — errorReason is optional (>=10 required).
	payload := []byte{2, 6, 0, 0, 0, 0, 0, 0, 1}
	c := gearcontrol.DecodeCalibStatus(payload)
	if c == nil {
		t.Fatal("DecodeCalibStatus returned nil for 9-byte payload")
	}
	if c.ErrorReason != 0 || c.ErrorReasonName != "" {
		t.Errorf("ErrorReason should be zero-valued when omitted, got 0x%02X (%q)",
			c.ErrorReason, c.ErrorReasonName)
	}
}

func TestDecodeSeqStatus(t *testing.T) {
	// Wire: [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]
	payload := []byte{2, 2, 1, 0, 0x20, 0x4E, 0x00, 0x00} // elapsed = 20000 ms
	s := gearcontrol.DecodeSeqStatus(payload)
	if s == nil {
		t.Fatal("DecodeSeqStatus returned nil")
	}
	if s.GearID != 2 || s.Phase != 2 || s.PhaseName != "running motor" {
		t.Errorf("got {id=%d phase=%d name=%q}, want {2 2 \"running motor\"}",
			s.GearID, s.Phase, s.PhaseName)
	}
	if !s.Deploying || s.Finished {
		t.Errorf("deploying/finished = %v/%v, want true/false", s.Deploying, s.Finished)
	}
	if s.Elapsed_ms != 20000 {
		t.Errorf("elapsed_ms = %d, want 20000", s.Elapsed_ms)
	}
}

func TestDecodeDoorStatus_PartialAndFull(t *testing.T) {
	// 2 bytes — min payload, only GearID+State.
	minimal := gearcontrol.DecodeDoorStatus([]byte{0, 2})
	if minimal == nil || minimal.StateName != "open" {
		t.Fatalf("minimal DoorStatus failed: %+v", minimal)
	}
	if minimal.HasDoor0 || minimal.HasDoor1 {
		t.Errorf("HasDoor0/1 should be false on 2-byte payload")
	}

	// 4 bytes — adds door0 position.
	with0 := gearcontrol.DecodeDoorStatus([]byte{1, 3, 0xDC, 0x05}) // door0=1500
	if !with0.HasDoor0 || with0.HasDoor1 || with0.Door0_us != 1500 {
		t.Errorf("door0-only: %+v", with0)
	}

	// 6 bytes — full.
	full := gearcontrol.DecodeDoorStatus([]byte{2, 4, 0xDC, 0x05, 0xE8, 0x03}) // door0=1500, door1=1000
	if !full.HasDoor0 || !full.HasDoor1 || full.Door0_us != 1500 || full.Door1_us != 1000 {
		t.Errorf("full DoorStatus: %+v", full)
	}
	if full.StateName != "closing" {
		t.Errorf("state name = %q, want closing", full.StateName)
	}
}

func TestDecodeDoorStatus_Short(t *testing.T) {
	if got := gearcontrol.DecodeDoorStatus([]byte{1}); got != nil {
		t.Error("DecodeDoorStatus on 1-byte payload should return nil")
	}
}
