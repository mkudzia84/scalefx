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

// StatusBroadcast is the LightFX periodic STATUS_BROADCAST payload (15-24 bytes).
// Wire format mirrors onStatusData() in lightfx_pico.ino.
type StatusBroadcast struct {
	LedBrightness   [8]uint8            `json:"ledBrightness"`
	SeqPlaying      [8]bool             `json:"seqPlaying"`
	Enabled         [8]bool             `json:"enabled"`
	Servo0_us       uint16              `json:"servo0_us"`
	Servo1_us       uint16              `json:"servo1_us"`
	Servo2_us       uint16              `json:"servo2_us"`
	LandingSlots    [3]LandingSlotState `json:"landingSlots"`
	MasterBrightness uint8              `json:"masterBrightness"`
	Battery_mV      uint16              `json:"battery_mV"`
	CellCount       uint8               `json:"cellCount"`
	BatteryPct      uint8               `json:"batteryPct"`
	BatteryPresent  bool                `json:"batteryPresent"`
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
