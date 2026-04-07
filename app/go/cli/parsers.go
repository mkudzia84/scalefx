package main

// ScaleFX CLI - Response Payload Parsers (shared helpers)
// Module-specific parsers are in parsers_core.go, parsers_gunfx.go,
// parsers_gearcontrol.go, parsers_lightfx.go, parsers_hubfx.go.

import (
	"fmt"
	"scalefx/protocol"
)

// gearIDName returns the human-readable name for a gear ID.
func gearIDName(id byte) string {
	names := map[byte]string{0: "Nose", 1: "Left Main", 2: "Right Main"}
	if name, ok := names[id]; ok {
		return name
	}
	return fmt.Sprintf("Gear %d", id)
}

// ParseGenericPayload prints unknown payload in a readable format.
func ParseGenericPayload(payload []byte) {
	if len(payload) == 0 {
		return
	}
	switch len(payload) {
	case 1:
		fmt.Printf("  Value: %d (0x%02X)\n", payload[0], payload[0])
	case 2:
		val := protocol.ReadU16LE(payload, 0)
		fmt.Printf("  Value (u16): %d (0x%04X)\n", val, val)
	case 4:
		val := protocol.ReadU32LE(payload, 0)
		fmt.Printf("  Value (u32): %d (0x%08X)\n", val, val)
	default:
		// Check if printable ASCII
		printable := true
		for _, b := range payload {
			if b < 0x20 || b > 0x7E {
				printable = false
				break
			}
		}
		if printable {
			fmt.Printf("  Text: \"%s\"\n", string(payload))
		} else {
			fmt.Printf("  Hex (%d bytes):", len(payload))
			for _, b := range payload {
				fmt.Printf(" %02X", b)
			}
			fmt.Println()
		}
	}
}
