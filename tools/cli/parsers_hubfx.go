package main

// ScaleFX CLI - HubFX Response Parsers

import (
	"fmt"
	"strings"
)

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
