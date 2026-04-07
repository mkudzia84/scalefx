package protocol

import "fmt"

// PacketType is a typed byte representing a ScaleFX packet type on the wire.
type PacketType byte

// ErrorCode is a typed byte representing a ScaleFX error code in NACK payloads.
type ErrorCode byte

// String returns the human-readable name for a packet type.
func (p PacketType) String() string { return PacketTypeName(p) }

// String returns the human-readable name for an error code.
func (e ErrorCode) String() string { return ErrorName(e) }

// ─── Name Registry ───
// Sub-packages register their names via init().

var packetNames = map[PacketType]string{}
var errorNames = map[ErrorCode]string{}

// RegisterPacketNames adds packet type display names to the global registry.
func RegisterPacketNames(m map[PacketType]string) {
	for k, v := range m {
		packetNames[k] = v
	}
}

// RegisterErrorNames adds error code display names to the global registry.
func RegisterErrorNames(m map[ErrorCode]string) {
	for k, v := range m {
		errorNames[k] = v
	}
}

// PacketTypeName returns a human-readable name for a packet type.
func PacketTypeName(p PacketType) string {
	if name, ok := packetNames[p]; ok {
		return name
	}
	return fmt.Sprintf("0x%02X", byte(p))
}

// ErrorName returns a human-readable name for an error code.
func ErrorName(e ErrorCode) string {
	if name, ok := errorNames[e]; ok {
		return name
	}
	return fmt.Sprintf("UNKNOWN(0x%02X)", byte(e))
}
