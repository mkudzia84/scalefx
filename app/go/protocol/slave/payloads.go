package slave

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// ── Decoded async-event payloads ─────────────────────────────────────

type ServoTargetReachedEvent struct {
	Index    uint8
	Position uint16 // microseconds
}

type ServoMotionUpdateEvent struct {
	Index    uint8
	Position uint16
	Target   uint16
	Velocity int16 // µs/sec
}

type PwmStallEvent struct {
	Index      uint8
	PeakMA     uint16
	DurationMs uint16
}

type LedProgramDoneEvent struct {
	Address uint8 // bit 7 = PWM-borrowed
	ProgID  uint8
}

// ── Decoded query responses ──────────────────────────────────────────

type ServoQueryResponse struct {
	Index    uint8
	Position uint16
	Target   uint16
	Velocity int16
	Flags    uint8
}

type PwmQueryResponse struct {
	Index      uint8
	Mode       ComponentKind
	Duty       uint16 // thousandths
	FreqHz     uint16
	VoltageMV  int32
	CurrentMA  int32
}

type PwmGetConfigResponse struct {
	Index           uint8
	Mode            ComponentKind
	FreqHz          uint16
	CfgFlags        uint8
	MaxDuty         uint16
	HwFlags         uint8
	VoltageSenseIdx uint8
	CurrentSenseIdx uint8
	PairedWith      uint8
}

type LedQueryResponse struct {
	Address    uint8
	Brightness uint8
	ProgID     uint8
	ProgState  uint8
}

type IdentResponse struct {
	BoardType uint8
	Name      string
}

// ── Slave bundled status ─────────────────────────────────────────────

type ServoStatus struct {
	PortID    uint8
	Position  uint16
	Target    uint16
	Velocity  int16
	Flags     uint8
}

type PwmStatus struct {
	PortID     uint8
	Mode       ComponentKind
	Duty       uint16
	VoltageMV  int16 // clamped from 32-bit slave-side
	CurrentMA  int16
	StallFlags uint8
	PeakMA     uint16
}

type LedStatus struct {
	PortID     uint8
	Brightness uint8
	ProgState  uint8
	ProgID     uint8
}

type SlaveStatus struct {
	BoardState  uint8
	InitMode    uint8
	UptimeMs    uint32
	FreeRamB    uint32
	Servos      []ServoStatus
	Pwms        []PwmStatus
	Leds        []LedStatus
}

// ── Payload encoders (master → slave) ────────────────────────────────

func EncodeServoSet(idx uint8, pulseUs uint16) []byte {
	return []byte{idx, byte(pulseUs), byte(pulseUs >> 8)}
}

func EncodeServoConfig(idx uint8, minUs, maxUs, centerUs, maxSpeed, accel, decel uint16) []byte {
	out := make([]byte, 13)
	out[0] = idx
	binary.LittleEndian.PutUint16(out[1:], minUs)
	binary.LittleEndian.PutUint16(out[3:], maxUs)
	binary.LittleEndian.PutUint16(out[5:], centerUs)
	binary.LittleEndian.PutUint16(out[7:], maxSpeed)
	binary.LittleEndian.PutUint16(out[9:], accel)
	binary.LittleEndian.PutUint16(out[11:], decel)
	return out
}

func EncodeServoSetMotion(idx uint8, maxSpeed, accel, decel uint16) []byte {
	out := make([]byte, 7)
	out[0] = idx
	binary.LittleEndian.PutUint16(out[1:], maxSpeed)
	binary.LittleEndian.PutUint16(out[3:], accel)
	binary.LittleEndian.PutUint16(out[5:], decel)
	return out
}

func EncodeServoApplyJerk(idx uint8, offsetUs int16, durationMs uint16) []byte {
	out := make([]byte, 5)
	out[0] = idx
	binary.LittleEndian.PutUint16(out[1:], uint16(offsetUs))
	binary.LittleEndian.PutUint16(out[3:], durationMs)
	return out
}

func EncodeServoHold(idx uint8, hold bool) []byte {
	h := uint8(0)
	if hold {
		h = 1
	}
	return []byte{idx, h}
}

func EncodeServoMotionUpdates(enable bool, rateHz uint8) []byte {
	en := uint8(0)
	if enable {
		en = 1
	}
	return []byte{en, rateHz}
}

func EncodePwmSetMode(idx uint8, mode ComponentKind) []byte {
	return []byte{idx, uint8(mode)}
}

func EncodePwmSetDuty(idx uint8, dutyThou uint16) []byte {
	return []byte{idx, byte(dutyThou), byte(dutyThou >> 8)}
}

func EncodePwmSetMotor(idx uint8, speedSigned int16) []byte {
	return []byte{idx, byte(speedSigned), byte(uint16(speedSigned) >> 8)}
}

func EncodePwmSetHeater(idx uint8, value uint16) []byte {
	return []byte{idx, byte(value), byte(value >> 8)}
}

func EncodePwmSetFreq(idx uint8, freqHz uint16) []byte {
	return []byte{idx, byte(freqHz), byte(freqHz >> 8)}
}

func EncodePwmReconfigure(idx uint8, mode ComponentKind, freqHz uint16, cfgFlags uint8, maxDuty uint16) []byte {
	out := make([]byte, 7)
	out[0] = idx
	out[1] = uint8(mode)
	binary.LittleEndian.PutUint16(out[2:], freqHz)
	out[4] = cfgFlags
	binary.LittleEndian.PutUint16(out[5:], maxDuty)
	return out
}

func EncodePwmSetStallGuard(idx uint8, thresholdMA uint16, debounceMs, flags uint8) []byte {
	out := make([]byte, 5)
	out[0] = idx
	binary.LittleEndian.PutUint16(out[1:], thresholdMA)
	out[3] = debounceMs
	out[4] = flags
	return out
}

func EncodeLedSetBrightness(addr, brightness uint8) []byte {
	return []byte{addr, brightness}
}

func EncodeLedRunProgram(addr, progID, flags uint8) []byte {
	return []byte{addr, progID, flags}
}

// EncodeLedProgramLoad packs [addr][progId][count][LedEvent×N].
// Each LedEvent is 8 bytes on the wire.
func EncodeLedProgramLoad(addr, progID uint8, events []LedEvent) []byte {
	out := make([]byte, 3+8*len(events))
	out[0] = addr
	out[1] = progID
	out[2] = uint8(len(events))
	off := 3
	for _, e := range events {
		out[off+0] = e.Type
		binary.LittleEndian.PutUint16(out[off+1:], e.P1)
		binary.LittleEndian.PutUint16(out[off+3:], e.P2)
		out[off+5] = e.P3
		out[off+6] = e.P4
		out[off+7] = e.P5
		off += 8
	}
	return out
}

// ── Payload decoders (slave → master) ────────────────────────────────

func DecodeServoTargetReached(payload []byte) (ServoTargetReachedEvent, error) {
	if len(payload) < 3 {
		return ServoTargetReachedEvent{}, errors.New("SERVO_TARGET_REACHED payload too short")
	}
	return ServoTargetReachedEvent{
		Index:    payload[0],
		Position: binary.LittleEndian.Uint16(payload[1:]),
	}, nil
}

func DecodeServoMotionUpdate(payload []byte) (ServoMotionUpdateEvent, error) {
	if len(payload) < 7 {
		return ServoMotionUpdateEvent{}, errors.New("SERVO_MOTION_UPDATE payload too short")
	}
	return ServoMotionUpdateEvent{
		Index:    payload[0],
		Position: binary.LittleEndian.Uint16(payload[1:]),
		Target:   binary.LittleEndian.Uint16(payload[3:]),
		Velocity: int16(binary.LittleEndian.Uint16(payload[5:])),
	}, nil
}

func DecodePwmStall(payload []byte) (PwmStallEvent, error) {
	if len(payload) < 5 {
		return PwmStallEvent{}, errors.New("PWM_STALL payload too short")
	}
	return PwmStallEvent{
		Index:      payload[0],
		PeakMA:     binary.LittleEndian.Uint16(payload[1:]),
		DurationMs: binary.LittleEndian.Uint16(payload[3:]),
	}, nil
}

func DecodeLedProgramDone(payload []byte) (LedProgramDoneEvent, error) {
	if len(payload) < 2 {
		return LedProgramDoneEvent{}, errors.New("LED_PROGRAM_DONE payload too short")
	}
	return LedProgramDoneEvent{Address: payload[0], ProgID: payload[1]}, nil
}

func DecodeServoQueryResp(payload []byte) (ServoQueryResponse, error) {
	if len(payload) < 8 {
		return ServoQueryResponse{}, errors.New("SERVO_QUERY_RESP payload too short")
	}
	return ServoQueryResponse{
		Index:    payload[0],
		Position: binary.LittleEndian.Uint16(payload[1:]),
		Target:   binary.LittleEndian.Uint16(payload[3:]),
		Velocity: int16(binary.LittleEndian.Uint16(payload[5:])),
		Flags:    payload[7],
	}, nil
}

func DecodePwmQueryResp(payload []byte) (PwmQueryResponse, error) {
	if len(payload) < 14 {
		return PwmQueryResponse{}, errors.New("PWM_QUERY_RESP payload too short")
	}
	return PwmQueryResponse{
		Index:     payload[0],
		Mode:      ComponentKind(payload[1]),
		Duty:      binary.LittleEndian.Uint16(payload[2:]),
		FreqHz:    binary.LittleEndian.Uint16(payload[4:]),
		VoltageMV: int32(binary.LittleEndian.Uint32(payload[6:])),
		CurrentMA: int32(binary.LittleEndian.Uint32(payload[10:])),
	}, nil
}

func DecodePwmGetConfigResp(payload []byte) (PwmGetConfigResponse, error) {
	if len(payload) < 10 {
		return PwmGetConfigResponse{}, errors.New("PWM_GET_CONFIG_RESP payload too short")
	}
	return PwmGetConfigResponse{
		Index:           payload[0],
		Mode:            ComponentKind(payload[1]),
		FreqHz:          binary.LittleEndian.Uint16(payload[2:]),
		CfgFlags:        payload[4],
		MaxDuty:         binary.LittleEndian.Uint16(payload[5:]),
		HwFlags:         payload[7],
		VoltageSenseIdx: payload[8],
		CurrentSenseIdx: payload[9],
		PairedWith:      0, // optional 11th byte; tolerant of older slaves
	}, nil
}

func DecodeLedQueryResp(payload []byte) (LedQueryResponse, error) {
	if len(payload) < 4 {
		return LedQueryResponse{}, errors.New("LED_QUERY_RESP payload too short")
	}
	return LedQueryResponse{
		Address:    payload[0],
		Brightness: payload[1],
		ProgID:     payload[2],
		ProgState:  payload[3],
	}, nil
}

// DecodeComponentList parses [count:u8][ComponentInfo×N], 4 bytes each.
func DecodeComponentList(payload []byte) ([]ComponentInfo, error) {
	if len(payload) < 1 {
		return nil, errors.New("COMPONENT_LIST_RESP payload too short")
	}
	count := int(payload[0])
	if len(payload) < 1+count*4 {
		return nil, fmt.Errorf("COMPONENT_LIST_RESP truncated (count=%d, payload=%d bytes)", count, len(payload))
	}
	out := make([]ComponentInfo, count)
	for i := 0; i < count; i++ {
		off := 1 + i*4
		out[i] = ComponentInfo{
			Index:    payload[off+0],
			Kind:     ComponentKind(payload[off+1]),
			Flags:    payload[off+2],
			Reserved: payload[off+3],
		}
	}
	return out, nil
}

// DecodeIdentResp parses [boardType:u8][len:u8][utf8 name…].
func DecodeIdentResp(payload []byte) (IdentResponse, error) {
	if len(payload) < 2 {
		return IdentResponse{}, errors.New("IDENT_GET_RESP payload too short")
	}
	l := int(payload[1])
	if len(payload) < 2+l {
		return IdentResponse{}, errors.New("IDENT_GET_RESP truncated")
	}
	return IdentResponse{BoardType: payload[0], Name: string(payload[2 : 2+l])}, nil
}

// DecodeStatus parses the SLAVE_STATUS_BROADCAST / SLAVE_STATUS_REQ
// response payload — see slave.h §"Unified status" for the layout.
func DecodeStatus(payload []byte) (SlaveStatus, error) {
	if len(payload) < 10 {
		return SlaveStatus{}, errors.New("status payload too short for header")
	}
	st := SlaveStatus{
		BoardState: payload[0],
		InitMode:   payload[1],
		UptimeMs:   binary.LittleEndian.Uint32(payload[2:]),
		FreeRamB:   binary.LittleEndian.Uint32(payload[6:]),
	}
	off := 10

	// Servos
	if off >= len(payload) {
		return st, nil
	}
	servoCount := int(payload[off])
	off++
	for i := 0; i < servoCount && off+9 <= len(payload); i++ {
		st.Servos = append(st.Servos, ServoStatus{
			PortID:   payload[off],
			Position: binary.LittleEndian.Uint16(payload[off+1:]),
			Target:   binary.LittleEndian.Uint16(payload[off+3:]),
			Velocity: int16(binary.LittleEndian.Uint16(payload[off+5:])),
			Flags:    payload[off+7],
		})
		off += 9
	}

	// PWMs
	if off >= len(payload) {
		return st, nil
	}
	pwmCount := int(payload[off])
	off++
	for i := 0; i < pwmCount && off+11 <= len(payload); i++ {
		st.Pwms = append(st.Pwms, PwmStatus{
			PortID:     payload[off],
			Mode:       ComponentKind(payload[off+1]),
			Duty:       binary.LittleEndian.Uint16(payload[off+2:]),
			VoltageMV:  int16(binary.LittleEndian.Uint16(payload[off+4:])),
			CurrentMA:  int16(binary.LittleEndian.Uint16(payload[off+6:])),
			StallFlags: payload[off+8],
			PeakMA:     binary.LittleEndian.Uint16(payload[off+9:]),
		})
		off += 11
	}

	// LEDs
	if off >= len(payload) {
		return st, nil
	}
	ledCount := int(payload[off])
	off++
	for i := 0; i < ledCount && off+4 <= len(payload); i++ {
		st.Leds = append(st.Leds, LedStatus{
			PortID:     payload[off],
			Brightness: payload[off+1],
			ProgState:  payload[off+2],
			ProgID:     payload[off+3],
		})
		off += 4
	}
	return st, nil
}
