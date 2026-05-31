package client

import (
	"fmt"

	"scalefx/protocol/input"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

// Input is the RC-receiver facet: it sets the broadcast cadence on an
// input port and decodes the channel-value frames the firmware streams
// (RCIN single-channel PPM, SBUS / Jeti multi-channel).  Live values flow
// through Events.OnInputValue.
type Input struct{ c *Client }

// ChannelValue is one decoded channel: its pulse width (µs) and validity.
type ChannelValue struct {
	Channel int    `json:"channel"`
	Us      uint16 `json:"us"`
	Valid   bool   `json:"valid"`
}

// InputValue is a decoded input frame for one port — one or many channels
// depending on the protocol.  GUID "" is the hub's own input port.
type InputValue struct {
	GUID     string         `json:"guid"`
	PortIdx  byte           `json:"portIdx"`
	Protocol string         `json:"protocol"`
	Channels []ChannelValue `json:"channels"`
}

// ServoMotionEvent is a decoded batched SERVO_MOTION_UPDATE frame — the live
// state of every active servo on one board.  Generic / port-keyed (Rule 42):
// any UI domain that uses a servo consumes this, independent of the effect.
// GUID "" is the hub's own servos.  Flows through Events.OnServoMotion.
type ServoMotionEvent struct {
	GUID   string             `json:"guid"`
	Servos []roles.ServoMotion `json:"servos"`
}

// decodeServoMotion turns a SERVO_MOTION_UPDATE payload (direct or topology-
// unwrapped) into a ServoMotionEvent.  Returns false if it isn't one.
func decodeServoMotion(guid string, innerType byte, p []byte) (ServoMotionEvent, bool) {
	if innerType != byte(roles.ServoMotionUpdate) {
		return ServoMotionEvent{}, false
	}
	servos, err := roles.DecodeServoMotionUpdate(p)
	if err != nil {
		return ServoMotionEvent{}, false
	}
	return ServoMotionEvent{GUID: guid, Servos: servos}, true
}

// SetBroadcastHz tells a hub-local input port to stream RC PWM values at
// `hz` (0 stops the stream).  This is what makes the live channel bars
// move — the "verbose signalling" the operator turns on while wiring.
func (in *Input) SetBroadcastHz(portIdx, hz byte) error {
	return in.c.sendExpectACK(roles.CmdRcinSetBroadcastHz(portIdx, hz))
}

// SetSbusBroadcastHz / SetJetiBroadcastHz are the multi-channel analogues
// (used once those protocols are enabled).
func (in *Input) SetSbusBroadcastHz(portIdx, hz byte) error {
	return in.c.sendExpectACK(roles.CmdSbusSetBroadcastHz(portIdx, hz))
}
func (in *Input) SetJetiBroadcastHz(portIdx, hz byte) error {
	return in.c.sendExpectACK(roles.CmdJetiExSetBroadcastHz(portIdx, hz))
}

// SetRouting toggles the master's global RC → effect routing gate.
// enabled=true (default firmware state) = RC drives effects; false =
// effects ignore RC and hold their last commanded state (the operator
// drives them from Studio).  Live channel monitors keep updating either
// way.  Protocol-agnostic — covers PPM / SBUS / Jeti EX in one switch.
func (in *Input) SetRouting(enabled bool) error {
	return in.c.sendExpectACK(input.CmdSetRouting(enabled))
}

// GetRouting reads the current routing-gate state from the hub.
func (in *Input) GetRouting() (bool, error) {
	resp, err := in.c.sendForResp(input.CmdGetRouting(), input.RoutingResp)
	if err != nil {
		return false, err
	}
	en, ok := input.DecodeRouting(resp.Payload)
	if !ok {
		return false, fmt.Errorf("routing resp: short payload (%d bytes)", len(resp.Payload))
	}
	return en, nil
}

// decodeInputValue turns an input broadcast (direct or unwrapped from a
// topology role event) into an InputValue.  `innerType` is the role
// packet type; `p` its payload.  Returns false for non-input packets.
func decodeInputValue(guid string, innerType byte, p []byte) (InputValue, bool) {
	switch innerType {
	case byte(roles.RcinValueBroadcast):
		// [portIdx:u8][us:u16LE][valid:u8] — single channel (PPM ch0).
		if len(p) < 4 {
			return InputValue{}, false
		}
		return InputValue{
			GUID:     guid,
			PortIdx:  p[0],
			Protocol: "ppm",
			Channels: []ChannelValue{{Channel: 0, Us: u16le(p[1:]), Valid: p[3] != 0}},
		}, true

	case byte(roles.PpmFrameBroadcast):
		// [portIdx:u8][count:u8][valid:u8][u16LE × count] — multi-channel
		// PPM frame (the pulse-capture role's current broadcast).  A
		// single-channel RC PWM arrives here as a 1-channel frame.
		if len(p) < 3 {
			return InputValue{}, false
		}
		count := int(p[1])
		if len(p) < 3+2*count {
			return InputValue{}, false
		}
		valid := p[2] != 0
		ch := make([]uint16, count)
		for i := 0; i < count; i++ {
			ch[i] = u16le(p[3+2*i:])
		}
		return InputValue{GUID: guid, PortIdx: p[0], Protocol: "ppm", Channels: chans(ch, valid)}, true

	case byte(roles.SbusFrameBroadcast):
		f, err := roles.DecodeSbusFrame(p)
		if err != nil {
			return InputValue{}, false
		}
		valid := f.Flags&roles.SbusFlagValid != 0 && f.Flags&roles.SbusFlagFailsafe == 0
		return InputValue{GUID: guid, PortIdx: f.Index, Protocol: "sbus", Channels: chans(f.Channels, valid)}, true

	case byte(roles.JetiExFrameBroadcast):
		// [portIdx:u8][count:u8][valid:u8][u16LE × count]
		if len(p) < 3 {
			return InputValue{}, false
		}
		count := int(p[1])
		if len(p) < 3+2*count {
			return InputValue{}, false
		}
		valid := p[2] != 0
		ch := make([]uint16, count)
		for i := 0; i < count; i++ {
			ch[i] = u16le(p[3+2*i:])
		}
		return InputValue{GUID: guid, PortIdx: p[0], Protocol: "jeti-ex", Channels: chans(ch, valid)}, true
	}
	return InputValue{}, false
}

func chans(us []uint16, valid bool) []ChannelValue {
	out := make([]ChannelValue, len(us))
	for i, v := range us {
		out[i] = ChannelValue{Channel: i, Us: v, Valid: valid}
	}
	return out
}

func u16le(b []byte) uint16 { return uint16(b[0]) | uint16(b[1])<<8 }

// compile-time: keep ports import referenced (PortKind names used by hosts).
var _ = ports.KindInput
