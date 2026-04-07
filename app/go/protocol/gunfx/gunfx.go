package gunfx

// GunFX Protocol — mirrors serial/gunfx/gunfx.h
// Trigger, servo, and smoke control commands.

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet Types (0x01-0x2F) ───

const (
	TriggerOn         protocol.PacketType = 0x01
	TriggerOff        protocol.PacketType = 0x02
	ServoSet          protocol.PacketType = 0x10
	ServoSettings     protocol.PacketType = 0x11
	ServoRecoil       protocol.PacketType = 0x12
	SmokeHeat         protocol.PacketType = 0x20
	SmokeSettings     protocol.PacketType = 0x21
	SmokeReset        protocol.PacketType = 0x22
	SmokeCurrentLimit protocol.PacketType = 0x23
)

// ─── Error Codes (0x20-0x4F) ───

const (
	ErrServoInvalidId    protocol.ErrorCode = 0x20
	ErrServoPulseRange   protocol.ErrorCode = 0x21
	ErrServoMinMax       protocol.ErrorCode = 0x22
	ErrServoNotConfigured protocol.ErrorCode = 0x23
	ErrInvalidFanSpeed   protocol.ErrorCode = 0x30
	ErrHeaterDisconnected protocol.ErrorCode = 0x31
	ErrFanDisconnected   protocol.ErrorCode = 0x32
	ErrHeaterOvercurrent protocol.ErrorCode = 0x33
	ErrFanOvercurrent    protocol.ErrorCode = 0x34
	ErrInvalidRpm        protocol.ErrorCode = 0x40
	ErrAlreadyFiring     protocol.ErrorCode = 0x41
	ErrNotFiring         protocol.ErrorCode = 0x42
)

// ─── Commands ───

func CmdTriggerOn(rpm uint16) []byte {
	return protocol.BuildPacket(TriggerOn, protocol.U16LE(rpm), 0)
}

func CmdTriggerOff(delay_ms uint16) []byte {
	return protocol.BuildPacket(TriggerOff, protocol.U16LE(delay_ms), 0)
}

func CmdServoSet(id byte, pulse_us uint16) []byte {
	payload := []byte{id}
	payload = append(payload, protocol.U16LE(pulse_us)...)
	return protocol.BuildPacket(ServoSet, payload, 0)
}

func CmdServoSettings(id byte, minPulse, maxPulse, speed, accel, decel uint16) []byte {
	payload := []byte{id}
	payload = append(payload, protocol.U16LE(minPulse)...)
	payload = append(payload, protocol.U16LE(maxPulse)...)
	payload = append(payload, protocol.U16LE(speed)...)
	payload = append(payload, protocol.U16LE(accel)...)
	payload = append(payload, protocol.U16LE(decel)...)
	return protocol.BuildPacket(ServoSettings, payload, 0)
}

func CmdServoRecoil(id byte, jerk_us, variance_us uint16) []byte {
	payload := []byte{id}
	payload = append(payload, protocol.U16LE(jerk_us)...)
	payload = append(payload, protocol.U16LE(variance_us)...)
	return protocol.BuildPacket(ServoRecoil, payload, 0)
}

func CmdSmokeHeat(on bool) []byte {
	v := byte(0)
	if on {
		v = 1
	}
	return protocol.BuildPacket(SmokeHeat, []byte{v}, 0)
}

func CmdSmokeSettings(pulsing bool, speed, high, low byte, pulse_ms, spindown_ms uint16) []byte {
	p := byte(0)
	if pulsing {
		p = 1
	}
	payload := []byte{p, speed, high, low}
	payload = append(payload, protocol.U16LE(pulse_ms)...)
	payload = append(payload, protocol.U16LE(spindown_ms)...)
	return protocol.BuildPacket(SmokeSettings, payload, 0)
}

func CmdSmokeReset() []byte {
	return protocol.BuildPacket(SmokeReset, nil, 0)
}

func CmdSmokeCurrentLimit(target byte, limit_mA uint16) []byte {
	payload := []byte{target}
	payload = append(payload, protocol.U16LE(limit_mA)...)
	return protocol.BuildPacket(SmokeCurrentLimit, payload, 0)
}

// ─── Name Lookups ───

// SmokeErrorReasonName returns smoke error reason name.
func SmokeErrorReasonName(reason byte) string {
	names := map[byte]string{
		0x00: "none",
		0x01: "heater disconnected",
		0x02: "fan disconnected",
		0x03: "heater overcurrent",
		0x04: "fan overcurrent",
	}
	if n, ok := names[reason]; ok {
		return n
	}
	return fmt.Sprintf("unknown(0x%02X)", reason)
}

// ─── Name Registration ───

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		TriggerOn:         "GFX.TRIGGER_ON",
		TriggerOff:        "GFX.TRIGGER_OFF",
		ServoSet:          "GFX.SERVO_SET",
		ServoSettings:     "GFX.SERVO_SETTINGS",
		ServoRecoil:       "GFX.SERVO_RECOIL",
		SmokeHeat:         "GFX.SMOKE_HEAT",
		SmokeSettings:     "GFX.SMOKE_SETTINGS",
		SmokeReset:        "GFX.SMOKE_RESET",
		SmokeCurrentLimit: "GFX.SMOKE_CURRENT_LIMIT",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrServoInvalidId:    "SERVO_INVALID_ID",
		ErrServoPulseRange:   "SERVO_PULSE_RANGE",
		ErrServoMinMax:       "SERVO_MIN_MAX",
		ErrServoNotConfigured: "SERVO_NOT_CONFIGURED",
		ErrInvalidFanSpeed:   "INVALID_FAN_SPEED",
		ErrHeaterDisconnected: "HEATER_DISCONNECTED",
		ErrFanDisconnected:   "FAN_DISCONNECTED",
		ErrHeaterOvercurrent: "HEATER_OVERCURRENT",
		ErrFanOvercurrent:    "FAN_OVERCURRENT",
		ErrInvalidRpm:        "INVALID_RPM",
		ErrAlreadyFiring:     "ALREADY_FIRING",
		ErrNotFiring:         "NOT_FIRING",
	})
}
