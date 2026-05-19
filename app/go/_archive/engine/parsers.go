package engine

// ScaleFX Engine - Response Payload Parsers (shared helpers)

import (
	"fmt"
	"scalefx/protocol"
)

// GearIDName returns the human-readable name for a gear ID.
func GearIDName(id byte) string {
	names := map[byte]string{0: "Nose", 1: "Left Main", 2: "Right Main"}
	if name, ok := names[id]; ok {
		return name
	}
	return fmt.Sprintf("Gear %d", id)
}

// ParseGenericPayload prints unknown payload in a readable format.
func (e *Engine) ParseGenericPayload(payload []byte) {
	if len(payload) == 0 {
		return
	}
	switch len(payload) {
	case 1:
		e.Out.Printf("  Value: %d (0x%02X)\n", payload[0], payload[0])
	case 2:
		val := protocol.ReadU16LE(payload, 0)
		e.Out.Printf("  Value (u16): %d (0x%04X)\n", val, val)
	case 4:
		val := protocol.ReadU32LE(payload, 0)
		e.Out.Printf("  Value (u32): %d (0x%08X)\n", val, val)
	default:
		printable := true
		for _, b := range payload {
			if b < 0x20 || b > 0x7E {
				printable = false
				break
			}
		}
		if printable {
			e.Out.Printf("  Text: \"%s\"\n", string(payload))
		} else {
			e.Out.Printf("  Hex (%d bytes):", len(payload))
			for _, b := range payload {
				e.Out.Printf(" %02X", b)
			}
			e.Out.Println()
		}
	}
}
