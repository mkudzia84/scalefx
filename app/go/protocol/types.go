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

// ErrorNameCollision records a single error code registered under two
// different names by two different modules' init() — the silent
// "last init() wins → wrong name shown" hazard (one namespace per spec
// range; see CLAUDE.md error ranges).  Detected by CheckErrorNameCollisions,
// asserted empty by tests/host/go_unit/error_collisions_test.
type ErrorNameCollision struct {
	Code  ErrorCode
	Names []string // distinct display names registered for this code
}

// errorNameVariants tracks every DISTINCT name a code has been registered
// under (in registration order), so a collision is visible even though the
// flat errorNames map only keeps the last writer.
var errorNameVariants = map[ErrorCode][]string{}

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
		seen := false
		for _, existing := range errorNameVariants[k] {
			if existing == v {
				seen = true
				break
			}
		}
		if !seen {
			errorNameVariants[k] = append(errorNameVariants[k], v)
		}
	}
}

// CheckErrorNameCollisions returns every error code that two or more modules
// registered under DIFFERENT names — i.e. a numeric collision across the
// per-module error ranges.  Empty slice = clean.  Call after all protocol
// sub-packages' init() have run (import them first).
func CheckErrorNameCollisions() []ErrorNameCollision {
	var out []ErrorNameCollision
	for code, names := range errorNameVariants {
		if len(names) > 1 {
			out = append(out, ErrorNameCollision{Code: code, Names: names})
		}
	}
	return out
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
