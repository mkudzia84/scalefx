package ports_protocol_test

// Unit tests for the Port-layer wire protocol (CLAUDE.md "Ports 0x10-0x3F").
//
// Ports are partitioned into 8-byte blocks by kind:
//   0x10..0x17  generic port enumeration
//   0x18..0x1F  servo
//   0x20..0x27  PWM (+ sense)
//   0x28..0x2F  reserved
//   0x30..0x37  H-bridge
//   0x38..0x3F  input
//
// The block alignment is what lets BoardOf<>::ownsType() route by
// upper nibble; a drift here breaks dispatch silently.

import (
	"testing"

	"scalefx/protocol"
	"scalefx/protocol/ports"
)

func TestPortsPacketTypeBytes(t *testing.T) {
	tests := []struct {
		name string
		got  protocol.PacketType
		want byte
	}{
		// 0x10..0x17 - generic port enumeration
		{"PORT_LIST_REQ", ports.PortListReq, 0x10},
		{"PORT_LIST_RESP", ports.PortListResp, 0x11},
		// 0x18..0x1F - servo
		{"SERVO_PORT_SET_US", ports.ServoPortSetUs, 0x18},
		{"SERVO_PORT_READ_US", ports.ServoPortReadUs, 0x19},
		{"SERVO_PORT_READ_RESP", ports.ServoPortReadResp, 0x1A},
		// 0x20..0x27 - PWM
		{"PWM_PORT_SET_DUTY", ports.PwmPortSetDuty, 0x20},
		{"PWM_PORT_SET_FREQ", ports.PwmPortSetFreq, 0x21},
		{"PWM_PORT_READ_SENSE", ports.PwmPortReadSense, 0x22},
		{"PWM_PORT_SENSE_RESP", ports.PwmPortSenseResp, 0x23},
		// 0x30..0x37 - H-bridge
		{"HBRIDGE_SET_SIGNED", ports.HBridgeSetSigned, 0x30},
		{"HBRIDGE_BRAKE", ports.HBridgeBrake, 0x31},
		{"HBRIDGE_COAST", ports.HBridgeCoast, 0x32},
		{"HBRIDGE_READ_SENSE", ports.HBridgeReadSense, 0x33},
		{"HBRIDGE_SENSE_RESP", ports.HBridgeSenseResp, 0x34},
		// 0x38..0x3F - input
		{"INPUT_READ_PULSE", ports.InputReadPulse, 0x38},
		{"INPUT_PULSE_RESP", ports.InputPulseResp, 0x39},
		{"INPUT_GET_MODE", ports.InputGetMode, 0x3A},
		{"INPUT_MODE_RESP", ports.InputModeResp, 0x3B},
	}
	for _, tc := range tests {
		if byte(tc.got) != tc.want {
			t.Errorf("%s = 0x%02X, want 0x%02X — block-alignment drift "+
				"will break BoardOf<>::ownsType() upper-nibble dispatch",
				tc.name, byte(tc.got), tc.want)
		}
	}
}

// Confirm each block is in its expected nibble range — guards against
// "wandered to wrong block" drift (e.g. ServoPortSetUs accidentally
// at 0x28 not 0x18).
func TestPortsBlockAlignment(t *testing.T) {
	checks := []struct {
		name      string
		got       protocol.PacketType
		minNibble byte
		maxNibble byte
	}{
		{"servo", ports.ServoPortSetUs, 0x18, 0x1F},
		{"pwm", ports.PwmPortSetDuty, 0x20, 0x27},
		{"hbridge", ports.HBridgeSetSigned, 0x30, 0x37},
		{"input", ports.InputReadPulse, 0x38, 0x3F},
	}
	for _, tc := range checks {
		b := byte(tc.got)
		if b < tc.minNibble || b > tc.maxNibble {
			t.Errorf("%s block: 0x%02X outside [0x%02X..0x%02X]",
				tc.name, b, tc.minNibble, tc.maxNibble)
		}
	}
}
