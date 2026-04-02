package main

// ScaleFX CLI - Response Payload Parsers
// Mirrors tests/cli/parsers.py — human-readable output for protocol responses.

import (
	"fmt"
	"strings"
)

// ParseGenericPayload prints unknown payload in a readable format.
func ParseGenericPayload(payload []byte) {
	if len(payload) == 0 {
		return
	}
	switch len(payload) {
	case 1:
		fmt.Printf("  Value: %d (0x%02X)\n", payload[0], payload[0])
	case 2:
		val := ReadU16LE(payload, 0)
		fmt.Printf("  Value (u16): %d (0x%04X)\n", val, val)
	case 4:
		val := ReadU32LE(payload, 0)
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

// ─── Module-Specific Status Parsers ───

func parseGunFXStatus(data []byte) {
	// Wire format (28 bytes):
	//   [flags:u8][fanSpeed:u8][fanOffMs:u16]
	//   [servo0:u16][servo1:u16][servo2:u16]
	//   [rpm:u16][shots:u32][heaterMs:u32]
	//   [heaterError:u8][fanError:u8]
	//   [heaterDuty:u8][fanDuty:u8]
	//   [batteryV_mV:u16][cellCount:u8][batteryPct:u8]
	if len(data) < 20 {
		fmt.Printf("  GunFX: (incomplete: %d bytes)\n", len(data))
		return
	}

	flags := data[0]
	firing := flags&0x01 != 0
	flashActive := flags&0x02 != 0
	flashFading := flags&0x04 != 0
	heaterOn := flags&0x08 != 0
	fanOn := flags&0x10 != 0
	fanSpindown := flags&0x20 != 0

	fanSpeed := data[1]
	fanOffMs := ReadU16LE(data, 2)
	servo0 := ReadU16LE(data, 4)
	servo1 := ReadU16LE(data, 6)
	servo2 := ReadU16LE(data, 8)
	rpm := ReadU16LE(data, 10)
	shots := ReadU32LE(data, 12)
	heaterMs := ReadU32LE(data, 16)

	// Build state flags string
	var stateParts []string
	if firing {
		stateParts = append(stateParts, colorize(colorRed, "FIRING"))
	}
	if flashActive {
		stateParts = append(stateParts, "FLASH")
	}
	if flashFading {
		stateParts = append(stateParts, "FADING")
	}
	if heaterOn {
		stateParts = append(stateParts, colorize(colorYellow, "HEATER"))
	}
	if fanOn {
		stateParts = append(stateParts, "FAN")
	}
	if fanSpindown {
		stateParts = append(stateParts, "SPINDOWN")
	}
	stateStr := "IDLE"
	if len(stateParts) > 0 {
		stateStr = strings.Join(stateParts, ", ")
	}

	fmt.Printf("  ── GunFX ──────────────────────\n")
	fmt.Printf("  State:     %s\n", stateStr)

	// Muzzle flash
	if firing {
		fmt.Printf("  Fire rate: %d RPM\n", rpm)
	}
	fmt.Printf("  Shots:     %d\n", shots)

	// Fan
	if fanOn || fanSpindown {
		fanInfo := fmt.Sprintf("speed=%d", fanSpeed)
		if fanSpindown && fanOffMs > 0 {
			fanInfo += fmt.Sprintf(", off in %dms", fanOffMs)
		}
		fmt.Printf("  Fan:       %s\n", fanInfo)
	}

	// Heater
	if heaterMs > 0 {
		heaterSec := float64(heaterMs) / 1000.0
		fmt.Printf("  Heater:    %.1fs total\n", heaterSec)
	}

	// Servos
	fmt.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	// Smoke error reasons (bytes 20-21)
	if len(data) >= 22 {
		htrErr := data[20]
		fanErr := data[21]
		if htrErr != 0 || fanErr != 0 {
			fmt.Printf("  ── Smoke Errors ──────────────\n")
			if htrErr != 0 {
				fmt.Printf("  Heater:    %s\n", colorize(colorRed, SmokeErrorReasonName(htrErr)))
			}
			if fanErr != 0 {
				fmt.Printf("  Fan:       %s\n", colorize(colorRed, SmokeErrorReasonName(fanErr)))
			}
		}
	}

	// Overcurrent throttle state (bytes 22-23)
	if len(data) >= 24 {
		htrDuty := data[22]
		fanDuty := data[23]
		if htrDuty < 255 || fanDuty < 255 {
			fmt.Printf("  ── Overcurrent Throttle ──────\n")
			if htrDuty < 255 {
				pct := int(htrDuty) * 100 / 255
				fmt.Printf("  Heater:    %s\n", colorize(colorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, htrDuty)))
			}
			if fanDuty < 255 {
				pct := int(fanDuty) * 100 / 255
				fmt.Printf("  Fan:       %s\n", colorize(colorYellow, fmt.Sprintf("throttled to %d%% (duty %d/255)", pct, fanDuty)))
			}
		}
	}

	// Battery (bytes 24-27)
	if len(data) >= 28 {
		batteryMV := ReadU16LE(data, 24)
		cellCount := data[26]
		batteryPct := data[27]

		if batteryMV > 0 {
			batteryV := float64(batteryMV) / 1000.0
			battParts := []string{fmt.Sprintf("%.2fV (%dmV)", batteryV, batteryMV)}
			if cellCount > 0 {
				battParts = append(battParts, fmt.Sprintf("%dS", cellCount))
			}
			if batteryPct > 0 {
				pctColor := colorGreen
				if batteryPct <= 10 {
					pctColor = colorRed
				} else if batteryPct <= 30 {
					pctColor = colorYellow
				}
				battParts = append(battParts, fmt.Sprintf("%s%d%%%s", pctColor, batteryPct, colorReset))
			}
			fmt.Printf("  Battery:   %s\n", strings.Join(battParts, ", "))
		} else {
			fmt.Printf("  Battery:   %s\n", colorize(colorYellow, "not detected"))
		}
	}
}

func parseGearControlStatus(data []byte) {
	fmt.Printf("  ── GearControl ────────────────\n")
	// Wire format: 3 gears × 11 bytes = 33, then 20 bytes global = 53 total
	if len(data) < 39 {
		fmt.Printf("  GearControl: (incomplete: %d bytes)\n", len(data))
		return
	}

	gearNames := []string{"Nose", "Left Main", "Right Main"}
	stateColors := map[byte]string{
		0: colorReset, 1: colorGreen, 2: colorCyan,
		3: colorYellow, 4: colorYellow, 5: colorRed,
		6: colorMagenta,
	}

	// Pre-parse global data from after the 3 gear blocks
	// Per-gear error reasons (bytes 39-41)
	errorReasons := [3]byte{0, 0, 0}
	if len(data) >= 42 {
		errorReasons = [3]byte{data[39], data[40], data[41]}
	}

	// Shunt resistance (bytes 42-43)
	shuntMohm := uint16(0)
	if len(data) >= 44 {
		shuntMohm = ReadU16LE(data, 42)
	}

	// Packed door modes per gear (bytes 44-46)
	doorModes := [3]byte{0, 0, 0}
	postDeployModes := [3]byte{0, 0, 0}
	if len(data) >= 47 {
		for i := 0; i < 3; i++ {
			packed := data[44+i]
			doorModes[i] = packed & 0x0F
			postDeployModes[i] = (packed >> 4) & 0x0F
		}
	}

	// Config flags per gear (bytes 47-49)
	configFlags := [3]byte{0, 0, 0}
	if len(data) >= 50 {
		configFlags = [3]byte{data[47], data[48], data[49]}
	}

	// Door state per gear (bytes 50-52)
	doorStates := [3]byte{0, 0, 0}
	if len(data) >= 53 {
		doorStates = [3]byte{data[50], data[51], data[52]}
	}

	// Global: yaw, led_flags, battery
	yaw := ReadU16LE(data, 33)
	ledFlags := data[35]
	batteryMV := ReadU16LE(data, 36)
	batteryFlags := data[38]

	// Per-gear display
	for i := 0; i < 3; i++ {
		offset := i * 11
		state := data[offset]
		currentMA := ReadU16LE(data, offset+1)
		door0 := ReadU16LE(data, offset+3)
		door1 := ReadU16LE(data, offset+5)
		stallMA := ReadU16LE(data, offset+7)
		shunt10uV := ReadI16LE(data, offset+9)
		shuntMV := float64(shunt10uV) * 10.0 / 1000.0

		stateName := GearStateName(state)
		sColor := stateColors[state]
		if sColor == "" {
			sColor = colorReset
		}

		// Config flags
		cflags := configFlags[i]
		enabled := cflags&0x80 != 0
		hasYaw := cflags&0x01 != 0

		// Build status tags
		var tags []string
		if !enabled {
			tags = append(tags, colorize(colorYellow, "DISABLED"))
		}
		if state == 5 && errorReasons[i] != 0 { // ERROR state
			tags = append(tags, colorize(colorRed, GearErrorReasonName(errorReasons[i])))
		}
		tagStr := ""
		if len(tags) > 0 {
			tagStr = "  [" + strings.Join(tags, ", ") + "]"
		}

		// Stall calibration
		stallStr := fmt.Sprintf("stall=%dmA", stallMA)
		if stallMA == 0 {
			stallStr = colorize(colorYellow, "uncalibrated")
		}

		// Door modes
		dModeName := strings.ToLower(DoorModeName(doorModes[i]))
		pdModeName := "skip"
		if postDeployModes[i] != 0 {
			pdModeName = strings.ToLower(DoorModeName(postDeployModes[i]))
		}

		// Line 1: state + tags
		fmt.Printf("  %10s: %s%s%s%s\n", gearNames[i], sColor, stateName, colorReset, tagStr)

		// Line 2: current readings + calibration
		fmt.Printf("             motor=%dmA  shunt=%.1fmV  %s\n", currentMA, shuntMV, stallStr)

		// Line 3: doors + config
		dState := doorStates[i]
		dStateName := DoorStateName(dState)
		dStateColors := map[byte]string{
			0: colorYellow, 1: colorCyan, 2: colorGreen, 3: colorYellow, 4: colorYellow,
		}
		dStateColor := dStateColors[dState]
		if dStateColor == "" {
			dStateColor = colorReset
		}

		yawStr := ""
		if hasYaw {
			yawStr = "  yaw"
		}

		if doorModes[i] != 0 {
			fmt.Printf("             doors=[%dµs, %dµs]  %s%s%s  pre=%s  post=%s%s\n",
				door0, door1, dStateColor, dStateName, colorReset, dModeName, pdModeName, yawStr)
		} else {
			fmt.Printf("             doors=none%s\n", yawStr)
		}
	}

	// ── Global ──
	fmt.Printf("  ── Global ─────────────────────\n")
	fmt.Printf("  Yaw:       %dµs\n", yaw)

	// Shunt resistance config
	if shuntMohm > 0 {
		shuntOhm := float64(shuntMohm) / 1000.0
		maxCurrent := 81.92 / shuntOhm // INA226 max shunt voltage
		fmt.Printf("  Shunt:     %dmΩ (%.3fΩ)  max=%.0fmA\n", shuntMohm, shuntOhm, maxCurrent)
	}

	// Battery voltage and config
	batteryEnabled := batteryFlags&0x04 != 0
	autoDeploy := batteryFlags&0x01 != 0
	lowVoltage := batteryFlags&0x02 != 0

	if !batteryEnabled {
		fmt.Printf("  Battery:   %s\n", colorize(colorYellow, "disabled"))
	} else {
		batteryV := float64(batteryMV) / 1000.0
		parts := []string{fmt.Sprintf("%.1fV (%dmV)", batteryV, batteryMV)}
		if autoDeploy {
			parts = append(parts, colorize(colorCyan, "auto-deploy"))
		}
		if lowVoltage {
			parts = append(parts, colorize(colorRed, "LOW VOLTAGE"))
		}
		fmt.Printf("  Battery:   %s\n", strings.Join(parts, ", "))
	}

	// Status LEDs
	var ledParts []string
	for gi := 0; gi < 3; gi++ {
		depBit := gi * 2
		retBit := gi*2 + 1
		depOn := ledFlags&(1<<depBit) != 0
		retOn := ledFlags&(1<<retBit) != 0
		abbr := string(gearNames[gi][0]) // N, L, R
		if depOn && retOn {
			ledParts = append(ledParts, colorize(colorYellow, abbr+":both"))
		} else if depOn {
			ledParts = append(ledParts, colorize(colorGreen, abbr+":dep"))
		} else if retOn {
			ledParts = append(ledParts, colorize(colorCyan, abbr+":ret"))
		} else {
			ledParts = append(ledParts, abbr+":off")
		}
	}
	// Indicator LEDs (bits 6-7)
	if ledFlags&(1<<6) != 0 {
		ledParts = append(ledParts, colorize(colorGreen, "CONN"))
	} else {
		ledParts = append(ledParts, "conn")
	}
	if ledFlags&(1<<7) != 0 {
		ledParts = append(ledParts, colorize(colorRed, "ERR"))
	} else {
		ledParts = append(ledParts, "err")
	}
	fmt.Printf("  LEDs:      [%s]\n", strings.Join(ledParts, ", "))
}

func parseLightFXStatus(data []byte) {
	// Wire format (24 bytes):
	//   [ledBrightness:u8×8][ledSeqFlags:u8]
	//   [servo0:u16][servo1:u16][servo2:u16]
	//   [landingLightStates:u8×3]
	//   [masterBrightness_pct:u8]
	//   [ledEnabledFlags:u8]
	//   [batteryV_mV:u16LE][cellCount:u8][batteryPct:u8]
	if len(data) < 15 {
		fmt.Printf("  LightFX: (incomplete: %d bytes)\n", len(data))
		return
	}

	// LED channels
	ledBrightness := make([]byte, 8)
	copy(ledBrightness, data[:8])
	seqFlags := data[8]

	// Servos
	servo0 := ReadU16LE(data, 9)
	servo1 := ReadU16LE(data, 11)
	servo2 := ReadU16LE(data, 13)

	// Landing light states (optional)
	llPhaseNames := map[byte]string{0: "RET", 1: "DEPLOYING", 2: "DEP", 3: "RETRACTING"}
	var llStates []string
	if len(data) >= 18 {
		for i := 0; i < 3; i++ {
			phase := data[15+i]
			name := llPhaseNames[phase]
			if name == "" {
				name = fmt.Sprintf("?(%d)", phase)
			}
			llStates = append(llStates, name)
		}
	}

	// Master brightness (optional)
	masterBrightness := byte(100)
	if len(data) >= 19 {
		masterBrightness = data[18]
	}

	// Enabled flags (optional)
	enabledFlags := byte(0xFF)
	if len(data) >= 20 {
		enabledFlags = data[19]
	}

	fmt.Printf("  ── LightFX ────────────────────\n")

	// LED status (compact format with enabled/disabled indicators)
	var ledParts []string
	for i := 0; i < 8; i++ {
		ch := i + 1
		bri := ledBrightness[i]
		seq := seqFlags&(1<<i) != 0
		enabled := enabledFlags&(1<<i) != 0
		if !enabled {
			ledParts = append(ledParts, fmt.Sprintf("ch%d=%d[DIS]", ch, bri))
		} else if bri > 0 || seq {
			seqMark := ""
			if seq {
				seqMark = "▶"
			}
			ledParts = append(ledParts, fmt.Sprintf("ch%d=%d%s", ch, bri, seqMark))
		}
	}
	if len(ledParts) > 0 {
		fmt.Printf("  LEDs:      %s\n", strings.Join(ledParts, ", "))
	} else {
		fmt.Printf("  LEDs:      all off\n")
	}

	// Master brightness (only show if not 100%)
	if masterBrightness < 100 {
		fmt.Printf("  Master:    %d%%\n", masterBrightness)
	}

	// Servos
	fmt.Printf("  Servos:    [%dµs, %dµs, %dµs]\n", servo0, servo1, servo2)

	// Landing lights
	if len(llStates) > 0 {
		var llParts []string
		for i, s := range llStates {
			llParts = append(llParts, fmt.Sprintf("slot%d=%s", i+1, s))
		}
		fmt.Printf("  Lights:    %s\n", strings.Join(llParts, ", "))
	}

	// Battery (optional, bytes 20-23)
	if len(data) >= 24 {
		batMV := ReadU16LE(data, 20)
		cellCount := data[22]
		batPct := data[23]
		batV := float64(batMV) / 1000.0
		fmt.Printf("  Battery:   %.2fV (%d%%, %dS)\n", batV, batPct, cellCount)
	}
}

func parseHubFXStatus(data []byte) {
	// Wire format: [flags:u8][slaveMask:u8][loop1Count:u32LE] = 6 bytes
	// Flags bits: 0=core1Ready, 1=audioInit, 2=flashReady, 3=usbHostReady, 4=sdCardReady
	if len(data) < 2 {
		fmt.Printf("  Hub data: %d bytes\n", len(data))
		return
	}

	flags := data[0]
	slaveMask := data[1]
	loop1Count := uint32(0)
	if len(data) >= 6 {
		loop1Count = ReadU32LE(data, 2)
	}

	core1Ready := flags&0x01 != 0
	audioInit := flags&0x02 != 0
	flashReady := flags&0x04 != 0
	usbReady := flags&0x08 != 0
	sdReady := flags&0x10 != 0

	fmt.Printf("\n  %s━━━ HubFX Status ━━━%s\n", colorCyan, colorReset)

	// Core 1
	c1Color := colorRed
	c1Text := "NOT READY"
	if core1Ready {
		c1Color = colorGreen
		c1Text = "Ready"
	}
	fmt.Printf("  Core 1:    %s%s%s\n", c1Color, c1Text, colorReset)
	if len(data) >= 6 {
		fmt.Printf("             %d iterations\n", loop1Count)
	}

	// Audio
	audioColor := colorYellow
	audioText := "Not initialized"
	if audioInit {
		audioColor = colorGreen
		audioText = "Initialized"
	}
	fmt.Printf("  Audio:     %s%s%s\n", audioColor, audioText, colorReset)

	// Flash
	flashColor := colorYellow
	flashText := "Not available"
	if flashReady {
		flashColor = colorGreen
		flashText = "Ready"
	}
	fmt.Printf("  Flash:     %s%s%s\n", flashColor, flashText, colorReset)

	// SD Card
	sdColor := colorYellow
	sdText := "Not available"
	if sdReady {
		sdColor = colorGreen
		sdText = "Ready"
	}
	fmt.Printf("  SD Card:   %s%s%s\n", sdColor, sdText, colorReset)

	// USB Host
	usbColor := colorYellow
	usbText := "Not active"
	if usbReady {
		usbColor = colorGreen
		usbText = "Active"
	}
	fmt.Printf("  USB Host:  %s%s%s\n", usbColor, usbText, colorReset)

	// Slaves
	slaveNames := map[int]string{0: "GunFX", 1: "LightFX", 2: "GearControl"}
	hasSlaves := false
	for bit := range slaveNames {
		if slaveMask&(1<<bit) != 0 {
			hasSlaves = true
			break
		}
	}
	if hasSlaves {
		fmt.Printf("  Slaves:\n")
		for bit := 0; bit <= 2; bit++ {
			name := slaveNames[bit]
			isReady := slaveMask&(1<<bit) != 0
			color := colorRed
			status := "not connected"
			if isReady {
				color = colorGreen
				status = "connected"
			}
			fmt.Printf("    %s: %s%s%s\n", name, color, status, colorReset)
		}
	} else {
		fmt.Printf("  Slaves:    %s\n", colorize(colorYellow, "None connected"))
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

// ParseGearCalibStatus parses GEAR_CALIB_STATUS async payload.
// Wire format (10 bytes): [gear_id:u8][phase:u8][current_mA:u16][peak_mA:u16][stall_mA:u16][finished:u8][errorReason:u8]
func ParseGearCalibStatus(payload []byte) {
	if len(payload) < 9 {
		if len(payload) > 0 {
			fmt.Printf("  CalibStatus: (incomplete: %d bytes)\n", len(payload))
		}
		return
	}
	gearID := payload[0]
	phase := payload[1]
	currentMA := ReadU16LE(payload, 2)
	peakMA := ReadU16LE(payload, 4)
	stallMA := ReadU16LE(payload, 6)
	finished := payload[8] != 0
	errorReason := byte(0)
	if len(payload) >= 10 {
		errorReason = payload[9]
	}

	gearNames := map[byte]string{0: "Nose", 1: "Left Main", 2: "Right Main"}
	gearName := gearNames[gearID]
	if gearName == "" {
		gearName = fmt.Sprintf("Gear %d", gearID)
	}

	phaseNames := map[byte]string{
		0: "IDLE", 1: "CLEAR_RUN", 2: "CLEAR_SETTLE", 3: "DEPLOY_RUN",
		4: "MID_SETTLE", 5: "RETRACT_RUN", 6: "COMPLETE", 7: "ERROR",
		8: "CANCELLED", 9: "OPENING_DOORS", 10: "CLOSING_DOORS",
	}
	phaseName := phaseNames[phase]
	if phaseName == "" {
		phaseName = fmt.Sprintf("?(%d)", phase)
	}

	// Color based on phase
	phaseColor := colorReset
	switch phase {
	case 6: // COMPLETE
		phaseColor = colorGreen
	case 7: // ERROR
		phaseColor = colorRed
	case 8: // CANCELLED
		phaseColor = colorYellow
	case 1, 3, 5: // Motor running phases
		phaseColor = colorCyan
	}

	var parts []string
	parts = append(parts, fmt.Sprintf("%s%s%s", phaseColor, phaseName, colorReset))
	parts = append(parts, fmt.Sprintf("current=%dmA", currentMA))
	if peakMA > 0 {
		parts = append(parts, fmt.Sprintf("peak=%dmA", peakMA))
	}
	if stallMA > 0 {
		parts = append(parts, fmt.Sprintf("stall=%dmA", stallMA))
	}
	if finished {
		parts = append(parts, colorize(colorWhite, "[FINISHED]"))
	}
	if phase == 7 && errorReason > 0 {
		parts = append(parts, colorize(colorRed, "reason="+GearErrorReasonName(errorReason)))
	}

	fmt.Printf("  %s◆%s %s calib: %s\n", colorMagenta, colorReset, gearName, strings.Join(parts, ", "))
}

// ParseGearSeqStatus parses GEAR_SEQ_STATUS async payload.
// Wire format (8 bytes): [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]
func ParseGearSeqStatus(payload []byte) {
	if len(payload) < 8 {
		if len(payload) > 0 {
			fmt.Printf("  SeqStatus: (incomplete: %d bytes)\n", len(payload))
		}
		return
	}
	gearID := payload[0]
	phase := payload[1]
	deploying := payload[2] != 0
	finished := payload[3] != 0
	elapsedMs := ReadU32LE(payload, 4)

	gearNames := map[byte]string{0: "Nose", 1: "Left Main", 2: "Right Main"}
	gearName := gearNames[gearID]
	if gearName == "" {
		gearName = fmt.Sprintf("Gear %d", gearID)
	}

	phaseName := GearSeqPhaseName(phase)
	action := "retract"
	if deploying {
		action = "deploy"
	}

	// Color based on state
	phaseColor := colorYellow
	switch {
	case finished && phase != 4: // COMPLETE (not error)
		phaseColor = colorGreen
	case phase == 4: // SEQ_ERROR
		phaseColor = colorRed
	case phase == 2: // RUNNING_MOTOR
		phaseColor = colorCyan
	case phase == 5: // SYNC_WAIT
		phaseColor = colorMagenta
	}

	elapsedSec := float64(elapsedMs) / 1000.0

	var parts []string
	parts = append(parts, fmt.Sprintf("%s%s%s", phaseColor, phaseName, colorReset))
	parts = append(parts, action)
	parts = append(parts, fmt.Sprintf("%.1fs", elapsedSec))
	if finished {
		parts = append(parts, colorize(colorWhite, fmt.Sprintf("[FINISHED in %.1fs]", elapsedSec)))
	}

	fmt.Printf("  %s▸%s %s seq: %s\n", colorMagenta, colorReset, gearName, strings.Join(parts, ", "))
}

// ParseGearDoorStatus parses GEAR_DOOR_STATUS async payload.
// Wire format (6 bytes): [gear_id:u8][state:u8][door0_pos_us:u16LE][door1_pos_us:u16LE]
func ParseGearDoorStatus(payload []byte) {
	if len(payload) < 2 {
		if len(payload) > 0 {
			fmt.Printf("  DoorStatus: (incomplete: %d bytes)\n", len(payload))
		}
		return
	}
	gearID := payload[0]
	state := payload[1]

	gearNames := map[byte]string{0: "Nose", 1: "Left Main", 2: "Right Main"}
	gearName := gearNames[gearID]
	if gearName == "" {
		gearName = fmt.Sprintf("Gear %d", gearID)
	}

	stateName := DoorStateName(state)
	stateColors := map[byte]string{
		0: colorYellow, 1: colorCyan, 2: colorGreen, 3: colorYellow, 4: colorYellow,
	}
	stateColor := stateColors[state]
	if stateColor == "" {
		stateColor = colorReset
	}

	var parts []string
	parts = append(parts, fmt.Sprintf("%s%s%s", stateColor, stateName, colorReset))
	if len(payload) >= 4 {
		parts = append(parts, fmt.Sprintf("d0=%dµs", ReadU16LE(payload, 2)))
	}
	if len(payload) >= 6 {
		parts = append(parts, fmt.Sprintf("d1=%dµs", ReadU16LE(payload, 4)))
	}

	fmt.Printf("  %s◇%s %s doors: %s\n", colorMagenta, colorReset, gearName, strings.Join(parts, ", "))
}

// ParseLandingLightStatus parses LANDING_LIGHT_STATUS async payload.
// Wire format (3 bytes): [slot:u8][phase:u8][finished:u8]
func ParseLandingLightStatus(payload []byte) {
	if len(payload) < 3 {
		if len(payload) > 0 {
			fmt.Printf("  LandingLightStatus: (incomplete: %d bytes)\n", len(payload))
		}
		return
	}
	slot := payload[0]
	phase := payload[1]
	finished := payload[2] != 0

	phaseName := LandingLightPhaseName(phase)

	// Color based on state
	phaseColor := colorCyan
	switch phase {
	case 2: // DEPLOYED
		phaseColor = colorGreen
	case 0: // RETRACTED
		phaseColor = colorYellow
	}

	var parts []string
	parts = append(parts, fmt.Sprintf("%s%s%s", phaseColor, phaseName, colorReset))
	if finished {
		parts = append(parts, colorize(colorWhite, "[FINISHED]"))
	}

	fmt.Printf("  %s▸%s Landing light %d: %s\n", colorBlue, colorReset, slot, strings.Join(parts, ", "))
}

// ─── LightFX Response Parsers ───

// LedSeqEventName returns event type name.
func LedSeqEventName(etype byte) string {
	names := []string{"ON", "OFF", "FLASH", "FADE_IN", "FADE_OUT", "FADING", "BEACON"}
	if int(etype) < len(names) {
		return names[etype]
	}
	return fmt.Sprintf("UNKNOWN(0x%02X)", etype)
}

// ParseLedStatus parses LED_STATUS_RESP payload: [ch:u8][brightness:u8][seq_playing:u8][seq_count:u8] per channel.
func ParseLedStatus(payload []byte) {
	if len(payload) < 4 {
		fmt.Println("  (empty LED status)")
		return
	}
	fmt.Printf("  ── LED Channel Status ──\n")
	for i := 0; i+4 <= len(payload); i += 4 {
		ch := payload[i]
		brightness := payload[i+1]
		seqPlaying := payload[i+2] != 0
		seqCount := payload[i+3]

		filled := int(brightness) * 8 / 100
		if brightness > 0 && filled == 0 {
			filled = 1
		}
		bar := ""
		for j := 0; j < 8; j++ {
			if j < filled {
				bar += "█"
			} else {
				bar += "░"
			}
		}
		seqIcon := "■"
		if seqPlaying {
			seqIcon = "▶"
		}
		fmt.Printf("  CH%d: %s %3d%% | Seq: %s (%d events)\n", ch, bar, brightness, seqIcon, seqCount)
	}
}

// ParseLedSeqStatus parses LED_SEQ_STATUS_RESP: [ch:u8][playing:u8][count:u8][index:u8][loops:u32LE][brightness:u8].
func ParseLedSeqStatus(payload []byte) {
	if len(payload) < 8 {
		fmt.Println("  (invalid sequence status)")
		return
	}
	ch := payload[0]
	playing := payload[1] != 0
	count := payload[2]
	index := payload[3]
	loops := ReadU32LE(payload, 4)
	brightness := byte(0)
	if len(payload) >= 9 {
		brightness = payload[8]
	}

	status := "STOPPED"
	statusColor := colorYellow
	if playing {
		status = "PLAYING"
		statusColor = colorGreen
	}

	fmt.Printf("  ── LED %d Sequence Status ──\n", ch)
	fmt.Printf("  Status:      %s%s%s\n", statusColor, status, colorReset)
	fmt.Printf("  Events:      %d\n", count)
	fmt.Printf("  Current:     %d\n", index)
	fmt.Printf("  Loop Count:  %d\n", loops)
	fmt.Printf("  Brightness:  %d%%\n", brightness)
}

// ParseLedSeqQueue parses LED_SEQ_QUEUE_RESP: [ch:u8][count:u8][index:u8][playing:u8][brightness:u8] + events.
func ParseLedSeqQueue(payload []byte) {
	if len(payload) < 5 {
		fmt.Println("  (invalid sequence queue)")
		return
	}
	ch := payload[0]
	count := payload[1]
	index := payload[2]
	playing := payload[3] != 0
	brightness := payload[4]

	status := "STOPPED"
	if playing {
		status = "PLAYING"
	}
	fmt.Printf("  ── LED %d Sequence Queue (%s, %d events, brightness %d%%) ──\n",
		ch, status, count, brightness)

	if count == 0 {
		fmt.Println("  (empty)")
		return
	}

	for i := 0; i < int(count); i++ {
		offset := 5 + (i * 4)
		if offset+4 > len(payload) {
			break
		}
		etype := payload[offset]
		duration := ReadU16LE(payload, offset+1)
		param1 := payload[offset+3]
		marker := ""
		if byte(i) == index {
			marker = " ← current"
		}
		fmt.Printf("  [%d] %-8s: %dms (param=%d)%s\n", i, LedSeqEventName(etype), duration, param1, marker)
	}
}

// ─── HubFX Response Parsers ───

// ParseSlaveList parses SLAVE_LIST_RESP: [count:u8] + per-entry [type:u8][connected:u8][ready:u8][nameLen:u8][name...].
func ParseSlaveList(payload []byte) {
	if len(payload) < 1 {
		fmt.Println("  (empty slave list)")
		return
	}
	count := payload[0]
	fmt.Printf("  ── Slave Controllers (%d) ──\n", count)
	if count == 0 {
		fmt.Println("  (no slaves registered)")
		return
	}

	pos := 1
	for i := 0; i < int(count); i++ {
		if pos+4 > len(payload) {
			break
		}
		stype := payload[pos]
		connected := payload[pos+1]
		ready := payload[pos+2]
		nameLen := int(payload[pos+3])
		pos += 4

		name := ""
		if nameLen > 0 && pos+nameLen <= len(payload) {
			name = string(payload[pos : pos+nameLen])
			pos += nameLen
		}

		typeName := SlaveTypeName(stype)
		statusColor := colorRed
		statusText := "disconnected"
		if ready != 0 {
			statusColor = colorGreen
			statusText = "ready"
		} else if connected != 0 {
			statusColor = colorYellow
			statusText = "connected"
		}
		displayName := ""
		if name != "" {
			displayName = fmt.Sprintf(" (%s)", name)
		}
		fmt.Printf("  [%d] %s%s: %s%s%s\n", i, typeName, displayName, statusColor, statusText, colorReset)
	}
}

// ParseAudioStatus parses AUDIO_STATUS_RESP (v3/v4 extended format).
func ParseAudioStatus(payload []byte) {
	if len(payload) < 7 {
		fmt.Println("  (audio status too short)")
		return
	}

	pos := 0
	masterVol := payload[pos]; pos++
	flags := payload[pos]; pos++
	initialized := flags&0x01 != 0
	i2sRunning := flags&0x02 != 0
	hasCodec := flags&0x04 != 0
	hasRingStats := flags&0x08 != 0
	hasBufferCaps := flags&0x10 != 0

	sampleRate := ReadU16LE(payload, pos); pos += 2
	bitDepth := payload[pos]; pos++
	maxChannels := payload[pos]; pos++
	codecNameLen := int(payload[pos]); pos++
	codecName := ""
	if codecNameLen > 0 && pos+codecNameLen <= len(payload) {
		codecName = string(payload[pos : pos+codecNameLen])
	}
	pos += codecNameLen

	// Ring buffer stats (v3)
	var ringFillPct byte
	var underruns, consumeLoops, consumeFrames uint32
	var ringAvailRead, ringAvailWrite uint16
	if hasRingStats && pos+9 <= len(payload) {
		ringFillPct = payload[pos]; pos++
		ringAvailRead = ReadU16LE(payload, pos); pos += 2
		ringAvailWrite = ReadU16LE(payload, pos); pos += 2
		underruns = ReadU32LE(payload, pos); pos += 4
		if pos+8 <= len(payload) {
			consumeLoops = ReadU32LE(payload, pos); pos += 4
			consumeFrames = ReadU32LE(payload, pos); pos += 4
		}
	}

	// Buffer capacities (v4)
	var wavBufCapacity, ringCapacity uint16
	if hasBufferCaps && pos+4 <= len(payload) {
		wavBufCapacity = ReadU16LE(payload, pos); pos += 2
		ringCapacity = ReadU16LE(payload, pos); pos += 2
	}

	// Display
	fmt.Printf("\n  %sAudio Mixer Status%s\n", colorCyan, colorReset)
	initStr := colorize(colorRed, "no")
	if initialized {
		initStr = colorize(colorGreen, "yes")
	}
	i2sStr := colorize(colorRed, "stopped")
	if i2sRunning {
		i2sStr = colorize(colorGreen, "running")
	}
	fmt.Printf("    Initialized: %s\n", initStr)
	fmt.Printf("    I2S:         %s (%dHz / %dbit)\n", i2sStr, sampleRate, bitDepth)
	if hasCodec {
		fmt.Printf("    Codec:       %s\n", codecName)
	} else {
		fmt.Printf("    Codec:       %snone (I2S only)%s\n", colorYellow, colorReset)
	}
	fmt.Printf("    Max Ch:      %d\n", maxChannels)
	fmt.Printf("    Master Vol:  %d%%\n", masterVol)

	if hasRingStats {
		underrunStr := colorize(colorGreen, "0")
		if underruns > 0 {
			underrunStr = colorize(colorRed, fmt.Sprintf("%d", underruns))
		}
		fillColor := colorRed
		if ringFillPct >= 50 {
			fillColor = colorGreen
		} else if ringFillPct >= 25 {
			fillColor = colorYellow
		}
		ringTotal := uint16(ringAvailRead) + uint16(ringAvailWrite)
		ringCapStr := fmt.Sprintf("/%d", ringTotal)
		if ringCapacity > 0 {
			ringCapStr = fmt.Sprintf("/%d", ringCapacity)
		}
		ringMs := ""
		if sampleRate > 0 && ringAvailRead > 0 {
			ringMs = fmt.Sprintf(" (%dms)", uint32(ringAvailRead)*1000/uint32(sampleRate))
		}
		fmt.Printf("    Ring Buf:    %s%d%%%s (%d%s frames%s)\n", fillColor, ringFillPct, colorReset, ringAvailRead, ringCapStr, ringMs)
		fmt.Printf("    Underruns:   %s\n", underrunStr)
		fmt.Printf("    Consumer:    %d loops, %d frames written to I2S\n", consumeLoops, consumeFrames)
	}
	if hasBufferCaps && wavBufCapacity > 0 {
		wavMs := uint32(0)
		if sampleRate > 0 {
			wavMs = uint32(wavBufCapacity) * 1000 / uint32(sampleRate)
		}
		fmt.Printf("    WAV Buf:     %d frames/ch (%dms, %dKB total)\n", wavBufCapacity, wavMs, uint32(wavBufCapacity)*8*2*4/1024)
	}

	if pos >= len(payload) {
		fmt.Printf("    %sNo channel data%s\n", colorYellow, colorReset)
		return
	}

	activeMask := payload[pos]; pos++
	if activeMask == 0 {
		fmt.Printf("    %sNo active channels%s\n", colorYellow, colorReset)
		return
	}

	activeCount := 0
	for b := activeMask; b != 0; b >>= 1 {
		activeCount += int(b & 1)
	}
	fmt.Printf("    Active:      %d channel(s) (mask: 0b%08b)\n", activeCount, activeMask)
	fmt.Println()

	outputNames := map[byte]string{AudioOutputCH1: "ch1", AudioOutputCH2: "ch2", AudioOutputALL: "all"}

	for i := 0; i < activeCount; i++ {
		if pos+16 > len(payload) {
			break
		}
		ch := payload[pos]; pos++
		vol := payload[pos]; pos++
		playing := payload[pos] != 0; pos++
		looping := payload[pos] != 0; pos++
		loopCount := ReadU16LE(payload, pos); pos += 2
		remaining_ms := ReadU32LE(payload, pos); pos += 4
		queueLen := payload[pos]; pos++
		output := payload[pos]; pos++

		wavRate := ReadU16LE(payload, pos); pos += 2
		wavCh := payload[pos]; pos++
		wavBits := payload[pos]; pos++

		wavBufFill := byte(0)
		if hasBufferCaps && pos < len(payload) {
			wavBufFill = payload[pos]; pos++
		}

		fnameLen := int(payload[pos]); pos++
		fname := ""
		if fnameLen > 0 && pos+fnameLen <= len(payload) {
			fname = string(payload[pos : pos+fnameLen])
		}
		pos += fnameLen

		status := colorize(colorYellow, "- queued")
		if playing {
			status = colorize(colorGreen, "> playing")
		}
		outName := outputNames[output]
		if outName == "" {
			outName = fmt.Sprintf("out%d", output)
		}
		loopStr := ""
		if looping {
			if loopCount == 0xFFFF {
				loopStr = " loop=inf"
			} else {
				loopStr = fmt.Sprintf(" loop=x%d", loopCount)
			}
		}
		remainingStr := ""
		if remaining_ms > 0 {
			remS := remaining_ms / 1000
			remFrac := remaining_ms % 1000
			remainingStr = fmt.Sprintf(" %d.%03ds left", remS, remFrac)
		}
		queueStr := ""
		if queueLen > 0 {
			queueStr = fmt.Sprintf(" [queue: %d]", queueLen)
		}
		bufStr := ""
		if hasBufferCaps && playing {
			bufColor := colorRed
			if wavBufFill >= 80 {
				bufColor = colorGreen
			} else if wavBufFill >= 40 {
				bufColor = colorYellow
			}
			bufStr = fmt.Sprintf(" buf=%s%d%%%s", bufColor, wavBufFill, colorReset)
		}

		fmt.Printf("    ch%d: %s vol=%d%% %s%s%s%s%s\n", ch, status, vol, outName, loopStr, remainingStr, queueStr, bufStr)
		if fname != "" {
			fmt.Printf("          file: %s\n", fname)
		}
		wavStr := ""
		if wavRate > 0 {
			chStr := "mono"
			if wavCh == 2 {
				chStr = "stereo"
			}
			wavStr = fmt.Sprintf("%dHz/%dbit/%s", wavRate, wavBits, chStr)
			fmt.Printf("          wav:  %s\n", wavStr)
		}
	}
	fmt.Println()
}

// ParseEngineStatus parses ENGINE_STATUS_RESP: [state:u8][toggle:u8][active:u8].
func ParseEngineStatus(payload []byte) {
	if len(payload) < 3 {
		fmt.Println("  (engine status too short)")
		return
	}
	state := payload[0]
	toggle := payload[1] != 0
	active := payload[2] != 0

	stateName := EngineStateName(state)
	stateColor := colorRed
	icon := "-"
	switch state {
	case 0:
		stateColor = colorRed; icon = "-" // Stopped
	case 1:
		stateColor = colorYellow; icon = "*" // Starting
	case 2:
		stateColor = colorGreen; icon = ">" // Running
	case 3:
		stateColor = colorYellow; icon = "~" // Stopping
	}

	fmt.Printf("\n  %sEngine FX Status%s\n", colorCyan, colorReset)
	fmt.Printf("    State:    %s %s%s%s\n", icon, stateColor, stateName, colorReset)
	fmt.Printf("    Toggle:   %s\n", map[bool]string{true: "engaged", false: "disengaged"}[toggle])
	fmt.Printf("    Active:   %s\n", map[bool]string{true: "yes", false: "no"}[active])
	fmt.Println()
}

// ParseConfigStatus parses CONFIG_STATUS_RESP: [loaded:u8][size:u16LE][valid:u8].
func ParseConfigStatus(payload []byte) {
	if len(payload) < 4 {
		fmt.Println("  (config status too short)")
		return
	}
	loaded := payload[0] != 0
	size := ReadU16LE(payload, 1)
	valid := payload[3] != 0

	fmt.Printf("\n  %sConfig Status%s\n", colorCyan, colorReset)
	statusStr := colorize(colorRed, "not loaded")
	if loaded {
		statusStr = colorize(colorGreen, "loaded")
	}
	fmt.Printf("    Status:     %s\n", statusStr)
	fmt.Printf("    Size:       %d bytes\n", size)
	if loaded {
		validStr := colorize(colorYellow, "invalid")
		if valid {
			validStr = colorize(colorGreen, "valid")
		}
		fmt.Printf("    Validation: %s\n", validStr)
	}
	fmt.Println()
}

// ParseSdStatus parses SD_STATUS_RESP: [init:u8][cardSize:u32][total:u32][free:u32][fatType:u8][cardType:u8][busMode:u8][used:u32].
func ParseSdStatus(payload []byte) {
	if len(payload) < 1 {
		fmt.Println("  (SD status too short)")
		return
	}
	initialized := payload[0] != 0

	cardTypes := map[byte]string{0: "NONE", 1: "MMC", 2: "SD", 3: "SDHC", 4: "UNKNOWN"}
	busModes := map[byte]string{0: "SPI", 1: "SDIO 1-bit", 2: "SDIO 4-bit"}

	fmt.Printf("\n  %sSD Card Status%s\n", colorCyan, colorReset)
	if initialized {
		fmt.Printf("    Status: %s\n", colorize(colorGreen, "initialized"))
		if len(payload) >= 14 {
			cardSize := ReadU32LE(payload, 1)
			totalSpace := ReadU32LE(payload, 5)
			freeSpace := ReadU32LE(payload, 9)
			fatType := payload[13]
			fmt.Printf("    Card:   %d MB\n", cardSize)
			fmt.Printf("    Total:  %d MB\n", totalSpace)
			fmt.Printf("    Free:   %d MB\n", freeSpace)
			if fatType > 0 {
				fmt.Printf("    FAT:    FAT%d\n", fatType)
			}
		}
		if len(payload) >= 20 {
			cardType := payload[14]
			busMode := payload[15]
			usedSpace := ReadU32LE(payload, 16)
			typeName := cardTypes[cardType]
			if typeName == "" {
				typeName = fmt.Sprintf("0x%02X", cardType)
			}
			busName := busModes[busMode]
			if busName == "" {
				busName = fmt.Sprintf("0x%02X", busMode)
			}
			fmt.Printf("    Type:   %s\n", typeName)
			fmt.Printf("    Bus:    %s\n", busName)
			fmt.Printf("    Used:   %d MB\n", usedSpace)
		}
	} else {
		fmt.Printf("    Status: %s\n", colorize(colorRed, "not initialized"))
		fmt.Printf("    %sUse 'sd.init' to remount%s\n", colorYellow, colorReset)
	}
	fmt.Println()
}

// ParseFlashStatus parses Flash status response: [init:u8][total:u32][used:u32][free:u32].
func ParseFlashStatus(payload []byte) {
	if len(payload) < 1 {
		fmt.Println("  (flash status too short)")
		return
	}
	initialized := payload[0] != 0
	fmt.Printf("\n  %sFlash Status%s\n", colorCyan, colorReset)
	if initialized && len(payload) >= 13 {
		total := ReadU32LE(payload, 1)
		used := ReadU32LE(payload, 5)
		free := ReadU32LE(payload, 9)
		fmt.Printf("    Status: %s\n", colorize(colorGreen, "initialized"))
		fmt.Printf("    Total:  %d bytes (%d KB)\n", total, total/1024)
		fmt.Printf("    Used:   %d bytes (%d KB)\n", used, used/1024)
		fmt.Printf("    Free:   %d bytes (%d KB)\n", free, free/1024)
	} else {
		fmt.Printf("    Status: %s\n", colorize(colorRed, "not initialized"))
	}
	fmt.Println()
}

// ParseUsbDevices parses USB_DEVICES_RESP: [init:u8][taskRunning:u8][backendLen:u8][backend...][count:u8] + per device.
func ParseUsbDevices(payload []byte) {
	if len(payload) < 4 {
		fmt.Println("  (USB devices too short)")
		return
	}

	pos := 0
	initialized := payload[pos] != 0; pos++
	taskRunning := payload[pos] != 0; pos++
	backendLen := int(payload[pos]); pos++

	if pos+backendLen > len(payload) {
		fmt.Println("  (malformed USB response)")
		return
	}
	backend := string(payload[pos : pos+backendLen]); pos += backendLen
	if pos >= len(payload) {
		fmt.Println("  (malformed USB response)")
		return
	}
	deviceCount := int(payload[pos]); pos++

	initStr := colorize(colorRed, "not initialized")
	if initialized {
		initStr = colorize(colorGreen, "initialized")
	}
	taskStr := colorize(colorRed, "stopped")
	if taskRunning {
		taskStr = colorize(colorGreen, "running")
	}

	fmt.Printf("\n  %sUSB Host (%s)%s\n", colorCyan, backend, colorReset)
	fmt.Printf("    Status: %s, Task: %s\n", initStr, taskStr)
	fmt.Printf("    CDC Devices: %d\n", deviceCount)

	if deviceCount == 0 {
		fmt.Printf("    %s(no USB devices)%s\n", colorYellow, colorReset)
		fmt.Println()
		return
	}

	stateNames := map[byte]string{0: "Disconnected", 1: "Connected", 2: "Mounted", 3: "Ready"}

	for i := 0; i < deviceCount; i++ {
		if pos+7 > len(payload) {
			break
		}
		addr := payload[pos]; pos++
		vid := ReadU16LE(payload, pos); pos += 2
		pid := ReadU16LE(payload, pos); pos += 2
		state := payload[pos]; pos++
		slaveType := payload[pos]; pos++

		stateText := stateNames[state]
		if stateText == "" {
			stateText = fmt.Sprintf("Unknown(%d)", state)
		}
		stateColor := colorRed
		if state == 3 {
			stateColor = colorGreen
		} else if state >= 1 {
			stateColor = colorYellow
		}
		slaveText := ""
		if slaveType > 0 {
			slaveText = " -> " + SlaveTypeName(slaveType)
		}
		fmt.Printf("    [%d] addr=%d VID=%04X PID=%04X %s%s%s%s\n",
			i, addr, vid, pid, stateColor, stateText, colorReset, slaveText)
	}
	fmt.Println()
}

// ParseCodecStatus parses CODEC_STATUS_RESP.
func ParseCodecStatus(payload []byte) {
	if len(payload) < 11 {
		fmt.Println("  (codec status too short)")
		return
	}
	codecType := payload[0]
	initialized := payload[1] != 0
	i2cOK := payload[2] != 0
	sdaPin := payload[3]
	sclPin := payload[4]
	supplyVolt := payload[5]
	muted := payload[6] != 0
	digitalVol := payload[7]
	deviceCtrl := payload[8]
	faultStatus := payload[9]
	codecNameLen := int(payload[10])

	codecName := ""
	if codecNameLen > 0 && len(payload) >= 11+codecNameLen {
		codecName = string(payload[11 : 11+codecNameLen])
	}

	codecTypes := map[byte]string{0: "Simple I2S (no control)", 1: "TAS5825M", 2: "PCM5102A"}
	codecTypeStr := codecTypes[codecType]
	if codecTypeStr == "" {
		codecTypeStr = fmt.Sprintf("Unknown (%d)", codecType)
	}
	supplyNames := map[byte]string{0: "12V", 1: "15V", 2: "20V", 3: "24V"}
	supplyStr := supplyNames[supplyVolt]
	if supplyStr == "" {
		supplyStr = fmt.Sprintf("Unknown (%d)", supplyVolt)
	}
	ctrlModes := map[byte]string{0x00: "Deep Sleep", 0x01: "Sleep", 0x02: "HiZ", 0x03: "Play"}
	ctrlStr := ctrlModes[deviceCtrl&0x03]
	if ctrlStr == "" {
		ctrlStr = fmt.Sprintf("Unknown (0x%02X)", deviceCtrl)
	}
	volDbStr := "MUTE"
	if digitalVol != 0xFF {
		volDb := float64(digitalVol-0x30) * 0.5
		volDbStr = fmt.Sprintf("%+.1f dB", volDb)
	}

	var faultStr string
	if faultStatus == 0xFF {
		faultStr = colorize(colorYellow, "read error")
	} else if faultStatus == 0 {
		faultStr = colorize(colorGreen, "none")
	} else {
		bits := []string{}
		if faultStatus&0x01 != 0 {
			bits = append(bits, "OT warning")
		}
		if faultStatus&0x02 != 0 {
			bits = append(bits, "OT error")
		}
		if faultStatus&0x04 != 0 {
			bits = append(bits, "OC")
		}
		if faultStatus&0x08 != 0 {
			bits = append(bits, "Clock fault")
		}
		faultStr = colorize(colorRed, fmt.Sprintf("%s (0x%02X)", strings.Join(bits, ", "), faultStatus))
	}

	initColor := colorRed
	if initialized {
		initColor = colorGreen
	}
	i2cColor := colorRed
	if i2cOK {
		i2cColor = colorGreen
	}

	display := codecName
	if display == "" {
		display = codecTypeStr
	}

	fmt.Printf("\n  %s═══ Codec Status ═══%s\n", colorCyan, colorReset)
	fmt.Printf("    Model:          %s\n", display)
	fmt.Printf("    Type:           %s\n", codecTypeStr)
	fmt.Printf("    Initialized:    %s%s%s\n", initColor, map[bool]string{true: "Yes", false: "No"}[initialized], colorReset)
	fmt.Printf("    I2C Connected:  %s%s%s\n", i2cColor, map[bool]string{true: "Yes", false: "No"}[i2cOK], colorReset)
	if sdaPin != 0xFF {
		fmt.Printf("    I2C Pins:       SDA=GPIO%d, SCL=GPIO%d\n", sdaPin, sclPin)
	}
	if codecType == 1 { // TAS5825M-specific
		muteColor := colorGreen
		if muted {
			muteColor = colorYellow
		}
		fmt.Printf("    Supply:         %s\n", supplyStr)
		fmt.Printf("    Muted:          %s%s%s\n", muteColor, map[bool]string{true: "Yes", false: "No"}[muted], colorReset)
		fmt.Printf("    Digital Volume: 0x%02X (%s)\n", digitalVol, volDbStr)
		fmt.Printf("    Device Ctrl:    0x%02X (%s)\n", deviceCtrl, ctrlStr)
		fmt.Printf("    Fault Status:   %s\n", faultStr)
	}
	fmt.Println()
}

// ParseSlaveInfo parses SLAVE_INFO_RESP payload.
func ParseSlaveInfo(payload []byte) {
	if len(payload) < 3 {
		fmt.Println("  (slave info too short)")
		return
	}
	pos := 0
	stype := payload[pos]; pos++
	ready := payload[pos] != 0; pos++
	connected := payload[pos] != 0; pos++

	typeName := SlaveTypeName(stype)
	statusColor := colorRed
	statusText := "disconnected"
	if ready {
		statusColor = colorGreen
		statusText = "ready"
	} else if connected {
		statusColor = colorYellow
		statusText = "connected"
	}

	readStr := func() string {
		if pos >= len(payload) {
			return ""
		}
		slen := int(payload[pos]); pos++
		if slen == 0 || pos+slen > len(payload) {
			return ""
		}
		s := string(payload[pos : pos+slen])
		pos += slen
		return s
	}

	name := readStr()
	version := readStr()
	platform := readStr()

	var cpuMHz, freeRAM, buildNum uint32
	if pos+4 <= len(payload) {
		cpuMHz = ReadU32LE(payload, pos); pos += 4
	}
	if pos+4 <= len(payload) {
		freeRAM = ReadU32LE(payload, pos); pos += 4
	}
	if pos+4 <= len(payload) {
		buildNum = ReadU32LE(payload, pos)
	}

	fmt.Printf("\n  %s%s Board Info%s\n", colorYellow, typeName, colorReset)
	fmt.Printf("    Status:    %s%s%s\n", statusColor, statusText, colorReset)
	if name != "" {
		fmt.Printf("    Name:      %s\n", name)
	}
	if version != "" {
		fmt.Printf("    Version:   %s (build %d)\n", version, buildNum)
	}
	if platform != "" {
		fmt.Printf("    Platform:  %s\n", platform)
	}
	if cpuMHz > 0 {
		fmt.Printf("    CPU:       %d MHz\n", cpuMHz)
	}
	if freeRAM > 0 {
		fmt.Printf("    Free RAM:  %d bytes\n", freeRAM)
	}
	fmt.Println()
}
