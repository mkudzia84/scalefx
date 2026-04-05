package main

// ScaleFX CLI - GearControl Response Parsers

import (
	"fmt"
	"strings"
)

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

	gearName := gearIDName(gearID)

	calibPhaseNames := map[byte]string{
		0: "idle", 1: "clear run", 2: "clear settle", 3: "deploy run",
		4: "mid settle", 5: "retract run", 6: "complete", 7: "ERROR",
		8: "cancelled", 9: "opening doors", 10: "closing doors",
	}
	phaseName := calibPhaseNames[phase]
	if phaseName == "" {
		phaseName = fmt.Sprintf("?(%d)", phase)
	}

	// Finished completion line
	if finished {
		if phase == 7 { // ERROR
			reason := ""
			if errorReason > 0 {
				reason = " — " + GearErrorReasonName(errorReason)
			}
			fmt.Printf("  %s✗%s %-10s calib %s%s\n",
				colorRed, colorReset, gearName,
				colorize(colorRed, "ERROR"), reason)
		} else if phase == 8 { // CANCELLED
			fmt.Printf("  %s⚠%s %-10s calib %s\n",
				colorYellow, colorReset, gearName,
				colorize(colorYellow, "cancelled"))
		} else { // COMPLETE
			result := fmt.Sprintf("stall=%dmA", stallMA)
			if peakMA > 0 {
				result += fmt.Sprintf(" peak=%dmA", peakMA)
			}
			fmt.Printf("  %s✓%s %-10s calib complete (%s)\n",
				colorGreen, colorReset, gearName, result)
		}
		return
	}

	// In-progress line
	phaseColor := colorReset
	switch phase {
	case 1, 3, 5: // Motor running phases
		phaseColor = colorCyan
	case 9, 10: // Door phases
		phaseColor = colorYellow
	}

	var info []string
	info = append(info, fmt.Sprintf("%dmA", currentMA))
	if peakMA > 0 {
		info = append(info, fmt.Sprintf("peak %dmA", peakMA))
	}

	fmt.Printf("  %s◆%s %-10s calib %s%s%s (%s)\n",
		colorMagenta, colorReset, gearName,
		phaseColor, phaseName, colorReset,
		strings.Join(info, ", "))
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

	gearName := gearIDName(gearID)
	phaseName := GearSeqPhaseName(phase)
	action := "retract"
	actionColor := colorCyan
	if deploying {
		action = "deploy"
		actionColor = colorGreen
	}

	elapsedSec := float64(elapsedMs) / 1000.0

	// Finished line: prominent completion display
	if finished {
		if phase == 4 { // SEQ_ERROR
			fmt.Printf("  %s✗%s %-10s %s %s in %.1fs\n",
				colorRed, colorReset, gearName,
				colorize(colorRed, "ERROR"),
				action, elapsedSec)
		} else {
			fmt.Printf("  %s✓%s %-10s %s%s%s complete (%.1fs)\n",
				colorGreen, colorReset, gearName,
				actionColor, action, colorReset, elapsedSec)
		}
		return
	}

	// In-progress line: phase, action, elapsed
	phaseColor := colorYellow
	switch phase {
	case 2: // RUNNING_MOTOR
		phaseColor = colorCyan
	case 5: // SYNC_WAIT
		phaseColor = colorGray
	}

	fmt.Printf("  %s▸%s %-10s %s%s%s %s (%.1fs)\n",
		colorMagenta, colorReset, gearName,
		phaseColor, phaseName, colorReset,
		action, elapsedSec)
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

	gearName := gearIDName(gearID)
	stateName := DoorStateName(state)

	stateColor := colorYellow
	switch state {
	case 1: // CLOSED
		stateColor = colorCyan
	case 2: // OPEN
		stateColor = colorGreen
	}

	posInfo := ""
	if len(payload) >= 6 {
		posInfo = fmt.Sprintf(" (%dµs, %dµs)", ReadU16LE(payload, 2), ReadU16LE(payload, 4))
	} else if len(payload) >= 4 {
		posInfo = fmt.Sprintf(" (%dµs)", ReadU16LE(payload, 2))
	}

	fmt.Printf("  %s◇%s %-10s doors %s%s%s%s\n",
		colorGray, colorReset, gearName,
		stateColor, stateName, colorReset, posInfo)
}
