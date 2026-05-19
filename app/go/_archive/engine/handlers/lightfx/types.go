package lightfx

// ScaleFX Engine - LightFX Decoded Types
// JSON-tagged decoded structs for STATUS_BROADCAST and async events.
// Decoders are pure so both CLI formatters and GUI listeners consume the same
// typed view. See CLAUDE.md Rule 19: decoded types live here — never re-decode
// in studio/app.go or cli/*.

import (
	"scalefx/protocol"
	lfxp "scalefx/protocol/lightfx"
)

// StatusBroadcast is the LightFX periodic STATUS_BROADCAST payload (15-64 bytes).
// Wire format mirrors onStatusData() in lightfx_pico.ino. The ServoConfig block
// was appended in LightFX 0.17.0 (Rule 11 extension) — older firmware emits the
// 25-byte prefix and HasServoConfig stays false.
type StatusBroadcast struct {
	LedBrightness       [8]uint8            `json:"ledBrightness"`
	SeqPlaying          [8]bool             `json:"seqPlaying"`
	Enabled             [8]bool             `json:"enabled"`
	Servo0_us           uint16              `json:"servo0_us"`
	Servo1_us           uint16              `json:"servo1_us"`
	Servo2_us           uint16              `json:"servo2_us"`
	LandingSlots        [3]LandingSlotState `json:"landingSlots"`
	MasterBrightness    uint8               `json:"masterBrightness"`
	Battery_mV          uint16              `json:"battery_mV"`
	CellCount           uint8               `json:"cellCount"`
	BatteryPct          uint8               `json:"batteryPct"`
	BatteryPresent      bool                `json:"batteryPresent"`
	BatteryAutoCutoff   bool                `json:"batteryAutoCutoff"`
	BatteryLowTriggered bool                `json:"batteryLowTriggered"`
	HasServoConfig      bool                `json:"hasServoConfig"`
	ServoConfig         [3]ServoConfig      `json:"servoConfig"`
}

// ServoConfig is the per-servo configuration snapshot carried in the tail of
// the LightFX STATUS payload (bytes 25..63, 13 bytes per servo).
type ServoConfig struct {
	Min_us    uint16 `json:"min_us"`
	Max_us    uint16 `json:"max_us"`
	Target_us uint16 `json:"target_us"`
	Speed     uint16 `json:"speed"`
	Accel     uint16 `json:"accel"`
	Decel     uint16 `json:"decel"`
	Reversed  bool   `json:"reversed"`
}

// LandingSlotState is the per-slot landing light state in StatusBroadcast.
type LandingSlotState struct {
	Phase     uint8  `json:"phase"`
	PhaseName string `json:"phaseName"`
}

// DecodeStatusBroadcast parses a LightFX STATUS_BROADCAST payload.
// Returns nil for payloads shorter than 15 bytes.
func DecodeStatusBroadcast(data []byte) *StatusBroadcast {
	if len(data) < 15 {
		return nil
	}
	s := &StatusBroadcast{}
	for i := 0; i < 8; i++ {
		s.LedBrightness[i] = data[i]
	}
	seqFlags := data[8]
	for i := 0; i < 8; i++ {
		s.SeqPlaying[i] = seqFlags&(1<<i) != 0
	}
	s.Servo0_us = protocol.ReadU16LE(data, 9)
	s.Servo1_us = protocol.ReadU16LE(data, 11)
	s.Servo2_us = protocol.ReadU16LE(data, 13)

	if len(data) >= 18 {
		for i := 0; i < 3; i++ {
			phase := data[15+i]
			s.LandingSlots[i].Phase = phase
			s.LandingSlots[i].PhaseName = landingPhaseName(phase)
		}
	}

	s.MasterBrightness = 100
	if len(data) >= 19 {
		s.MasterBrightness = data[18]
	}

	enabledFlags := byte(0xFF)
	if len(data) >= 20 {
		enabledFlags = data[19]
	}
	for i := 0; i < 8; i++ {
		s.Enabled[i] = enabledFlags&(1<<i) != 0
	}

	if len(data) >= 24 {
		s.Battery_mV = protocol.ReadU16LE(data, 20)
		s.CellCount = data[22]
		s.BatteryPct = data[23]
		s.BatteryPresent = s.Battery_mV > 0
	}
	if len(data) >= 25 {
		flags := data[24]
		s.BatteryAutoCutoff = flags&0x01 != 0
		s.BatteryLowTriggered = flags&0x02 != 0
	}
	// Per-servo config tail (Rule 11 extension, LightFX 0.17.0+). 13 bytes
	// each × 3 servos = 39 bytes, so the full payload is 64 bytes.
	if len(data) >= 64 {
		s.HasServoConfig = true
		off := 25
		for i := 0; i < 3; i++ {
			s.ServoConfig[i] = ServoConfig{
				Min_us:    protocol.ReadU16LE(data, off+0),
				Max_us:    protocol.ReadU16LE(data, off+2),
				Target_us: protocol.ReadU16LE(data, off+4),
				Speed:     protocol.ReadU16LE(data, off+6),
				Accel:     protocol.ReadU16LE(data, off+8),
				Decel:     protocol.ReadU16LE(data, off+10),
				Reversed:  data[off+12] != 0,
			}
			off += 13
		}
	}
	return s
}

// LandingLightStatus is the async LANDING_LIGHT_STATUS packet.
// Wire: [slot:u8][phase:u8][finished:u8]
type LandingLightStatus struct {
	Slot      uint8  `json:"slot"`
	Phase     uint8  `json:"phase"`
	PhaseName string `json:"phaseName"`
	Finished  bool   `json:"finished"`
}

// DecodeLandingLightStatus parses a LANDING_LIGHT_STATUS async payload.
func DecodeLandingLightStatus(payload []byte) *LandingLightStatus {
	if len(payload) < 3 {
		return nil
	}
	return &LandingLightStatus{
		Slot:      payload[0],
		Phase:     payload[1],
		PhaseName: lfxp.LandingLightPhaseName(payload[1]),
		Finished:  payload[2] != 0,
	}
}

// landingPhaseName mirrors the small map used by the CLI status formatter so
// JSON consumers get the same short labels ("RET", "DEP", …).
func landingPhaseName(phase uint8) string {
	switch phase {
	case 0:
		return "RET"
	case 1:
		return "DEPLOYING"
	case 2:
		return "DEP"
	case 3:
		return "RETRACTING"
	}
	return ""
}
