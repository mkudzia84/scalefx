package gearcontrol

// GearControl Protocol — mirrors serial/gearcontrol/gearcontrol.h
// Landing gear deploy/retract, servo, yaw, calibration, battery monitoring.

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet Types (0x60-0x7F) ───

const (
	// Gear control
	GearDeploy     protocol.PacketType = 0x60
	GearRetract    protocol.PacketType = 0x61
	GearStop       protocol.PacketType = 0x62
	GearAll        protocol.PacketType = 0x63

	// Servo control
	ServoSet       protocol.PacketType = 0x64
	ServoSettings  protocol.PacketType = 0x65

	// Configuration
	GearConfig     protocol.PacketType = 0x66
	YawConfig      protocol.PacketType = 0x68
	YawInput       protocol.PacketType = 0x69

	// Calibration
	GearCalibrate  protocol.PacketType = 0x6A
	GearCalibStatus protocol.PacketType = 0x6B
	GearCalibCancel protocol.PacketType = 0x6C

	// Battery — GearControl-specific safety toggle.
	// Sensor configuration (chemistry / cell count) flows through the generic
	// CorePacket::BATTERY_CONFIG handler in protocol/core.
	BatteryAutoDeploy protocol.PacketType = 0x6D

	// Door mode
	DoorMode       protocol.PacketType = 0x6E

	// Reset and enable
	GearReset      protocol.PacketType = 0x6F
	GearSeqStatus  protocol.PacketType = 0x70
	GearEnable     protocol.PacketType = 0x71
	GearDoorStatus protocol.PacketType = 0x72
)

// ─── Error Codes (0x60-0x6F) ───

const (
	ErrInvalidGearId    protocol.ErrorCode = 0x60
	ErrInvalidServoId   protocol.ErrorCode = 0x61
	ErrGearBusy         protocol.ErrorCode = 0x62
	ErrMotorStall       protocol.ErrorCode = 0x63
	ErrMotorTimeout     protocol.ErrorCode = 0x64
	ErrServoOutOfRange  protocol.ErrorCode = 0x65
	ErrIna226Error      protocol.ErrorCode = 0x66
	ErrYawNotAvailable  protocol.ErrorCode = 0x67
	ErrInvalidAction    protocol.ErrorCode = 0x68
	ErrNoCurrentMonitor protocol.ErrorCode = 0x69
	ErrNotCalibrating   protocol.ErrorCode = 0x6A
	ErrGearDisabled     protocol.ErrorCode = 0x6B
)

// ─── Commands ───

func CmdDeploy(gearID byte) []byte {
	return protocol.BuildPacket(GearDeploy, []byte{gearID}, 0)
}

func CmdRetract(gearID byte) []byte {
	return protocol.BuildPacket(GearRetract, []byte{gearID}, 0)
}

func CmdStop(gearID byte) []byte {
	return protocol.BuildPacket(GearStop, []byte{gearID}, 0)
}

func CmdAll(action byte) []byte {
	return protocol.BuildPacket(GearAll, []byte{action}, 0)
}

func CmdServoSet(id byte, pulse_us uint16) []byte {
	payload := []byte{id}
	payload = append(payload, protocol.U16LE(pulse_us)...)
	return protocol.BuildPacket(ServoSet, payload, 0)
}

func CmdServoSettings(id byte, minPulse, maxPulse, speed, accel, decel uint16, reversed bool) []byte {
	payload := []byte{id}
	payload = append(payload, protocol.U16LE(minPulse)...)
	payload = append(payload, protocol.U16LE(maxPulse)...)
	payload = append(payload, protocol.U16LE(speed)...)
	payload = append(payload, protocol.U16LE(accel)...)
	payload = append(payload, protocol.U16LE(decel)...)
	if reversed {
		payload = append(payload, 1)
	}
	return protocol.BuildPacket(ServoSettings, payload, 0)
}

func CmdGearConfig(gearID byte, flags byte, stall_mA uint16, timeout_ms uint16) []byte {
	payload := []byte{gearID, flags}
	payload = append(payload, protocol.U16LE(stall_mA)...)
	payload = append(payload, protocol.U16LE(timeout_ms)...)
	return protocol.BuildPacket(GearConfig, payload, 0)
}

func CmdYawConfig(gearID byte, neutral, min, max uint16) []byte {
	payload := []byte{gearID}
	payload = append(payload, protocol.U16LE(neutral)...)
	payload = append(payload, protocol.U16LE(min)...)
	payload = append(payload, protocol.U16LE(max)...)
	return protocol.BuildPacket(YawConfig, payload, 0)
}

func CmdYawInput(position_us uint16) []byte {
	return protocol.BuildPacket(YawInput, protocol.U16LE(position_us), 0)
}

func CmdCalibrate(gearID byte, timeout_s byte) []byte {
	payload := []byte{gearID}
	if timeout_s > 0 {
		payload = append(payload, timeout_s)
	}
	return protocol.BuildPacket(GearCalibrate, payload, 0)
}

func CmdCalibCancel(gearID byte) []byte {
	return protocol.BuildPacket(GearCalibCancel, []byte{gearID}, 0)
}

// CmdBatteryAutoDeploy — 1-byte payload: [enabled].
// GearControl-specific safety: deploy gear automatically on low voltage.
// Sensor configuration (chemistry / cellCount) goes through core.CmdBatteryConfig.
func CmdBatteryAutoDeploy(enabled bool) []byte {
	return protocol.BuildPacket(BatteryAutoDeploy, []byte{boolByte(enabled)}, 0)
}

func boolByte(b bool) byte {
	if b {
		return 1
	}
	return 0
}

func CmdDoorMode(gearID, preDeploy, postDeploy byte, delay_ms uint16) []byte {
	payload := []byte{gearID, preDeploy, postDeploy}
	payload = append(payload, protocol.U16LE(delay_ms)...)
	return protocol.BuildPacket(DoorMode, payload, 0)
}

func CmdReset(gearID byte) []byte {
	return protocol.BuildPacket(GearReset, []byte{gearID}, 0)
}

func CmdEnable(gearID byte, enabled bool) []byte {
	e := byte(0)
	if enabled {
		e = 1
	}
	return protocol.BuildPacket(GearEnable, []byte{gearID, e}, 0)
}

// ─── Name Lookups ───

// GearStateName returns gear state name.
func GearStateName(state byte) string {
	names := map[byte]string{
		0: "UNKNOWN", 1: "DEPLOYED", 2: "RETRACTED",
		3: "DEPLOYING", 4: "RETRACTING", 5: "ERROR", 6: "CALIBRATING",
	}
	if n, ok := names[state]; ok {
		return n
	}
	return fmt.Sprintf("?(%d)", state)
}

// DoorModeName returns door mode name.
func DoorModeName(mode byte) string {
	names := map[byte]string{
		0: "NONE", 1: "SINGLE", 2: "DUAL_SYNC", 3: "DUAL_DELAY", 4: "DUAL_SEQ",
	}
	if n, ok := names[mode]; ok {
		return n
	}
	return fmt.Sprintf("?(%d)", mode)
}

// DoorStateName returns door state name.
func DoorStateName(state byte) string {
	names := map[byte]string{
		0: "unknown", 1: "closed", 2: "open", 3: "opening", 4: "closing",
	}
	if n, ok := names[state]; ok {
		return n
	}
	return fmt.Sprintf("?(%d)", state)
}

// GearErrorReasonName returns gear error reason name.
func GearErrorReasonName(reason byte) string {
	names := map[byte]string{
		0x00: "none",
		0x01: "INA226 init failed",
		0x02: "motor stall",
		0x03: "motor timeout",
		0x04: "sequence error",
		0x05: "motor disconnected",
		0x06: "calibration timeout",
		0x07: "no stall detected",
	}
	if n, ok := names[reason]; ok {
		return n
	}
	return fmt.Sprintf("unknown(0x%02X)", reason)
}

// GearSeqPhaseName returns gear sequence phase name.
func GearSeqPhaseName(phase byte) string {
	names := map[byte]string{
		0: "idle", 1: "opening doors", 2: "running motor",
		3: "closing doors", 4: "error", 5: "sync wait",
	}
	if n, ok := names[phase]; ok {
		return n
	}
	return fmt.Sprintf("unknown(%d)", phase)
}

// ─── Name Registration ───

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		GearDeploy:     "GC.GEAR_DEPLOY",
		GearRetract:    "GC.GEAR_RETRACT",
		GearStop:       "GC.GEAR_STOP",
		GearAll:        "GC.GEAR_ALL",
		ServoSet:       "GC.SERVO_SET",
		ServoSettings:  "GC.SRV_SETTINGS",
		GearConfig:     "GC.GEAR_CONFIG",
		YawConfig:      "GC.YAW_CONFIG",
		YawInput:       "GC.YAW_INPUT",
		GearCalibrate:  "GC.GEAR_CALIBRATE",
		GearCalibStatus: "GC.CALIB_STATUS",
		GearCalibCancel: "GC.CALIB_CANCEL",
		BatteryAutoDeploy: "GC.BATTERY_AUTO_DEPLOY",
		DoorMode:       "GC.DOOR_MODE",
		GearReset:      "GC.GEAR_RESET",
		GearSeqStatus:  "GC.GC_SEQ_STATUS",
		GearEnable:     "GC.GEAR_ENABLE",
		GearDoorStatus: "GC.DOOR_STATUS",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrInvalidGearId:    "INVALID_GEAR_ID",
		ErrInvalidServoId:   "GC_INVALID_SERVO_ID",
		ErrGearBusy:         "GEAR_BUSY",
		ErrMotorStall:       "MOTOR_STALL",
		ErrMotorTimeout:     "MOTOR_TIMEOUT",
		ErrServoOutOfRange:  "SERVO_OUT_OF_RANGE",
		ErrIna226Error:      "INA226_ERROR",
		ErrYawNotAvailable:  "YAW_NOT_AVAILABLE",
		ErrInvalidAction:    "INVALID_ACTION",
		ErrNoCurrentMonitor: "NO_CURRENT_MONITOR",
		ErrNotCalibrating:   "NOT_CALIBRATING",
		ErrGearDisabled:     "GEAR_DISABLED",
	})
}
