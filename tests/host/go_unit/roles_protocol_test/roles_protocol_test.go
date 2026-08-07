package roles_protocol_test

// Unit tests for the Role-layer wire protocol (CLAUDE.md "Roles 0x40-0x7F").
//
// Like ports, roles are block-aligned by role kind:
//   0x40..0x47  generic role infrastructure (attach/detach/list/events)
//   0x48..0x4F  servo actuator
//   0x50..0x57  RC PWM input
//   0x58..0x5F  LED animator
//   0x60..0x67  DC motor
//   0x68..0x6F  bi-DC motor
//   0x70..0x77  heater + motor pct + bi-motor guard
//   0x78..0x7B  SBUS
//   0x7C..0x7F  Jeti EX

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/roles"
)

func TestRolesGenericPacketBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		// 0x40..0x47 generic
		{"ROLE_ATTACH", roles.RoleAttach, 0x40},
		{"ROLE_DETACH", roles.RoleDetach, 0x41},
		{"ROLE_LIST_REQ", roles.RoleListReq, 0x42},
		{"ROLE_LIST_RESP", roles.RoleListResp, 0x43},
		{"ROLE_ATTACHED (async)", roles.RoleAttached, 0x44},
		{"ROLE_DETACHED (async)", roles.RoleDetached, 0x45},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestRolesServoBlock(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"SERVO_SET_TARGET", roles.ServoSetTarget, 0x48},
		{"SERVO_GET_STATUS_REQ", roles.ServoGetStatusReq, 0x49},
		{"SERVO_STATUS_RESP", roles.ServoStatusResp, 0x4A},
		{"SERVO_TARGET_REACHED", roles.ServoTargetReached, 0x4B},
		{"SERVO_MOTION_UPDATE", roles.ServoMotionUpdate, 0x4C},
		{"SERVO_SET_PROFILE", roles.ServoSetProfile, 0x4D},
		{"SERVO_GET_PROFILE_REQ", roles.ServoGetProfileReq, 0x4E},
		{"SERVO_PROFILE_RESP", roles.ServoProfileResp, 0x4F},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestRolesLedBlock(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"LED_QUEUE_LOAD", roles.LedQueueLoad, 0x58},
		{"LED_START", roles.LedStart, 0x59},
		{"LED_STOP", roles.LedStop, 0x5A},
		{"LED_SET_BRIGHTNESS", roles.LedSetBrightness, 0x5B},
		{"LED_GET_STATUS_REQ", roles.LedGetStatusReq, 0x5C},
		{"LED_STATUS_RESP", roles.LedStatusResp, 0x5D},
		{"LED_QUEUE_DONE (async)", roles.LedQueueDone, 0x5E},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestRolesMotorBlock(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"MOTOR_SET_DUTY", roles.MotorSetDuty, 0x60},
		{"MOTOR_BRAKE", roles.MotorBrake, 0x61},
		{"MOTOR_GET_STATUS_REQ", roles.MotorGetStatusReq, 0x62},
		{"MOTOR_STATUS_RESP", roles.MotorStatusResp, 0x63},
		{"MOTOR_STALL_EVENT", roles.MotorStallEvent, 0x64},
		{"MOTOR_SET_ELEMENT", roles.MotorSetElement, 0x65},
		{"MOTOR_GET_ELEMENT_REQ", roles.MotorGetElementReq, 0x66},
		{"MOTOR_ELEMENT_RESP", roles.MotorElementResp, 0x67},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

func TestRolesHeaterBlock(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		{"HEATER_SET_TARGET", roles.HeaterSetTarget, 0x70},
		{"HEATER_GET_STATUS_REQ", roles.HeaterGetStatusReq, 0x71},
		{"HEATER_STATUS_RESP", roles.HeaterStatusResp, 0x72},
		{"HEATER_SET_ELEMENT", roles.HeaterSetElement, 0x73},
		{"HEATER_GET_ELEMENT_REQ", roles.HeaterGetElementReq, 0x74},
		{"HEATER_ELEMENT_RESP", roles.HeaterElementResp, 0x75},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

// RC input blocks (RCIN, SBUS, JetiEX) — pin the broadcast-rate
// and value-request bytes for each protocol.
func TestRolesRcInputBlocks(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		// 0x50..0x53 - RCIN (RC PWM)
		{"RCIN_GET_VALUE_REQ", roles.RcinGetValueReq, 0x50},
		{"RCIN_VALUE_RESP", roles.RcinValueResp, 0x51},
		{"RCIN_SET_BROADCAST_HZ", roles.RcinSetBroadcastHz, 0x52},
		{"RCIN_VALUE_BROADCAST", roles.RcinValueBroadcast, 0x53},
		// 0x78..0x7B - SBUS
		{"SBUS_GET_FRAME_REQ", roles.SbusGetFrameReq, 0x78},
		{"SBUS_FRAME_RESP", roles.SbusFrameResp, 0x79},
		{"SBUS_SET_BROADCAST_HZ", roles.SbusSetBroadcastHz, 0x7A},
		{"SBUS_FRAME_BROADCAST", roles.SbusFrameBroadcast, 0x7B},
		// 0x7C..0x7F - Jeti EX
		{"JETI_EX_GET_FRAME_REQ", roles.JetiExGetFrameReq, 0x7C},
		{"JETI_EX_FRAME_RESP", roles.JetiExFrameResp, 0x7D},
		{"JETI_EX_SET_BROADCAST_HZ", roles.JetiExSetBroadcastHz, 0x7E},
		{"JETI_EX_FRAME_BROADCAST", roles.JetiExFrameBroadcast, 0x7F},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X", tc.name, byte(tc.got), tc.want)
		}
	}
}

// BiMotor SET_GUARD Rule 11 appends (2026-08-07): the 14-byte form carries
// [12:14] ceiling_mA and leaves the peer's voltage cap untouched; passing
// elementMv emits the 16-byte form with [14:16] motor rated voltage (0 = cap
// off).  Rides SET_GUARD because 0x40..0x7F is exhausted.
func TestBiMotorSetGuardVoltageCapForms(t *testing.T) {
	short := roles.CmdBiMotorSetGuard(2, roles.BiMotorGuardLiveRatio, 80, 250, 200, 150, 0, 1200)
	_, _, p, ok := protocol.ParsePacket(short)
	if !ok {
		t.Fatal("short form: ParsePacket failed")
	}
	if len(p) != 14 {
		t.Fatalf("short form payload = %d bytes, want 14 (element untouched)", len(p))
	}

	long := roles.CmdBiMotorSetGuard(2, roles.BiMotorGuardLiveRatio, 80, 250, 200, 150, 0, 1200, 6000)
	_, _, p, ok = protocol.ParsePacket(long)
	if !ok {
		t.Fatal("long form: ParsePacket failed")
	}
	if len(p) != 16 {
		t.Fatalf("long form payload = %d bytes, want 16", len(p))
	}
	if got := uint16(p[14]) | uint16(p[15])<<8; got != 6000 {
		t.Errorf("element_mV = %d, want 6000", got)
	}
	if got := uint16(p[12]) | uint16(p[13])<<8; got != 1200 {
		t.Errorf("ceiling_mA = %d, want 1200", got)
	}
}

// BIMOTOR_STATUS_RESP voltage-cap tail: [10:12] element_mV [12:14] railNow_mV
// [14:16] capDuty.  8- and 10-byte responses (older firmware) decode with the
// cap fields zero.
func TestBiMotorStatusVoltageCapTail(t *testing.T) {
	p := make([]byte, 16)
	p[0] = 1
	p[1], p[2] = 0x10, 0x27 // duty 10000
	p[7] = 0                // not stalled
	p[8] = 2                // position B
	p[9] = 1                // guard LiveRatio
	p[10], p[11] = 0x70, 0x17 // element 6000
	p[12], p[13] = 0xB0, 0x36 // rail 14000
	p[14], p[15] = 0xBC, 0x36 // cap 14012

	st, err := roles.DecodeBiMotorStatus(p)
	if err != nil {
		t.Fatal(err)
	}
	if st.ElementMv != 6000 || st.RailNowMv != 14000 || st.CapDuty != 14012 {
		t.Errorf("cap tail = %d/%d/%d, want 6000/14000/14012", st.ElementMv, st.RailNowMv, st.CapDuty)
	}

	st, err = roles.DecodeBiMotorStatus(p[:10])
	if err != nil {
		t.Fatal(err)
	}
	if st.ElementMv != 0 || st.RailNowMv != 0 || st.CapDuty != 0 {
		t.Errorf("10-byte resp must leave cap fields zero, got %d/%d/%d", st.ElementMv, st.RailNowMv, st.CapDuty)
	}
}
