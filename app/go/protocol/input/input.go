// Package input mirrors the HubFX input-routing control packets
// (controllers/hubfx/esp32s3/src/effects/input/input_protocol.h).
//
// These toggle the master's global RC → effect routing gate.  When
// routing is disabled the InputDispatcher stops feeding effect triggers
// (RC sticks no longer drive engine / gun / etc.; effects hold their
// last state), while the input broadcast that monitors read keeps
// flowing.  Protocol-agnostic: one flag covers PPM / SBUS / Jeti EX.
package input

import "scalefx/protocol"

// Wire packet types — twin of InputRoutingPacket in input_protocol.h.
const (
	RoutingSetEnabled protocol.PacketType = 0xE8 // [enabled:u8] → ACK
	RoutingGetReq     protocol.PacketType = 0xE9 // [] → RoutingResp
	RoutingResp       protocol.PacketType = 0xEA // [enabled:u8]
)

// CmdSetRouting builds the SET packet (enabled = RC drives effects).
func CmdSetRouting(enabled bool) []byte {
	b := byte(0)
	if enabled {
		b = 1
	}
	return protocol.BuildPacket(RoutingSetEnabled, []byte{b}, 0)
}

// CmdGetRouting builds the GET request (reply is RoutingResp).
func CmdGetRouting() []byte {
	return protocol.BuildPacket(RoutingGetReq, nil, 0)
}

// DecodeRouting reads the [enabled:u8] payload of a RoutingResp.
func DecodeRouting(payload []byte) (bool, bool) {
	if len(payload) < 1 {
		return false, false
	}
	return payload[0] != 0, true
}
