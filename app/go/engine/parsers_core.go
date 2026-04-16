package engine

// ScaleFX Engine - Core Response Parsers

import (
	"fmt"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"strings"
)

// ParseLogMessage parses LOG_MESSAGE payload: [level:u8][millis:u32LE][message:str]
func (e *Engine) ParseLogMessage(payload []byte) {
	if len(payload) < 5 {
		e.Out.Printf("  Log: (incomplete)\n")
		return
	}
	level := payload[0]
	timestamp := protocol.ReadU32LE(payload, 1)
	message := string(payload[5:])

	levelName := core.DiagLevelName(level)
	colors := map[byte]Color{0: ColorReset, 1: ColorCyan, 2: ColorYellow, 3: ColorRed}
	color := colors[level]

	secs := timestamp / 1000
	ms := timestamp % 1000
	e.Out.Printf("  %s\n", e.Out.C(color, fmt.Sprintf("[%6d.%03d] %-5s %s", secs, ms, levelName, message)))
}

// ParseInitReady parses INIT_READY/IDENTIFY payload.
func ParseInitReady(payload []byte) *InitReadyInfo {
	if len(payload) < 3 {
		return nil
	}

	info := &InitReadyInfo{}
	offset := 0

	nameLen := int(payload[offset])
	offset++
	if offset+nameLen > len(payload) {
		return nil
	}
	info.Name = string(payload[offset : offset+nameLen])
	offset += nameLen

	if offset >= len(payload) {
		return nil
	}
	verLen := int(payload[offset])
	offset++
	if offset+verLen > len(payload) {
		return nil
	}
	info.Version = string(payload[offset : offset+verLen])
	offset += verLen

	if offset >= len(payload) {
		return nil
	}
	platLen := int(payload[offset])
	offset++
	if offset+platLen > len(payload) {
		return nil
	}
	info.Platform = string(payload[offset : offset+platLen])
	offset += platLen

	if offset+12 > len(payload) {
		return nil
	}
	info.CPUMHz = protocol.ReadU32LE(payload, offset)
	offset += 4
	info.FreeRAM = protocol.ReadU32LE(payload, offset)
	offset += 4
	info.Build = protocol.ReadU32LE(payload, offset)

	info.ControllerType = core.DetectControllerType(info.Name)
	return info
}

// PrintInitReadyInfo displays parsed board info.
func (e *Engine) PrintInitReadyInfo(info *InitReadyInfo) {
	e.Out.Printf("  Name:     %s\n", info.Name)
	e.Out.Printf("  Version:  %s (build %d)\n", info.Version, info.Build)
	e.Out.Printf("  Platform: %s @ %d MHz\n", info.Platform, info.CPUMHz)
	e.Out.Printf("  Free RAM: %d bytes (%.1f KB)\n", info.FreeRAM, float64(info.FreeRAM)/1024)
}

// ParseStatusPayload parses STATUS response with core header and module data.
// Core header: 22 bytes (v2) or 20 bytes (legacy).
// v2: [counter:u32][uptime:u32][freeRam:u32][lastAct:u32][keepalives:u32][boardState:u8][initFlags:u8]
func (e *Engine) ParseStatusPayload(payload []byte) {
	if len(payload) < 12 {
		e.Out.Println("  (payload too short)")
		return
	}

	counter := protocol.ReadU32LE(payload, 0)
	uptime_ms := protocol.ReadU32LE(payload, 4)
	freeRAM := protocol.ReadU32LE(payload, 8)

	var moduleData []byte
	hasV2Header := len(payload) >= 22
	hasExtended := len(payload) >= 20

	if hasV2Header {
		lastActivity := protocol.ReadU32LE(payload, 12)
		keepalives := protocol.ReadU32LE(payload, 16)
		boardState := payload[20]
		initFlags := payload[21]
		moduleData = payload[22:]

		uptimeSec := uptime_ms / 1000
		hours := uptimeSec / 3600
		minutes := (uptimeSec % 3600) / 60
		seconds := uptimeSec % 60

		e.Out.Printf("  Counter:    %d\n", counter)
		e.Out.Printf("  Uptime:     %02d:%02d:%02d (%d ms)\n", hours, minutes, seconds, uptime_ms)
		e.Out.Printf("  Free RAM:   %d bytes (%.1f KB)\n", freeRAM, float64(freeRAM)/1024)
		e.Out.Printf("  Last Act:   %d ms ago\n", lastActivity)
		e.Out.Printf("  Keepalives: %d\n", keepalives)
		e.Out.Printf("  State:      %s\n", core.BoardStateName(boardState))
		if initFlags != 0 {
			flags := []string{}
			if initFlags&core.InitFlagVerbose != 0 {
				flags = append(flags, "VERBOSE")
			}
			e.Out.Printf("  Flags:      %s\n", strings.Join(flags, " "))
		}
	} else if hasExtended {
		lastActivity := protocol.ReadU32LE(payload, 12)
		keepalives := protocol.ReadU32LE(payload, 16)
		moduleData = payload[20:]

		uptimeSec := uptime_ms / 1000
		hours := uptimeSec / 3600
		minutes := (uptimeSec % 3600) / 60
		seconds := uptimeSec % 60

		e.Out.Printf("  Counter:    %d\n", counter)
		e.Out.Printf("  Uptime:     %02d:%02d:%02d (%d ms)\n", hours, minutes, seconds, uptime_ms)
		e.Out.Printf("  Free RAM:   %d bytes (%.1f KB)\n", freeRAM, float64(freeRAM)/1024)
		e.Out.Printf("  Last Act:   %d ms ago\n", lastActivity)
		e.Out.Printf("  Keepalives: %d\n", keepalives)
	} else {
		moduleData = payload[12:]
		uptimeSec := uptime_ms / 1000
		e.Out.Printf("  Counter: %d  Uptime: %ds  RAM: %d\n", counter, uptimeSec, freeRAM)
	}

	if len(moduleData) > 0 {
		if parser, ok := e.statusParsers[e.ControllerType]; ok {
			parser(moduleData)
		} else {
			e.Out.Printf("  Module data (%d bytes):", len(moduleData))
			for _, b := range moduleData {
				e.Out.Printf(" %02X", b)
			}
			e.Out.Println()
		}
	}
}

// ParseI2CScanResult parses I2C_SCAN_RESULT payload.
func (e *Engine) ParseI2CScanResult(payload []byte) {
	if len(payload) < 2 {
		e.Out.Println("  I2C scan: (incomplete)")
		return
	}

	offset := 0
	expectedCount := int(payload[offset])
	offset++

	e.Out.Printf("  ── I2C Bus Scan ───────────────\n")
	e.Out.Printf("  Expected devices: %d\n", expectedCount)

	for i := 0; i < expectedCount; i++ {
		if offset+2 >= len(payload) {
			break
		}
		addr := payload[offset]
		found := payload[offset+1] != 0
		identified := payload[offset+2] != 0
		offset += 3

		var statusStr string
		if found && identified {
			statusStr = e.Out.C(ColorGreen, "OK") + " (found + verified)"
		} else if found {
			statusStr = e.Out.C(ColorYellow, "FOUND") + " (ACK but not verified)"
		} else {
			statusStr = e.Out.C(ColorRed, "MISSING") + " (no ACK)"
		}
		e.Out.Printf("  0x%02X: %s\n", addr, statusStr)
	}

	if offset < len(payload) {
		extraCount := int(payload[offset])
		offset++
		if extraCount > 0 {
			var addrs []string
			for j := 0; j < extraCount && offset < len(payload); j++ {
				addrs = append(addrs, fmt.Sprintf("0x%02X", payload[offset]))
				offset++
			}
			e.Out.Printf("  Other devices: %s\n", strings.Join(addrs, ", "))
		} else {
			e.Out.Printf("  Other devices: none\n")
		}
	}

	e.Out.Printf("  ────────────────────────────────\n")
}
