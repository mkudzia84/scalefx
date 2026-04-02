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
	if len(data) < 28 {
		fmt.Printf("  GunFX module data (%d bytes, expected 28)\n", len(data))
		return
	}
	firing := data[0] != 0
	rpm := ReadU16LE(data, 1)
	fmt.Printf("  ── GunFX Status ──\n")
	fmt.Printf("  Firing:     %v (RPM: %d)\n", firing, rpm)

	// Servos (3x): [pulse_us:u16][configured:u8]
	offset := 3
	for i := 0; i < 3; i++ {
		if offset+3 > len(data) {
			break
		}
		pulse := ReadU16LE(data, offset)
		configured := data[offset+2] != 0
		fmt.Printf("  Servo %d:    %d µs (configured: %v)\n", i+1, pulse, configured)
		offset += 3
	}

	// Smoke: [heaterOn:u8][fanSpeed:u8][heater_mA:u16][fan_mA:u16][temp_C:i16][errorReason:u8]
	if offset+8 <= len(data) {
		heaterOn := data[offset] != 0
		fanSpeed := data[offset+1]
		heater_mA := ReadU16LE(data, offset+2)
		fan_mA := ReadU16LE(data, offset+4)
		temp_C := ReadI16LE(data, offset+6)
		offset += 8
		fmt.Printf("  Smoke:\n")
		fmt.Printf("    Heater:   %v (%d mA)\n", heaterOn, heater_mA)
		fmt.Printf("    Fan:      %d%% (%d mA)\n", fanSpeed, fan_mA)
		fmt.Printf("    Temp:     %d°C\n", temp_C)
		if offset < len(data) {
			errReason := data[offset]
			if errReason != 0 {
				errNames := map[byte]string{
					1: "heater disconnected", 2: "fan disconnected",
					3: "heater overcurrent", 4: "fan overcurrent",
				}
				name := errNames[errReason]
				if name == "" {
					name = fmt.Sprintf("unknown(0x%02X)", errReason)
				}
				fmt.Printf("    Error:    %s%s%s\n", colorRed, name, colorReset)
			}
		}
	}
}

func parseGearControlStatus(data []byte) {
	fmt.Printf("  ── GearControl Status ──\n")
	// Per-gear data structure (53 bytes for 3 gears, from README)
	// This is complex — show key fields
	if len(data) < 3 {
		fmt.Printf("  Module data: %d bytes\n", len(data))
		return
	}

	// Each gear block: state, errorReason, flags, calibState, plus config data
	// For simplicity, show gear states at offsets known from the protocol
	gearBlockSize := 17 // approximate per-gear block
	numGears := 3
	if len(data) < numGears*gearBlockSize {
		// Just show raw
		fmt.Printf("  Module data (%d bytes):", len(data))
		for _, b := range data {
			fmt.Printf(" %02X", b)
		}
		fmt.Println()
		return
	}

	for i := 0; i < numGears; i++ {
		offset := i * gearBlockSize
		if offset+3 > len(data) {
			break
		}
		state := data[offset]
		errReason := data[offset+1]
		flags := data[offset+2]

		gearNames := []string{"Nose", "Left", "Right"}
		name := gearNames[i]
		stateName := GearStateName(state)

		fmt.Printf("  Gear %d (%s): %s", i, name, stateName)
		if errReason != 0 {
			fmt.Printf(" [error: 0x%02X]", errReason)
		}
		enabled := flags&0x80 != 0
		if !enabled {
			fmt.Printf(" (DISABLED)")
		}
		fmt.Println()
	}

	// Yaw + battery at end
	yawOffset := numGears * gearBlockSize
	if yawOffset+2 <= len(data) {
		yaw_us := ReadU16LE(data, yawOffset)
		fmt.Printf("  Yaw:        %d µs\n", yaw_us)
	}
}

func parseLightFXStatus(data []byte) {
	fmt.Printf("  ── LightFX Status ──\n")
	if len(data) < 1 {
		return
	}
	// LED channels: each has brightness + sequence running flag
	fmt.Printf("  Module data (%d bytes):", len(data))
	for _, b := range data {
		fmt.Printf(" %02X", b)
	}
	fmt.Println()
}

func parseHubFXStatus(data []byte) {
	fmt.Printf("  ── HubFX Status ──\n")
	if len(data) < 4 {
		fmt.Printf("  Module data (%d bytes)\n", len(data))
		return
	}
	// HubFX status: [flags:u16LE][slaveMask:u8][audioMask:u8]...
	flags := ReadU16LE(data, 0)
	fmt.Printf("  Flags:      0x%04X\n", flags)
	if len(data) >= 3 {
		slaveMask := data[2]
		fmt.Printf("  Slaves:     0x%02X", slaveMask)
		if slaveMask&0x01 != 0 {
			fmt.Print(" GunFX")
		}
		if slaveMask&0x02 != 0 {
			fmt.Print(" LightFX")
		}
		if slaveMask&0x04 != 0 {
			fmt.Print(" GearControl")
		}
		fmt.Println()
	}
	if len(data) > 4 {
		fmt.Printf("  Extended (%d bytes):", len(data)-4)
		for _, b := range data[4:] {
			fmt.Printf(" %02X", b)
		}
		fmt.Println()
	}
}

// ParseI2CScanResult parses I2C_SCAN_RESULT payload.
func ParseI2CScanResult(payload []byte) {
	if len(payload) < 1 {
		fmt.Println("  (empty scan result)")
		return
	}

	// Format: [expectedCount:u8][expected: addr,status pairs...][extraCount:u8][extra addrs...]
	offset := 0
	expectedCount := int(payload[offset])
	offset++

	if expectedCount > 0 {
		fmt.Printf("  Expected devices (%d):\n", expectedCount)
		for i := 0; i < expectedCount && offset+1 < len(payload); i++ {
			addr := payload[offset]
			status := payload[offset+1]
			offset += 2
			statusStr := "MISSING"
			if status == 1 {
				statusStr = colorize(colorGreen, "OK")
			}
			fmt.Printf("    0x%02X: %s\n", addr, statusStr)
		}
	}

	if offset < len(payload) {
		extraCount := int(payload[offset])
		offset++
		if extraCount > 0 {
			fmt.Printf("  Extra devices (%d):\n", extraCount)
			for i := 0; i < extraCount && offset < len(payload); i++ {
				fmt.Printf("    0x%02X\n", payload[offset])
				offset++
			}
		}
	}
}

// ParseGearCalibStatus parses GEAR_CALIB_STATUS async payload.
func ParseGearCalibStatus(payload []byte) {
	if len(payload) < 4 {
		return
	}
	gearID := payload[0]
	phase := payload[1]
	// progress data follows
	phaseNames := map[byte]string{
		0: "IDLE", 1: "CLEAR_RUN", 2: "CLEAR_SETTLE", 3: "DEPLOY_RUN",
		4: "MID_SETTLE", 5: "RETRACT_RUN", 6: "COMPLETE", 7: "ERROR",
		8: "CANCELLED", 9: "OPENING_DOORS", 10: "CLOSING_DOORS",
	}
	phaseName := phaseNames[phase]
	if phaseName == "" {
		phaseName = fmt.Sprintf("?(%d)", phase)
	}
	fmt.Printf("  Calibration gear %d: %s\n", gearID, phaseName)

	if phase == 6 && len(payload) >= 8 { // COMPLETE
		stall_mA := ReadU16LE(payload, 2)
		deployTime := ReadU16LE(payload, 4)
		retractTime := ReadU16LE(payload, 6)
		fmt.Printf("    Stall current: %d mA\n", stall_mA)
		fmt.Printf("    Deploy time:   %d ms\n", deployTime)
		fmt.Printf("    Retract time:  %d ms\n", retractTime)
	}
}

// ParseGearSeqStatus parses GEAR_SEQ_STATUS async payload.
func ParseGearSeqStatus(payload []byte) {
	if len(payload) < 4 {
		return
	}
	gearID := payload[0]
	phase := payload[1]
	deploying := payload[2] != 0
	finished := payload[3] != 0

	phaseNames := map[byte]string{
		0: "idle", 1: "opening doors", 2: "running motor",
		3: "closing doors", 4: "error", 5: "sync wait",
	}
	phaseName := phaseNames[phase]
	if phaseName == "" {
		phaseName = fmt.Sprintf("?(%d)", phase)
	}

	action := "retracting"
	if deploying {
		action = "deploying"
	}

	if finished {
		fmt.Printf("  Gear %d: %s complete\n", gearID, action)
	} else {
		fmt.Printf("  Gear %d: %s — %s\n", gearID, action, phaseName)
	}
}

// ParseGearDoorStatus parses GEAR_DOOR_STATUS async payload.
func ParseGearDoorStatus(payload []byte) {
	if len(payload) < 5 {
		return
	}
	gearID := payload[0]
	state := payload[1]
	door0_us := ReadU16LE(payload, 2)
	if len(payload) >= 6 {
		door1_us := ReadU16LE(payload, 4)
		fmt.Printf("  Gear %d doors: %s (d0=%dµs, d1=%dµs)\n",
			gearID, DoorStateName(state), door0_us, door1_us)
	} else {
		fmt.Printf("  Gear %d doors: %s (d0=%dµs)\n",
			gearID, DoorStateName(state), door0_us)
	}
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
