package gunfx

// ScaleFX Engine - GunFX Decoded Types
// JSON-tagged decoded structs for STATUS_BROADCAST. Decoders are pure so both
// CLI formatters and GUI listeners consume the same typed view. See CLAUDE.md
// Rule 19: decoded types live here — never re-decode in studio/app.go.

import (
	"scalefx/protocol"
	gfxp "scalefx/protocol/gunfx"
)

// StatusBroadcast is the GunFX periodic STATUS_BROADCAST payload (20-67 bytes).
// Wire format mirrors onStatusData() in gunfx_pico.ino. The ServoConfig block
// was appended in GunFX 0.8.0 (Rule 11 extension, aligned with LightFX's
// 0.17.0 tail) — older firmware emits the 28-byte prefix and HasServoConfig
// stays false.
type StatusBroadcast struct {
	// Flags (data[0])
	Firing      bool `json:"firing"`
	FlashActive bool `json:"flashActive"`
	FlashFading bool `json:"flashFading"`
	HeaterOn    bool `json:"heaterOn"`
	FanOn       bool `json:"fanOn"`
	FanSpindown bool `json:"fanSpindown"`

	FanSpeed    uint8  `json:"fanSpeed"`
	FanOff_ms   uint16 `json:"fanOff_ms"`
	Servo0_us   uint16 `json:"servo0_us"`
	Servo1_us   uint16 `json:"servo1_us"`
	Servo2_us   uint16 `json:"servo2_us"`
	RPM         uint16 `json:"rpm"`
	Shots       uint32 `json:"shots"`
	Heater_ms   uint32 `json:"heater_ms"`

	// Smoke error reasons (optional, bytes 20-21)
	HeaterError     uint8  `json:"heaterError"`
	HeaterErrorName string `json:"heaterErrorName"`
	FanError        uint8  `json:"fanError"`
	FanErrorName    string `json:"fanErrorName"`

	// Overcurrent throttle (optional, bytes 22-23)
	HeaterDuty uint8 `json:"heaterDuty"`
	FanDuty    uint8 `json:"fanDuty"`

	// Battery (optional, bytes 24-27)
	Battery_mV     uint16 `json:"battery_mV"`
	CellCount      uint8  `json:"cellCount"`
	BatteryPct     uint8  `json:"batteryPct"`
	BatteryPresent bool   `json:"batteryPresent"`

	// Per-servo config tail (optional, bytes 28-66). Index i maps to
	// user-facing servoId i+1 (GunFX is 1-based: servos 1..3).
	HasServoConfig bool           `json:"hasServoConfig"`
	ServoConfig    [3]ServoConfig `json:"servoConfig"`

	// Trigger input (optional, bytes 67-68, added in GunFX v0.8.1).
	// GP0 RC PWM pulse width; 0 = no signal / stale.
	HasTriggerInput  bool   `json:"hasTriggerInput"`
	TriggerInput_us  uint16 `json:"triggerInput_us"`
}

// ServoConfig is the 13-byte per-servo configuration appended to the GunFX
// STATUS payload (mirror of LightFX's ServoConfig so both boards decode the
// same struct through their respective handlers).
type ServoConfig struct {
	Min_us    uint16 `json:"min_us"`
	Max_us    uint16 `json:"max_us"`
	Target_us uint16 `json:"target_us"`
	Speed     uint16 `json:"speed"`
	Accel     uint16 `json:"accel"`
	Decel     uint16 `json:"decel"`
	Reversed  bool   `json:"reversed"`
}

// DecodeStatusBroadcast parses a GunFX STATUS_BROADCAST payload.
// Returns nil for payloads shorter than the required 20 bytes.
func DecodeStatusBroadcast(data []byte) *StatusBroadcast {
	if len(data) < 20 {
		return nil
	}
	s := &StatusBroadcast{}

	flags := data[0]
	s.Firing = flags&0x01 != 0
	s.FlashActive = flags&0x02 != 0
	s.FlashFading = flags&0x04 != 0
	s.HeaterOn = flags&0x08 != 0
	s.FanOn = flags&0x10 != 0
	s.FanSpindown = flags&0x20 != 0

	s.FanSpeed = data[1]
	s.FanOff_ms = protocol.ReadU16LE(data, 2)
	s.Servo0_us = protocol.ReadU16LE(data, 4)
	s.Servo1_us = protocol.ReadU16LE(data, 6)
	s.Servo2_us = protocol.ReadU16LE(data, 8)
	s.RPM = protocol.ReadU16LE(data, 10)
	s.Shots = protocol.ReadU32LE(data, 12)
	s.Heater_ms = protocol.ReadU32LE(data, 16)

	if len(data) >= 22 {
		s.HeaterError = data[20]
		s.FanError = data[21]
		if s.HeaterError != 0 {
			s.HeaterErrorName = gfxp.SmokeErrorReasonName(s.HeaterError)
		}
		if s.FanError != 0 {
			s.FanErrorName = gfxp.SmokeErrorReasonName(s.FanError)
		}
	}

	if len(data) >= 24 {
		s.HeaterDuty = data[22]
		s.FanDuty = data[23]
	} else {
		s.HeaterDuty = 255
		s.FanDuty = 255
	}

	if len(data) >= 28 {
		s.Battery_mV = protocol.ReadU16LE(data, 24)
		s.CellCount = data[26]
		s.BatteryPct = data[27]
		s.BatteryPresent = s.Battery_mV > 0
	}

	// Per-servo config tail (Rule 11 extension, GunFX 0.8.0+). 13 bytes each
	// × 3 servos = 39 bytes, so the full payload is 67 bytes.
	if len(data) >= 67 {
		s.HasServoConfig = true
		off := 28
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

	// Trigger input tail (GunFX 0.8.1+, Rule 11). 2 bytes at offset 67.
	if len(data) >= 69 {
		s.HasTriggerInput = true
		s.TriggerInput_us = protocol.ReadU16LE(data, 67)
	}

	return s
}
