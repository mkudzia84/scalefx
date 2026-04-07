package engine

// ScaleFX Engine - HubFX Response Parsers

import (
	"fmt"
	"scalefx/protocol"
	"scalefx/protocol/hubfx"
	"strings"
)

func (e *Engine) parseHubFXStatus(data []byte) {
	// Wire format v1: [flags:u8][slaveMask:u8][loop1Count:u32LE] = 6 bytes
	// Wire format v2: + [i2cDeviceMask:u8][ina226_mV[0..5]:u16LE x 6] = 19 bytes
	if len(data) < 2 {
		e.Out.Printf("  Hub data: %d bytes\n", len(data))
		return
	}

	flags := data[0]
	slaveMask := data[1]
	loop1Count := uint32(0)
	if len(data) >= 6 {
		loop1Count = protocol.ReadU32LE(data, 2)
	}

	core1Ready := flags&0x01 != 0
	audioInit := flags&0x02 != 0
	flashReady := flags&0x04 != 0
	usbReady := flags&0x08 != 0
	sdReady := flags&0x10 != 0

	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "━━━ HubFX Status ━━━"))

	// Core 1
	c1Color := ColorRed
	c1Text := "NOT READY"
	if core1Ready {
		c1Color = ColorGreen
		c1Text = "Ready"
	}
	e.Out.Printf("  Core 1:    %s\n", e.Out.C(c1Color, c1Text))
	if len(data) >= 6 {
		e.Out.Printf("             %d iterations\n", loop1Count)
	}

	// Audio
	audioColor := ColorYellow
	audioText := "Not initialized"
	if audioInit {
		audioColor = ColorGreen
		audioText = "Initialized"
	}
	e.Out.Printf("  Audio:     %s\n", e.Out.C(audioColor, audioText))

	// Flash
	flashColor := ColorYellow
	flashText := "Not available"
	if flashReady {
		flashColor = ColorGreen
		flashText = "Ready"
	}
	e.Out.Printf("  Flash:     %s\n", e.Out.C(flashColor, flashText))

	// SD Card
	sdColor := ColorYellow
	sdText := "Not available"
	if sdReady {
		sdColor = ColorGreen
		sdText = "Ready"
	}
	e.Out.Printf("  SD Card:   %s\n", e.Out.C(sdColor, sdText))

	// USB Host
	usbColor := ColorYellow
	usbText := "Not active"
	if usbReady {
		usbColor = ColorGreen
		usbText = "Active"
	}
	e.Out.Printf("  USB Host:  %s\n", e.Out.C(usbColor, usbText))

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
		e.Out.Printf("  Slaves:\n")
		for bit := 0; bit <= 2; bit++ {
			name := slaveNames[bit]
			isReady := slaveMask&(1<<bit) != 0
			color := ColorRed
			status := "not connected"
			if isReady {
				color = ColorGreen
				status = "connected"
			}
			e.Out.Printf("    %s: %s\n", name, e.Out.C(color, status))
		}
	} else {
		e.Out.Printf("  Slaves:    %s\n", e.Out.C(ColorYellow, "None connected"))
	}

	// I2C device status (v2 extended, 13 bytes at offset 6)
	if len(data) >= 19 {
		i2cMask := data[6]
		detected := 0
		for b := 0; b < 8; b++ {
			if i2cMask&(1<<b) != 0 {
				detected++
			}
		}
		e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, fmt.Sprintf("━━━ I2C Devices (%d/8) ━━━", detected)))

		// PCAL6416A
		pcalOK := i2cMask&0x01 != 0
		pcalColor := ColorRed
		pcalText := "not found"
		if pcalOK {
			pcalColor = ColorGreen
			pcalText = "OK"
		}
		e.Out.Printf("  PCAL6416A: %s  (0x20 GPIO expander)\n", e.Out.C(pcalColor, pcalText))

		// INA226 monitors with voltage readings
		for i := 0; i < 6; i++ {
			present := i2cMask&(1<<(i+1)) != 0
			voltage_mV := protocol.ReadU16LE(data, 7+i*2)
			addr := 0x40 + i
			if present {
				voltage_V := float64(voltage_mV) / 1000.0
				e.Out.Printf("  INA226[%d]: %s  (0x%02X)\n",
					i, e.Out.C(ColorGreen, fmt.Sprintf("%.3fV (%d mV)", voltage_V, voltage_mV)), addr)
			} else {
				e.Out.Printf("  INA226[%d]: %s  (0x%02X)\n",
					i, e.Out.C(ColorRed, "not found"), addr)
			}
		}

		// TAS5825M (reserved bit 7)
		if i2cMask&0x80 != 0 {
			e.Out.Printf("  TAS5825M:  %s  (0x4C audio codec)\n", e.Out.C(ColorGreen, "OK"))
		}
	} else if len(data) >= 7 {
		i2cMask := data[6]
		if i2cMask != 0 {
			e.Out.Printf("\n  I2C mask:  0x%02X\n", i2cMask)
		}
	}
}

// ParseSlaveList parses SLAVE_LIST_RESP: [count:u8] + per-entry [type:u8][connected:u8][ready:u8][nameLen:u8][name...].
func (e *Engine) ParseSlaveList(payload []byte) {
	if len(payload) < 1 {
		e.Out.Println("  (empty slave list)")
		return
	}
	count := payload[0]
	e.Out.Printf("  ── Slave Controllers (%d) ──\n", count)
	if count == 0 {
		e.Out.Println("  (no slaves registered)")
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

		typeName := hubfx.SlaveTypeName(stype)
		statusColor := ColorRed
		statusText := "disconnected"
		if ready != 0 {
			statusColor = ColorGreen
			statusText = "ready"
		} else if connected != 0 {
			statusColor = ColorYellow
			statusText = "connected"
		}
		displayName := ""
		if name != "" {
			displayName = fmt.Sprintf(" (%s)", name)
		}
		e.Out.Printf("  [%d] %s%s: %s\n", i, typeName, displayName, e.Out.C(statusColor, statusText))
	}
}

// ParseAudioStatus parses AUDIO_STATUS_RESP (v3/v4 extended format).
func (e *Engine) ParseAudioStatus(payload []byte) {
	if len(payload) < 7 {
		e.Out.Println("  (audio status too short)")
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

	sampleRate := protocol.ReadU16LE(payload, pos); pos += 2
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
		ringAvailRead = protocol.ReadU16LE(payload, pos); pos += 2
		ringAvailWrite = protocol.ReadU16LE(payload, pos); pos += 2
		underruns = protocol.ReadU32LE(payload, pos); pos += 4
		if pos+8 <= len(payload) {
			consumeLoops = protocol.ReadU32LE(payload, pos); pos += 4
			consumeFrames = protocol.ReadU32LE(payload, pos); pos += 4
		}
	}

	// Buffer capacities (v4)
	var wavBufCapacity, ringCapacity uint16
	if hasBufferCaps && pos+4 <= len(payload) {
		wavBufCapacity = protocol.ReadU16LE(payload, pos); pos += 2
		ringCapacity = protocol.ReadU16LE(payload, pos); pos += 2
	}

	// Display
	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "Audio Mixer Status"))
	initStr := e.Out.C(ColorRed, "no")
	if initialized {
		initStr = e.Out.C(ColorGreen, "yes")
	}
	i2sStr := e.Out.C(ColorRed, "stopped")
	if i2sRunning {
		i2sStr = e.Out.C(ColorGreen, "running")
	}
	e.Out.Printf("    Initialized: %s\n", initStr)
	e.Out.Printf("    I2S:         %s (%dHz / %dbit)\n", i2sStr, sampleRate, bitDepth)
	if hasCodec {
		e.Out.Printf("    Codec:       %s\n", codecName)
	} else {
		e.Out.Printf("    Codec:       %s\n", e.Out.C(ColorYellow, "none (I2S only)"))
	}
	e.Out.Printf("    Max Ch:      %d\n", maxChannels)
	e.Out.Printf("    Master Vol:  %d%%\n", masterVol)

	if hasRingStats {
		underrunStr := e.Out.C(ColorGreen, "0")
		if underruns > 0 {
			underrunStr = e.Out.C(ColorRed, fmt.Sprintf("%d", underruns))
		}
		fillColor := ColorRed
		if ringFillPct >= 50 {
			fillColor = ColorGreen
		} else if ringFillPct >= 25 {
			fillColor = ColorYellow
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
		e.Out.Printf("    Ring Buf:    %s (%d%s frames%s)\n",
			e.Out.C(fillColor, fmt.Sprintf("%d%%", ringFillPct)), ringAvailRead, ringCapStr, ringMs)
		e.Out.Printf("    Underruns:   %s\n", underrunStr)
		e.Out.Printf("    Consumer:    %d loops, %d frames written to I2S\n", consumeLoops, consumeFrames)
	}
	if hasBufferCaps && wavBufCapacity > 0 {
		wavMs := uint32(0)
		if sampleRate > 0 {
			wavMs = uint32(wavBufCapacity) * 1000 / uint32(sampleRate)
		}
		e.Out.Printf("    WAV Buf:     %d frames/ch (%dms, %dKB total)\n", wavBufCapacity, wavMs, uint32(wavBufCapacity)*8*2*4/1024)
	}

	if pos >= len(payload) {
		e.Out.Printf("    %s\n", e.Out.C(ColorYellow, "No channel data"))
		return
	}

	activeMask := payload[pos]; pos++
	if activeMask == 0 {
		e.Out.Printf("    %s\n", e.Out.C(ColorYellow, "No active channels"))
		return
	}

	activeCount := 0
	for b := activeMask; b != 0; b >>= 1 {
		activeCount += int(b & 1)
	}
	e.Out.Printf("    Active:      %d channel(s) (mask: 0b%08b)\n", activeCount, activeMask)
	e.Out.Println("")

	outputNames := map[byte]string{hubfx.AudioOutputCh1: "ch1", hubfx.AudioOutputCh2: "ch2", hubfx.AudioOutputAll: "all"}

	for i := 0; i < activeCount; i++ {
		if pos+16 > len(payload) {
			break
		}
		ch := payload[pos]; pos++
		vol := payload[pos]; pos++
		playing := payload[pos] != 0; pos++
		looping := payload[pos] != 0; pos++
		loopCount := protocol.ReadU16LE(payload, pos); pos += 2
		remaining_ms := protocol.ReadU32LE(payload, pos); pos += 4
		queueLen := payload[pos]; pos++
		output := payload[pos]; pos++

		wavRate := protocol.ReadU16LE(payload, pos); pos += 2
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

		status := e.Out.C(ColorYellow, "- queued")
		if playing {
			status = e.Out.C(ColorGreen, "> playing")
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
			bufColor := ColorRed
			if wavBufFill >= 80 {
				bufColor = ColorGreen
			} else if wavBufFill >= 40 {
				bufColor = ColorYellow
			}
			bufStr = fmt.Sprintf(" buf=%s", e.Out.C(bufColor, fmt.Sprintf("%d%%", wavBufFill)))
		}

		e.Out.Printf("    ch%d: %s vol=%d%% %s%s%s%s%s\n", ch, status, vol, outName, loopStr, remainingStr, queueStr, bufStr)
		if fname != "" {
			e.Out.Printf("          file: %s\n", fname)
		}
		if wavRate > 0 {
			chStr := "mono"
			if wavCh == 2 {
				chStr = "stereo"
			}
			e.Out.Printf("          wav:  %dHz/%dbit/%s\n", wavRate, wavBits, chStr)
		}
	}
	e.Out.Println("")
}

// ParseEngineStatus parses ENGINE_STATUS_RESP: [state:u8][toggle:u8][active:u8].
func (e *Engine) ParseEngineStatus(payload []byte) {
	if len(payload) < 3 {
		e.Out.Println("  (engine status too short)")
		return
	}
	state := payload[0]
	toggle := payload[1] != 0
	active := payload[2] != 0

	stateName := hubfx.EngineStateName(state)
	stateColor := ColorRed
	icon := "-"
	switch state {
	case 0:
		stateColor = ColorRed; icon = "-" // Stopped
	case 1:
		stateColor = ColorYellow; icon = "*" // Starting
	case 2:
		stateColor = ColorGreen; icon = ">" // Running
	case 3:
		stateColor = ColorYellow; icon = "~" // Stopping
	}

	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "Engine FX Status"))
	e.Out.Printf("    State:    %s %s\n", icon, e.Out.C(stateColor, stateName))
	e.Out.Printf("    Toggle:   %s\n", map[bool]string{true: "engaged", false: "disengaged"}[toggle])
	e.Out.Printf("    Active:   %s\n", map[bool]string{true: "yes", false: "no"}[active])
	e.Out.Println("")
}

// ParseConfigStatus parses CONFIG_STATUS_RESP: [loaded:u8][size:u16LE][valid:u8].
func (e *Engine) ParseConfigStatus(payload []byte) {
	if len(payload) < 4 {
		e.Out.Println("  (config status too short)")
		return
	}
	loaded := payload[0] != 0
	size := protocol.ReadU16LE(payload, 1)
	valid := payload[3] != 0

	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "Config Status"))
	statusStr := e.Out.C(ColorRed, "not loaded")
	if loaded {
		statusStr = e.Out.C(ColorGreen, "loaded")
	}
	e.Out.Printf("    Status:     %s\n", statusStr)
	e.Out.Printf("    Size:       %d bytes\n", size)
	if loaded {
		validStr := e.Out.C(ColorYellow, "invalid")
		if valid {
			validStr = e.Out.C(ColorGreen, "valid")
		}
		e.Out.Printf("    Validation: %s\n", validStr)
	}
	e.Out.Println("")
}

// ParseSdStatus parses SD_STATUS_RESP.
func (e *Engine) ParseSdStatus(payload []byte) {
	if len(payload) < 1 {
		e.Out.Println("  (SD status too short)")
		return
	}
	initialized := payload[0] != 0

	cardTypes := map[byte]string{0: "NONE", 1: "MMC", 2: "SD", 3: "SDHC", 4: "UNKNOWN"}
	busModes := map[byte]string{0: "SPI", 1: "SDIO 1-bit", 2: "SDIO 4-bit"}

	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "SD Card Status"))
	if initialized {
		e.Out.Printf("    Status: %s\n", e.Out.C(ColorGreen, "initialized"))
		if len(payload) >= 14 {
			cardSize := protocol.ReadU32LE(payload, 1)
			totalSpace := protocol.ReadU32LE(payload, 5)
			freeSpace := protocol.ReadU32LE(payload, 9)
			fatType := payload[13]
			e.Out.Printf("    Card:   %d MB\n", cardSize)
			e.Out.Printf("    Total:  %d MB\n", totalSpace)
			e.Out.Printf("    Free:   %d MB\n", freeSpace)
			if fatType > 0 {
				e.Out.Printf("    FAT:    FAT%d\n", fatType)
			}
		}
		if len(payload) >= 20 {
			cardType := payload[14]
			busMode := payload[15]
			usedSpace := protocol.ReadU32LE(payload, 16)
			typeName := cardTypes[cardType]
			if typeName == "" {
				typeName = fmt.Sprintf("0x%02X", cardType)
			}
			busName := busModes[busMode]
			if busName == "" {
				busName = fmt.Sprintf("0x%02X", busMode)
			}
			e.Out.Printf("    Type:   %s\n", typeName)
			e.Out.Printf("    Bus:    %s\n", busName)
			e.Out.Printf("    Used:   %d MB\n", usedSpace)
		}
	} else {
		e.Out.Printf("    Status: %s\n", e.Out.C(ColorRed, "not initialized"))
		e.Out.Printf("    %s\n", e.Out.C(ColorYellow, "Use 'sd.init' to remount"))
	}
	e.Out.Println("")
}

// ParseFlashStatus parses Flash status response: [init:u8][total:u32][used:u32][free:u32].
func (e *Engine) ParseFlashStatus(payload []byte) {
	if len(payload) < 1 {
		e.Out.Println("  (flash status too short)")
		return
	}
	initialized := payload[0] != 0
	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "Flash Status"))
	if initialized && len(payload) >= 13 {
		total := protocol.ReadU32LE(payload, 1)
		used := protocol.ReadU32LE(payload, 5)
		free := protocol.ReadU32LE(payload, 9)
		e.Out.Printf("    Status: %s\n", e.Out.C(ColorGreen, "initialized"))
		e.Out.Printf("    Total:  %d bytes (%d KB)\n", total, total/1024)
		e.Out.Printf("    Used:   %d bytes (%d KB)\n", used, used/1024)
		e.Out.Printf("    Free:   %d bytes (%d KB)\n", free, free/1024)
	} else {
		e.Out.Printf("    Status: %s\n", e.Out.C(ColorRed, "not initialized"))
	}
	e.Out.Println("")
}

// ParseUsbDevices parses USB_DEVICES_RESP.
func (e *Engine) ParseUsbDevices(payload []byte) {
	if len(payload) < 4 {
		e.Out.Println("  (USB devices too short)")
		return
	}

	pos := 0
	initialized := payload[pos] != 0; pos++
	taskRunning := payload[pos] != 0; pos++
	backendLen := int(payload[pos]); pos++

	if pos+backendLen > len(payload) {
		e.Out.Println("  (malformed USB response)")
		return
	}
	backend := string(payload[pos : pos+backendLen]); pos += backendLen
	if pos >= len(payload) {
		e.Out.Println("  (malformed USB response)")
		return
	}
	deviceCount := int(payload[pos]); pos++

	initStr := e.Out.C(ColorRed, "not initialized")
	if initialized {
		initStr = e.Out.C(ColorGreen, "initialized")
	}
	taskStr := e.Out.C(ColorRed, "stopped")
	if taskRunning {
		taskStr = e.Out.C(ColorGreen, "running")
	}

	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, fmt.Sprintf("USB Host (%s)", backend)))
	e.Out.Printf("    Status: %s, Task: %s\n", initStr, taskStr)
	e.Out.Printf("    CDC Devices: %d\n", deviceCount)

	if deviceCount == 0 {
		e.Out.Printf("    %s\n", e.Out.C(ColorYellow, "(no USB devices)"))
		e.Out.Println("")
		return
	}

	stateNames := map[byte]string{0: "Disconnected", 1: "Connected", 2: "Mounted", 3: "Ready"}

	for i := 0; i < deviceCount; i++ {
		if pos+7 > len(payload) {
			break
		}
		addr := payload[pos]; pos++
		vid := protocol.ReadU16LE(payload, pos); pos += 2
		pid := protocol.ReadU16LE(payload, pos); pos += 2
		state := payload[pos]; pos++
		slaveType := payload[pos]; pos++

		stateText := stateNames[state]
		if stateText == "" {
			stateText = fmt.Sprintf("Unknown(%d)", state)
		}
		stateColor := ColorRed
		if state == 3 {
			stateColor = ColorGreen
		} else if state >= 1 {
			stateColor = ColorYellow
		}
		slaveText := ""
		if slaveType > 0 {
			slaveText = " -> " + hubfx.SlaveTypeName(slaveType)
		}
		e.Out.Printf("    [%d] addr=%d VID=%04X PID=%04X %s%s\n",
			i, addr, vid, pid, e.Out.C(stateColor, stateText), slaveText)
	}
	e.Out.Println("")
}

// ParseCodecStatus parses CODEC_STATUS_RESP.
func (e *Engine) ParseCodecStatus(payload []byte) {
	if len(payload) < 11 {
		e.Out.Println("  (codec status too short)")
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
		faultStr = e.Out.C(ColorYellow, "read error")
	} else if faultStatus == 0 {
		faultStr = e.Out.C(ColorGreen, "none")
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
		faultStr = e.Out.C(ColorRed, fmt.Sprintf("%s (0x%02X)", strings.Join(bits, ", "), faultStatus))
	}

	initColor := ColorRed
	if initialized {
		initColor = ColorGreen
	}
	i2cColor := ColorRed
	if i2cOK {
		i2cColor = ColorGreen
	}

	display := codecName
	if display == "" {
		display = codecTypeStr
	}

	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "═══ Codec Status ═══"))
	e.Out.Printf("    Model:          %s\n", display)
	e.Out.Printf("    Type:           %s\n", codecTypeStr)
	e.Out.Printf("    Initialized:    %s\n", e.Out.C(initColor, map[bool]string{true: "Yes", false: "No"}[initialized]))
	e.Out.Printf("    I2C Connected:  %s\n", e.Out.C(i2cColor, map[bool]string{true: "Yes", false: "No"}[i2cOK]))
	if sdaPin != 0xFF {
		e.Out.Printf("    I2C Pins:       SDA=GPIO%d, SCL=GPIO%d\n", sdaPin, sclPin)
	}
	if codecType == 1 { // TAS5825M-specific
		muteColor := ColorGreen
		if muted {
			muteColor = ColorYellow
		}
		e.Out.Printf("    Supply:         %s\n", supplyStr)
		e.Out.Printf("    Muted:          %s\n", e.Out.C(muteColor, map[bool]string{true: "Yes", false: "No"}[muted]))
		e.Out.Printf("    Digital Volume: 0x%02X (%s)\n", digitalVol, volDbStr)
		e.Out.Printf("    Device Ctrl:    0x%02X (%s)\n", deviceCtrl, ctrlStr)
		e.Out.Printf("    Fault Status:   %s\n", faultStr)
	}
	e.Out.Println("")
}

// ParseSlaveInfo parses SLAVE_INFO_RESP payload.
func (e *Engine) ParseSlaveInfo(payload []byte) {
	if len(payload) < 3 {
		e.Out.Println("  (slave info too short)")
		return
	}
	pos := 0
	stype := payload[pos]; pos++
	ready := payload[pos] != 0; pos++
	connected := payload[pos] != 0; pos++

	typeName := hubfx.SlaveTypeName(stype)
	statusColor := ColorRed
	statusText := "disconnected"
	if ready {
		statusColor = ColorGreen
		statusText = "ready"
	} else if connected {
		statusColor = ColorYellow
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
		s := string(payload[pos : pos+slen]); pos += slen
		return s
	}

	name := readStr()
	version := readStr()
	platform := readStr()

	var cpuMHz, freeRAM, buildNum uint32
	if pos+4 <= len(payload) {
		cpuMHz = protocol.ReadU32LE(payload, pos); pos += 4
	}
	if pos+4 <= len(payload) {
		freeRAM = protocol.ReadU32LE(payload, pos); pos += 4
	}
	if pos+4 <= len(payload) {
		buildNum = protocol.ReadU32LE(payload, pos)
	}

	e.Out.Printf("\n  %s\n", e.Out.C(ColorYellow, typeName+" Board Info"))
	e.Out.Printf("    Status:    %s\n", e.Out.C(statusColor, statusText))
	if name != "" {
		e.Out.Printf("    Name:      %s\n", name)
	}
	if version != "" {
		e.Out.Printf("    Version:   %s (build %d)\n", version, buildNum)
	}
	if platform != "" {
		e.Out.Printf("    Platform:  %s\n", platform)
	}
	if cpuMHz > 0 {
		e.Out.Printf("    CPU:       %d MHz\n", cpuMHz)
	}
	if freeRAM > 0 {
		e.Out.Printf("    Free RAM:  %d bytes\n", freeRAM)
	}
	e.Out.Println("")
}
