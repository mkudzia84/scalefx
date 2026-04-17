package gearcontrol

// ScaleFX Engine - GearControl Decoded Types
// JSON-tagged decoded structs for async/broadcast events. Decoders are pure
// (no I/O, no formatting) so both CLI formatters and GUI listeners consume
// the same typed view. See CLAUDE.md Rule 19: decoded types live here — never
// re-decode in studio/app.go or cli/*.

import (
	"scalefx/protocol"
	gcp "scalefx/protocol/gearcontrol"
)

// StatusBroadcast is the 53-byte GearControl periodic STATUS_BROADCAST payload.
// Wire format mirrors onStatusData() in gearcontrol_pico.ino.
type StatusBroadcast struct {
	Gears   [3]GearStatus `json:"gears"`
	Yaw_us  uint16        `json:"yaw_us"`
	LEDs    uint8         `json:"leds"`
	Battery BatteryStatus `json:"battery"`
	Shunt_mOhm uint16     `json:"shunt_mOhm"`
}

// GearStatus is the per-gear slice of a StatusBroadcast.
type GearStatus struct {
	State             uint8   `json:"state"`
	StateName         string  `json:"stateName"`
	Current_mA        uint16  `json:"current_mA"`
	Door0_us          uint16  `json:"door0_us"`
	Door1_us          uint16  `json:"door1_us"`
	StallThreshold_mA uint16  `json:"stallThreshold_mA"`
	ShuntVoltage_10uV int16   `json:"shuntVoltage_10uV"`
	ShuntVoltage_mV   float64 `json:"shuntVoltage_mV"`
	ErrorReason       uint8   `json:"errorReason"`
	ErrorReasonName   string  `json:"errorReasonName"`
	DoorPreMode       uint8   `json:"doorPreMode"`
	DoorPostMode      uint8   `json:"doorPostMode"`
	ConfigFlags       uint8   `json:"configFlags"`
	Enabled           bool    `json:"enabled"`
	HasYaw            bool    `json:"hasYaw"`
	DoorState         uint8   `json:"doorState"`
	DoorStateName     string  `json:"doorStateName"`
}

// BatteryStatus is the global battery slice of a StatusBroadcast.
// Monitor is always running; the device-side enable byte is gone.
type BatteryStatus struct {
	Voltage_mV uint16 `json:"voltage_mV"`
	AutoDeploy bool   `json:"autoDeploy"`
	LowVoltage bool   `json:"lowVoltage"`
}

// CalibStatus is the 10-byte GEAR_CALIB_STATUS async payload.
// Wire: [gear_id:u8][phase:u8][current_mA:u16][peak_mA:u16][stall_mA:u16][finished:u8][errorReason:u8]
type CalibStatus struct {
	GearID          uint8  `json:"gearId"`
	Phase           uint8  `json:"phase"`
	PhaseName       string `json:"phaseName"`
	Current_mA      uint16 `json:"current_mA"`
	Peak_mA         uint16 `json:"peak_mA"`
	Stall_mA        uint16 `json:"stall_mA"`
	Finished        bool   `json:"finished"`
	ErrorReason     uint8  `json:"errorReason"`
	ErrorReasonName string `json:"errorReasonName"`
}

// SeqStatus is the 8-byte GEAR_SEQ_STATUS async payload.
// Wire: [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]
type SeqStatus struct {
	GearID    uint8  `json:"gearId"`
	Phase     uint8  `json:"phase"`
	PhaseName string `json:"phaseName"`
	Deploying bool   `json:"deploying"`
	Finished  bool   `json:"finished"`
	Elapsed_ms uint32 `json:"elapsed_ms"`
}

// DoorStatus is the 6-byte GEAR_DOOR_STATUS async payload.
// Wire: [gear_id:u8][state:u8][door0_pos_us:u16LE][door1_pos_us:u16LE]
type DoorStatus struct {
	GearID    uint8  `json:"gearId"`
	State     uint8  `json:"state"`
	StateName string `json:"stateName"`
	Door0_us  uint16 `json:"door0_us"`
	Door1_us  uint16 `json:"door1_us"`
	HasDoor0  bool   `json:"hasDoor0"`
	HasDoor1  bool   `json:"hasDoor1"`
}

// calibPhaseName returns a human-readable calibration phase name.
// Shared by CLI formatter and Decode* consumers.
func calibPhaseName(phase byte) string {
	names := map[byte]string{
		0: "idle", 1: "clear run", 2: "clear settle", 3: "deploy run",
		4: "mid settle", 5: "retract run", 6: "complete", 7: "ERROR",
		8: "cancelled", 9: "opening doors", 10: "closing doors",
	}
	if n, ok := names[phase]; ok {
		return n
	}
	return ""
}

// DecodeStatusBroadcast parses a GearControl STATUS_BROADCAST payload.
// Returns nil for payloads shorter than the required 39 bytes.
func DecodeStatusBroadcast(data []byte) *StatusBroadcast {
	if len(data) < 39 {
		return nil
	}
	s := &StatusBroadcast{}

	// Per-gear blocks (3 × 11 = 33 bytes)
	for i := 0; i < 3; i++ {
		off := i * 11
		g := &s.Gears[i]
		g.State = data[off]
		g.StateName = gcp.GearStateName(g.State)
		g.Current_mA = protocol.ReadU16LE(data, off+1)
		g.Door0_us = protocol.ReadU16LE(data, off+3)
		g.Door1_us = protocol.ReadU16LE(data, off+5)
		g.StallThreshold_mA = protocol.ReadU16LE(data, off+7)
		g.ShuntVoltage_10uV = protocol.ReadI16LE(data, off+9)
		g.ShuntVoltage_mV = float64(g.ShuntVoltage_10uV) * 10.0 / 1000.0
	}

	// Global: yaw, led_flags, battery
	s.Yaw_us = protocol.ReadU16LE(data, 33)
	s.LEDs = data[35]
	batteryMV := protocol.ReadU16LE(data, 36)
	batteryFlags := data[38]
	s.Battery.Voltage_mV = batteryMV
	s.Battery.AutoDeploy = batteryFlags&0x01 != 0
	s.Battery.LowVoltage = batteryFlags&0x02 != 0

	// Per-gear error reasons (bytes 39-41)
	if len(data) >= 42 {
		for i := 0; i < 3; i++ {
			r := data[39+i]
			s.Gears[i].ErrorReason = r
			if r != 0 {
				s.Gears[i].ErrorReasonName = gcp.GearErrorReasonName(r)
			}
		}
	}

	// Shunt resistance (bytes 42-43)
	if len(data) >= 44 {
		s.Shunt_mOhm = protocol.ReadU16LE(data, 42)
	}

	// Packed door modes per gear (bytes 44-46)
	if len(data) >= 47 {
		for i := 0; i < 3; i++ {
			packed := data[44+i]
			s.Gears[i].DoorPreMode = packed & 0x0F
			s.Gears[i].DoorPostMode = (packed >> 4) & 0x0F
		}
	}

	// Config flags per gear (bytes 47-49)
	if len(data) >= 50 {
		for i := 0; i < 3; i++ {
			cf := data[47+i]
			s.Gears[i].ConfigFlags = cf
			s.Gears[i].Enabled = cf&0x80 != 0
			s.Gears[i].HasYaw = cf&0x01 != 0
		}
	}

	// Door state per gear (bytes 50-52)
	if len(data) >= 53 {
		for i := 0; i < 3; i++ {
			st := data[50+i]
			s.Gears[i].DoorState = st
			s.Gears[i].DoorStateName = gcp.DoorStateName(st)
		}
	}

	return s
}

// DecodeCalibStatus parses a GEAR_CALIB_STATUS async payload.
// Returns nil for payloads shorter than 9 bytes (errorReason is optional).
func DecodeCalibStatus(payload []byte) *CalibStatus {
	if len(payload) < 9 {
		return nil
	}
	c := &CalibStatus{
		GearID:     payload[0],
		Phase:      payload[1],
		Current_mA: protocol.ReadU16LE(payload, 2),
		Peak_mA:    protocol.ReadU16LE(payload, 4),
		Stall_mA:   protocol.ReadU16LE(payload, 6),
		Finished:   payload[8] != 0,
	}
	if len(payload) >= 10 {
		c.ErrorReason = payload[9]
	}
	c.PhaseName = calibPhaseName(c.Phase)
	if c.ErrorReason != 0 {
		c.ErrorReasonName = gcp.GearErrorReasonName(c.ErrorReason)
	}
	return c
}

// DecodeSeqStatus parses a GEAR_SEQ_STATUS async payload.
func DecodeSeqStatus(payload []byte) *SeqStatus {
	if len(payload) < 8 {
		return nil
	}
	return &SeqStatus{
		GearID:     payload[0],
		Phase:      payload[1],
		PhaseName:  gcp.GearSeqPhaseName(payload[1]),
		Deploying:  payload[2] != 0,
		Finished:   payload[3] != 0,
		Elapsed_ms: protocol.ReadU32LE(payload, 4),
	}
}

// DecodeDoorStatus parses a GEAR_DOOR_STATUS async payload.
func DecodeDoorStatus(payload []byte) *DoorStatus {
	if len(payload) < 2 {
		return nil
	}
	d := &DoorStatus{
		GearID:    payload[0],
		State:     payload[1],
		StateName: gcp.DoorStateName(payload[1]),
	}
	if len(payload) >= 4 {
		d.Door0_us = protocol.ReadU16LE(payload, 2)
		d.HasDoor0 = true
	}
	if len(payload) >= 6 {
		d.Door1_us = protocol.ReadU16LE(payload, 4)
		d.HasDoor1 = true
	}
	return d
}
