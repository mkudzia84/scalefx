package main

// ScaleFX CLI - Core Response Parsers
// Handles INIT_READY, STATUS, LOG_MESSAGE, and I2C scan responses.

import (
	"fmt"
	"strings"
)

// ParseLogMessage parses LOG_MESSAGE payload: [level:u8][millis:u32LE][message:str]
func ParseLogMessage(payload []byte) {
	if len(payload) < 5 {
		fmt.Printf("  Log: (incomplete)\n")
		return
	}
	level := payload[0]
	timestamp := ReadU32LE(payload, 1)
	message := string(payload[5:])

	levelName := DiagLevelName(level)
	colors := map[byte]string{0: colorReset, 1: colorCyan, 2: colorYellow, 3: colorRed}
	color := colors[level]
	if color == "" {
		color = colorReset
	}

	secs := timestamp / 1000
	ms := timestamp % 1000
	fmt.Printf("  %s[%6d.%03d] %-5s %s%s\n", color, secs, ms, levelName, message, colorReset)
}

// InitReadyInfo holds parsed INIT_READY/IDENTIFY data.
type InitReadyInfo struct {
	Name           string
	Version        string
	Platform       string
	CPUMHz         uint32
	FreeRAM        uint32
	Build          uint32
	ControllerType string
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
	info.CPUMHz = ReadU32LE(payload, offset)
	offset += 4
	info.FreeRAM = ReadU32LE(payload, offset)
	offset += 4
	info.Build = ReadU32LE(payload, offset)

	info.ControllerType = DetectControllerType(info.Name)
	return info
}

// PrintInitReadyInfo displays parsed board info.
func PrintInitReadyInfo(info *InitReadyInfo) {
	fmt.Printf("  Name:     %s\n", info.Name)
	fmt.Printf("  Version:  %s (build %d)\n", info.Version, info.Build)
	fmt.Printf("  Platform: %s @ %d MHz\n", info.Platform, info.CPUMHz)
	fmt.Printf("  Free RAM: %d bytes (%.1f KB)\n", info.FreeRAM, float64(info.FreeRAM)/1024)
}

// ParseStatusPayload parses STATUS response with core header and module data.
func ParseStatusPayload(payload []byte, controllerType string) {
	if len(payload) < 12 {
		fmt.Println("  (payload too short)")
		return
	}

	counter := ReadU32LE(payload, 0)
	uptime_ms := ReadU32LE(payload, 4)
	freeRAM := ReadU32LE(payload, 8)

	// Extended header (20-byte format)
	var moduleData []byte
	hasExtended := len(payload) >= 20
	if hasExtended {
		lastActivity := ReadU32LE(payload, 12)
		keepalives := ReadU32LE(payload, 16)
		moduleData = payload[20:]

		uptimeSec := uptime_ms / 1000
		hours := uptimeSec / 3600
		minutes := (uptimeSec % 3600) / 60
		seconds := uptimeSec % 60

		fmt.Printf("  Counter:    %d\n", counter)
		fmt.Printf("  Uptime:     %02d:%02d:%02d (%d ms)\n", hours, minutes, seconds, uptime_ms)
		fmt.Printf("  Free RAM:   %d bytes (%.1f KB)\n", freeRAM, float64(freeRAM)/1024)
		fmt.Printf("  Last Act:   %d ms ago\n", lastActivity)
		fmt.Printf("  Keepalives: %d\n", keepalives)
	} else {
		moduleData = payload[12:]
		uptimeSec := uptime_ms / 1000
		fmt.Printf("  Counter: %d  Uptime: %ds  RAM: %d\n", counter, uptimeSec, freeRAM)
	}

	if len(moduleData) > 0 {
		switch controllerType {
		case CtrlGunFX:
			parseGunFXStatus(moduleData)
		case CtrlGearControl:
			parseGearControlStatus(moduleData)
		case CtrlLightFX:
			parseLightFXStatus(moduleData)
		case CtrlHubFX:
			parseHubFXStatus(moduleData)
		default:
			if len(moduleData) > 0 {
				fmt.Printf("  Module data (%d bytes):", len(moduleData))
				for _, b := range moduleData {
					fmt.Printf(" %02X", b)
				}
				fmt.Println()
			}
		}
	}
}

// ParseI2CScanResult parses I2C_SCAN_RESULT payload.
// Wire format: [numExpected:u8][N×(addr:u8, found:u8, identified:u8)][numExtra:u8][M×addr:u8]
func ParseI2CScanResult(payload []byte) {
	if len(payload) < 2 {
		fmt.Println("  I2C scan: (incomplete)")
		return
	}

	offset := 0
	expectedCount := int(payload[offset])
	offset++

	fmt.Printf("  ── I2C Bus Scan ───────────────\n")
	fmt.Printf("  Expected devices: %d\n", expectedCount)

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
			statusStr = colorize(colorGreen, "OK") + " (found + verified)"
		} else if found {
			statusStr = colorize(colorYellow, "FOUND") + " (ACK but not verified)"
		} else {
			statusStr = colorize(colorRed, "MISSING") + " (no ACK)"
		}
		fmt.Printf("  0x%02X: %s\n", addr, statusStr)
	}

	// Extra devices
	if offset < len(payload) {
		extraCount := int(payload[offset])
		offset++
		if extraCount > 0 {
			var addrs []string
			for j := 0; j < extraCount && offset < len(payload); j++ {
				addrs = append(addrs, fmt.Sprintf("0x%02X", payload[offset]))
				offset++
			}
			fmt.Printf("  Other devices: %s\n", strings.Join(addrs, ", "))
		} else {
			fmt.Printf("  Other devices: none\n")
		}
	}

	fmt.Printf("  ────────────────────────────────\n")
}
