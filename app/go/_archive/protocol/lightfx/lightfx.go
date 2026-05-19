package lightfx

// LightFX Protocol — mirrors serial/lightfx/lightfx.h
// LED control, sequences, servo, and landing light commands.

import (
	"fmt"

	"scalefx/protocol"
)

// ─── Packet Types (0x40-0x5F) ───

const (
	// LED direct control
	LedSet             protocol.PacketType = 0x40
	LedOff             protocol.PacketType = 0x41

	// LED sequence control
	LedSeqClear        protocol.PacketType = 0x42
	LedSeqAdd          protocol.PacketType = 0x43
	LedSeqStart        protocol.PacketType = 0x44
	LedSeqStop         protocol.PacketType = 0x45
	LedSeqRestart      protocol.PacketType = 0x46
	LedSeqStatus       protocol.PacketType = 0x47
	LedStatus          protocol.PacketType = 0x48
	LedSeqQueue        protocol.PacketType = 0x49

	// LED master brightness and enable
	LedMasterBrightness protocol.PacketType = 0x4A
	LedReset           protocol.PacketType = 0x4B
	LedEnable          protocol.PacketType = 0x4C

	// Light program runtime control (resolved against /lightfx.yaml programs)
	LightProgramSelect protocol.PacketType = 0x4D
	LightProgramReset  protocol.PacketType = 0x4E

	// Servo control
	ServoSet           protocol.PacketType = 0x50
	ServoSettings      protocol.PacketType = 0x51

	// Landing light control
	LandingLightBind    protocol.PacketType = 0x52
	LandingLightUnbind  protocol.PacketType = 0x53
	LandingLightDeploy  protocol.PacketType = 0x54
	LandingLightRetract protocol.PacketType = 0x55
	LandingLightStatus  protocol.PacketType = 0x56

	// LightFX-specific battery safety: soft-disable LED channels on low voltage.
	BatteryAutoCutoff   protocol.PacketType = 0x5E

	// Response packets
	LedSeqStatusResp   protocol.PacketType = 0x5A
	LedStatusResp      protocol.PacketType = 0x5B
	LedSeqQueueResp    protocol.PacketType = 0x5D
)

// ─── LED Event Types ───

const (
	EvtOn      byte = 0x00
	EvtOff     byte = 0x01
	EvtFlash   byte = 0x02
	EvtFadeIn  byte = 0x03
	EvtFadeOut byte = 0x04
	EvtFading  byte = 0x05
	EvtBeacon  byte = 0x06
)

// ─── Error Codes (0x50-0x5F) ───

const (
	ErrInvalidChannel  protocol.ErrorCode = 0x50
	ErrSeqFull         protocol.ErrorCode = 0x51
	ErrInvalidEvent    protocol.ErrorCode = 0x52
	ErrInvalidParam    protocol.ErrorCode = 0x53
	ErrInvalidServo    protocol.ErrorCode = 0x54
	ErrInvalidSlot     protocol.ErrorCode = 0x55
	ErrChannelDisabled protocol.ErrorCode = 0x56
	ErrInvalidProgram  protocol.ErrorCode = 0x57
)

// ─── Commands ───

func CmdLedSet(ch, brightness byte) []byte {
	return protocol.BuildPacket(LedSet, []byte{ch, brightness}, 0)
}

func CmdLedOff(ch byte) []byte {
	return protocol.BuildPacket(LedOff, []byte{ch}, 0)
}

func CmdLedSeqClear(ch byte) []byte {
	return protocol.BuildPacket(LedSeqClear, []byte{ch}, 0)
}

func CmdLedSeqAdd(ch, eventType byte, param1, param2 uint16, param3, param4 byte, extra ...byte) []byte {
	payload := []byte{ch, eventType}
	payload = append(payload, protocol.U16LE(param1)...)
	payload = append(payload, protocol.U16LE(param2)...)
	payload = append(payload, param3, param4)
	payload = append(payload, extra...)
	return protocol.BuildPacket(LedSeqAdd, payload, 0)
}

func CmdLedSeqStart(ch byte, loopCount uint16) []byte {
	payload := []byte{ch}
	payload = append(payload, protocol.U16LE(loopCount)...)
	return protocol.BuildPacket(LedSeqStart, payload, 0)
}

func CmdLedSeqStop(ch byte) []byte {
	return protocol.BuildPacket(LedSeqStop, []byte{ch}, 0)
}

func CmdLedSeqRestart(ch byte) []byte {
	return protocol.BuildPacket(LedSeqRestart, []byte{ch}, 0)
}

func CmdLedSeqStatus(ch byte) []byte {
	return protocol.BuildPacket(LedSeqStatus, []byte{ch}, 0)
}

func CmdLedSeqQueue(ch byte) []byte {
	return protocol.BuildPacket(LedSeqQueue, []byte{ch}, 0)
}

func CmdLedStatus() []byte {
	return protocol.BuildPacket(LedStatus, nil, 0)
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

func CmdMasterBrightness(brightness byte) []byte {
	return protocol.BuildPacket(LedMasterBrightness, []byte{brightness}, 0)
}

func CmdLedReset(ch byte) []byte {
	return protocol.BuildPacket(LedReset, []byte{ch}, 0)
}

func CmdLedEnable(ch byte, enabled bool) []byte {
	e := byte(0)
	if enabled {
		e = 1
	}
	return protocol.BuildPacket(LedEnable, []byte{ch, e}, 0)
}

// CmdLandingLightBind binds a landing group to a servo + channel mask.
// servoID=0 means no servo (LED-only group).
// channelMask: bitmask, bit0=ch1 .. bit7=ch8.
func CmdLandingLightBind(slot, servoID, channelMask, brightness byte) []byte {
	payload := []byte{slot, servoID, channelMask, brightness}
	return protocol.BuildPacket(LandingLightBind, payload, 0)
}

func CmdLandingLightUnbind(slot byte) []byte {
	return protocol.BuildPacket(LandingLightUnbind, []byte{slot}, 0)
}

func CmdLandingLightDeploy(slot byte) []byte {
	return protocol.BuildPacket(LandingLightDeploy, []byte{slot}, 0)
}

func CmdLandingLightRetract(slot byte) []byte {
	return protocol.BuildPacket(LandingLightRetract, []byte{slot}, 0)
}

// CmdBatteryAutoCutoff toggles the low-voltage LED soft-disable safety reaction.
func CmdBatteryAutoCutoff(enabled bool) []byte {
	e := byte(0)
	if enabled {
		e = 1
	}
	return protocol.BuildPacket(BatteryAutoCutoff, []byte{e}, 0)
}

// CmdLightProgramSelect activates a program by 0-based index. Slave rejects
// with ErrInvalidProgram if no config is loaded or index is past programCount.
func CmdLightProgramSelect(index byte) []byte {
	return protocol.BuildPacket(LightProgramSelect, []byte{index}, 0)
}

// CmdLightProgramReset stops sequences, retracts all groups, leaves no active program.
func CmdLightProgramReset() []byte {
	return protocol.BuildPacket(LightProgramReset, nil, 0)
}

// ─── Name Lookups ───

// LandingLightPhaseName returns landing light phase name.
func LandingLightPhaseName(phase byte) string {
	names := map[byte]string{
		0: "retracted", 1: "deploying", 2: "deployed", 3: "retracting",
	}
	if n, ok := names[phase]; ok {
		return n
	}
	return fmt.Sprintf("unknown(%d)", phase)
}

// ─── Name Registration ───

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		LedSet:              "LFX.LED_SET",
		LedOff:              "LFX.LED_OFF",
		LedSeqClear:         "LFX.LED_SEQ_CLEAR",
		LedSeqAdd:           "LFX.LED_SEQ_ADD",
		LedSeqStart:         "LFX.LED_SEQ_START",
		LedSeqStop:          "LFX.LED_SEQ_STOP",
		LedSeqRestart:       "LFX.LED_SEQ_RESTART",
		LedSeqStatus:        "LFX.LED_SEQ_STATUS",
		LedStatus:           "LFX.LED_STATUS",
		LedSeqQueue:         "LFX.LED_SEQ_QUEUE",
		LedMasterBrightness: "LFX.LED_MASTER_BRIGHTNESS",
		LedReset:            "LFX.LED_RESET",
		LedEnable:           "LFX.LED_ENABLE",
		LightProgramSelect:  "LFX.LIGHT_PROGRAM_SELECT",
		LightProgramReset:   "LFX.LIGHT_PROGRAM_RESET",
		ServoSet:            "LFX.SERVO_SET",
		ServoSettings:       "LFX.SERVO_SETTINGS",
		LandingLightBind:    "LFX.LANDING_LIGHT_BIND",
		LandingLightUnbind:  "LFX.LANDING_LIGHT_UNBIND",
		LandingLightDeploy:  "LFX.LANDING_LIGHT_DEPLOY",
		LandingLightRetract: "LFX.LANDING_LIGHT_RETRACT",
		LandingLightStatus:  "LFX.LANDING_LIGHT_STATUS",
		BatteryAutoCutoff:   "LFX.BATTERY_AUTO_CUTOFF",
		LedSeqStatusResp:    "LFX.LED_SEQ_STATUS_RESP",
		LedStatusResp:       "LFX.LED_STATUS_RESP",
		LedSeqQueueResp:     "LFX.LED_SEQ_QUEUE_RESP",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrInvalidChannel:  "INVALID_CHANNEL",
		ErrSeqFull:         "SEQ_FULL",
		ErrInvalidEvent:    "INVALID_EVENT",
		ErrInvalidParam:    "LFX_INVALID_PARAM",
		ErrInvalidServo:    "INVALID_SERVO",
		ErrInvalidSlot:     "INVALID_SLOT",
		ErrChannelDisabled: "CHANNEL_DISABLED",
		ErrInvalidProgram:  "INVALID_PROGRAM",
	})
}
