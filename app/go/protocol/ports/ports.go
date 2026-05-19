// Package ports mirrors serial/ports.h — the port enumeration + raw access
// protocol exposed by every ScaleFX board (hub or expander).
package ports

import (
	"encoding/binary"
	"fmt"

	"scalefx/protocol"
)

// ─── PortKind — append-only u8 enum (Rule 11) ─────────────────────────

const (
	KindUnknown  byte = 0x00
	KindServo    byte = 0x01
	KindPwm      byte = 0x02
	KindHBridge  byte = 0x03
	KindInput    byte = 0x04
	KindReserved byte = 0xFF
)

// KindName returns the canonical wire name for a port kind.
func KindName(k byte) string {
	switch k {
	case KindServo:
		return "servo"
	case KindPwm:
		return "pwm"
	case KindHBridge:
		return "hbridge"
	case KindInput:
		return "input"
	default:
		return "unknown"
	}
}

// ─── InputPort runtime mode (INPUT_MODE_RESP) ─────────────────────────

const (
	InputModeIdle    byte = 0x00
	InputModePulse   byte = 0x01
	InputModeSbus    byte = 0x02
	InputModeJetiEx  byte = 0x03
	InputModeUartRaw byte = 0x04
)

// InputModeName returns a human-readable input-port mode name.
func InputModeName(m byte) string {
	switch m {
	case InputModeIdle:
		return "idle"
	case InputModePulse:
		return "pulse"
	case InputModeSbus:
		return "sbus"
	case InputModeJetiEx:
		return "jeti-ex"
	case InputModeUartRaw:
		return "uart-raw"
	default:
		return fmt.Sprintf("0x%02X", m)
	}
}

// ─── ServoPort cap flags ──────────────────────────────────────────────

const ServoFlagEmits byte = 1 << 0

// ─── InputPort cap flags ──────────────────────────────────────────────

const (
	InputFlagPulse   byte = 1 << 0
	InputFlagSbus    byte = 1 << 1
	InputFlagJetiEx  byte = 1 << 2
	InputFlagUartRaw byte = 1 << 3
)

// InputFlagsNames returns symbolic names for the bits set in flags.
func InputFlagsNames(flags byte) []string {
	out := []string{}
	if flags&InputFlagPulse != 0 {
		out = append(out, "PULSE")
	}
	if flags&InputFlagSbus != 0 {
		out = append(out, "SBUS")
	}
	if flags&InputFlagJetiEx != 0 {
		out = append(out, "JETI_EX")
	}
	if flags&InputFlagUartRaw != 0 {
		out = append(out, "UART_RAW")
	}
	return out
}

// ─── Sense flags (PWM / HBridge ports) ────────────────────────────────

const (
	SenseFlagVoltage     byte = 1 << 0
	SenseFlagCurrent     byte = 1 << 1
	SenseFlagTemperature byte = 1 << 2
)

// SenseFlagsNames returns symbolic names for the bits set in flags.
func SenseFlagsNames(flags byte) []string {
	out := []string{}
	if flags&SenseFlagVoltage != 0 {
		out = append(out, "V")
	}
	if flags&SenseFlagCurrent != 0 {
		out = append(out, "I")
	}
	if flags&SenseFlagTemperature != 0 {
		out = append(out, "T")
	}
	return out
}

// ─── Packet types (0x10..0x3F) ────────────────────────────────────────

const (
	PortListReq        protocol.PacketType = 0x10
	PortListResp       protocol.PacketType = 0x11
	ServoPortSetUs     protocol.PacketType = 0x18
	ServoPortReadUs    protocol.PacketType = 0x19
	ServoPortReadResp  protocol.PacketType = 0x1A
	PwmPortSetDuty     protocol.PacketType = 0x20
	PwmPortSetFreq     protocol.PacketType = 0x21
	PwmPortReadSense   protocol.PacketType = 0x22
	PwmPortSenseResp   protocol.PacketType = 0x23
	HBridgeSetSigned   protocol.PacketType = 0x30
	HBridgeBrake       protocol.PacketType = 0x31
	HBridgeCoast       protocol.PacketType = 0x32
	HBridgeReadSense   protocol.PacketType = 0x33
	HBridgeSenseResp   protocol.PacketType = 0x34
	InputReadPulse     protocol.PacketType = 0x38
	InputPulseResp     protocol.PacketType = 0x39
	InputGetMode       protocol.PacketType = 0x3A
	InputModeResp      protocol.PacketType = 0x3B
)

// ─── Error codes (0x20..0x2F) ─────────────────────────────────────────

const (
	ErrPortNotFound       protocol.ErrorCode = 0x20
	ErrPortKindMismatch   protocol.ErrorCode = 0x21
	ErrPortHasRole        protocol.ErrorCode = 0x22
	ErrSenseNotAvailable  protocol.ErrorCode = 0x23
	ErrPortFreqUnsupp     protocol.ErrorCode = 0x24
)

// ─── Decoded data types ───────────────────────────────────────────────

// PortDescriptor is one (idx, flags) entry in PORT_LIST_RESP.
type PortDescriptor struct {
	Index byte
	Flags byte
}

// PortList holds the decoded PORT_LIST_RESP payload — four arrays, one
// per port kind, each entry an (index, flags) pair.
type PortList struct {
	Servos   []PortDescriptor
	Pwms     []PortDescriptor
	HBridges []PortDescriptor
	Inputs   []PortDescriptor
}

// DecodePortListPayload decodes a PORT_LIST_RESP payload (without the
// guid/name prefix that TopologyServicePolicy adds — that's the
// topology layer's responsibility).
func DecodePortListPayload(p []byte) (PortList, error) {
	var out PortList
	off := 0
	read := func(kind string) ([]PortDescriptor, error) {
		if off >= len(p) {
			return nil, fmt.Errorf("truncated %s count", kind)
		}
		n := int(p[off])
		off++
		if off+2*n > len(p) {
			return nil, fmt.Errorf("truncated %s entries (need %d)", kind, n)
		}
		entries := make([]PortDescriptor, n)
		for i := 0; i < n; i++ {
			entries[i] = PortDescriptor{Index: p[off], Flags: p[off+1]}
			off += 2
		}
		return entries, nil
	}
	var err error
	if out.Servos, err = read("servo"); err != nil {
		return out, err
	}
	if out.Pwms, err = read("pwm"); err != nil {
		return out, err
	}
	if out.HBridges, err = read("hbridge"); err != nil {
		return out, err
	}
	if out.Inputs, err = read("input"); err != nil {
		return out, err
	}
	return out, nil
}

// SenseReading is the shared shape of PWM_PORT_SENSE_RESP and
// HBRIDGE_SENSE_RESP.
type SenseReading struct {
	Index      byte
	VoltageMV  int16
	CurrentMA  int16
	TempCx10   int16
}

// DecodeSenseResp decodes [idx][v_mV:i16][i_mA:i16][t_cx10:i16].
func DecodeSenseResp(p []byte) (SenseReading, error) {
	if len(p) != 7 {
		return SenseReading{}, fmt.Errorf("sense response: expected 7 bytes, got %d", len(p))
	}
	return SenseReading{
		Index:     p[0],
		VoltageMV: int16(binary.LittleEndian.Uint16(p[1:3])),
		CurrentMA: int16(binary.LittleEndian.Uint16(p[3:5])),
		TempCx10:  int16(binary.LittleEndian.Uint16(p[5:7])),
	}, nil
}

// PulseReading is the shared shape of SERVO_PORT_READ_RESP and INPUT_PULSE_RESP.
type PulseReading struct {
	Index byte
	Us    uint16
	Valid bool
}

// DecodePulseResp decodes [idx][us:u16][valid:u8].
func DecodePulseResp(p []byte) (PulseReading, error) {
	if len(p) != 4 {
		return PulseReading{}, fmt.Errorf("pulse response: expected 4 bytes, got %d", len(p))
	}
	return PulseReading{
		Index: p[0],
		Us:    binary.LittleEndian.Uint16(p[1:3]),
		Valid: p[3] != 0,
	}, nil
}

// ─── Command builders ────────────────────────────────────────────────

func CmdPortListReq() []byte { return protocol.BuildPacket(PortListReq, nil, 0) }

func CmdServoPortSetUs(idx byte, us uint16) []byte {
	return protocol.BuildPacket(ServoPortSetUs, append([]byte{idx}, protocol.U16LE(us)...), 0)
}
func CmdServoPortReadUs(idx byte) []byte {
	return protocol.BuildPacket(ServoPortReadUs, []byte{idx}, 0)
}
func CmdPwmPortSetDuty(idx byte, duty uint16) []byte {
	return protocol.BuildPacket(PwmPortSetDuty, append([]byte{idx}, protocol.U16LE(duty)...), 0)
}
func CmdPwmPortSetFreq(idx byte, hz uint16) []byte {
	return protocol.BuildPacket(PwmPortSetFreq, append([]byte{idx}, protocol.U16LE(hz)...), 0)
}
func CmdPwmPortReadSense(idx byte) []byte {
	return protocol.BuildPacket(PwmPortReadSense, []byte{idx}, 0)
}
func CmdHBridgeSetSigned(idx byte, signed int16) []byte {
	return protocol.BuildPacket(HBridgeSetSigned,
		append([]byte{idx}, protocol.U16LE(uint16(signed))...), 0)
}
func CmdHBridgeBrake(idx byte) []byte {
	return protocol.BuildPacket(HBridgeBrake, []byte{idx}, 0)
}
func CmdHBridgeCoast(idx byte) []byte {
	return protocol.BuildPacket(HBridgeCoast, []byte{idx}, 0)
}
func CmdHBridgeReadSense(idx byte) []byte {
	return protocol.BuildPacket(HBridgeReadSense, []byte{idx}, 0)
}
func CmdInputReadPulse(idx byte) []byte {
	return protocol.BuildPacket(InputReadPulse, []byte{idx}, 0)
}
func CmdInputGetMode(idx byte) []byte {
	return protocol.BuildPacket(InputGetMode, []byte{idx}, 0)
}

// ─── Name registration ───────────────────────────────────────────────

func init() {
	protocol.RegisterPacketNames(map[protocol.PacketType]string{
		PortListReq:       "PORT_LIST_REQ",
		PortListResp:      "PORT_LIST_RESP",
		ServoPortSetUs:    "SERVO_PORT_SET_US",
		ServoPortReadUs:   "SERVO_PORT_READ_US",
		ServoPortReadResp: "SERVO_PORT_READ_RESP",
		PwmPortSetDuty:    "PWM_PORT_SET_DUTY",
		PwmPortSetFreq:    "PWM_PORT_SET_FREQ",
		PwmPortReadSense:  "PWM_PORT_READ_SENSE",
		PwmPortSenseResp:  "PWM_PORT_SENSE_RESP",
		HBridgeSetSigned:  "HBRIDGE_SET_SIGNED",
		HBridgeBrake:      "HBRIDGE_BRAKE",
		HBridgeCoast:      "HBRIDGE_COAST",
		HBridgeReadSense:  "HBRIDGE_READ_SENSE",
		HBridgeSenseResp:  "HBRIDGE_SENSE_RESP",
		InputReadPulse:    "INPUT_READ_PULSE",
		InputPulseResp:    "INPUT_PULSE_RESP",
		InputGetMode:      "INPUT_GET_MODE",
		InputModeResp:     "INPUT_MODE_RESP",
	})

	protocol.RegisterErrorNames(map[protocol.ErrorCode]string{
		ErrPortNotFound:      "PORT_NOT_FOUND",
		ErrPortKindMismatch:  "PORT_KIND_MISMATCH",
		ErrPortHasRole:       "PORT_HAS_ROLE",
		ErrSenseNotAvailable: "SENSE_NOT_AVAILABLE",
		ErrPortFreqUnsupp:    "PORT_FREQ_UNSUPPORTED",
	})
}
