package hubfx

// ScaleFX Engine - HubFX Query-Response Parsers
// Display helpers invoked from cmdSlaves / cmdAudioStatus / cmdCodecStatus /
// cmdSdStatus / cmdFlashStatus / cmdUsbDevices / cmdSlaveInfo / cmdEngineStatus
// — they render typed query responses and stay here because they aren't fired
// through the observer chain. Broadcast + sync-status paths live in handler.go
// Register() via DecodeStatusBroadcast + FormatStatusBroadcast.

import (
	"fmt"
	"scalefx/engine"
	"scalefx/protocol"
	hfxp "scalefx/protocol/hubfx"
	"strings"
)

// parseSlaveList parses SLAVE_LIST_RESP.
func (h *Handler) parseSlaveList(payload []byte) {
	if len(payload) < 1 {
		h.E.Out.Println("  (empty slave list)")
		return
	}
	count := payload[0]
	h.E.Out.Printf("  ── Slave Controllers (%d) ──\n", count)
	if count == 0 {
		h.E.Out.Println("  (no slaves registered)")
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

		typeName := hfxp.SlaveTypeName(stype)
		statusColor := engine.ColorRed
		statusText := "disconnected"
		if ready != 0 {
			statusColor = engine.ColorGreen
			statusText = "ready"
		} else if connected != 0 {
			statusColor = engine.ColorYellow
			statusText = "connected"
		}
		displayName := ""
		if name != "" {
			displayName = fmt.Sprintf(" (%s)", name)
		}
		h.E.Out.Printf("  [%d] %s%s: %s\n", i, typeName, displayName, h.E.Out.C(statusColor, statusText))
	}
}

// parseAudioStatus parses AUDIO_STATUS_RESP (v3/v4 extended format).
func (h *Handler) parseAudioStatus(payload []byte) {
	if len(payload) < 7 {
		h.E.Out.Println("  (audio status too short)")
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
	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, "Audio Mixer Status"))
	initStr := h.E.Out.C(engine.ColorRed, "no")
	if initialized {
		initStr = h.E.Out.C(engine.ColorGreen, "yes")
	}
	i2sStr := h.E.Out.C(engine.ColorRed, "stopped")
	if i2sRunning {
		i2sStr = h.E.Out.C(engine.ColorGreen, "running")
	}
	h.E.Out.Printf("    Initialized: %s\n", initStr)
	h.E.Out.Printf("    I2S:         %s (%dHz / %dbit)\n", i2sStr, sampleRate, bitDepth)
	if hasCodec {
		h.E.Out.Printf("    Codec:       %s\n", codecName)
	} else {
		h.E.Out.Printf("    Codec:       %s\n", h.E.Out.C(engine.ColorYellow, "none (I2S only)"))
	}
	h.E.Out.Printf("    Max Ch:      %d\n", maxChannels)
	h.E.Out.Printf("    Master Vol:  %d%%\n", masterVol)

	if hasRingStats {
		underrunStr := h.E.Out.C(engine.ColorGreen, "0")
		if underruns > 0 {
			underrunStr = h.E.Out.C(engine.ColorRed, fmt.Sprintf("%d", underruns))
		}
		fillColor := engine.ColorRed
		if ringFillPct >= 50 {
			fillColor = engine.ColorGreen
		} else if ringFillPct >= 25 {
			fillColor = engine.ColorYellow
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
		h.E.Out.Printf("    Ring Buf:    %s (%d%s frames%s)\n",
			h.E.Out.C(fillColor, fmt.Sprintf("%d%%", ringFillPct)), ringAvailRead, ringCapStr, ringMs)
		h.E.Out.Printf("    Underruns:   %s\n", underrunStr)
		h.E.Out.Printf("    Consumer:    %d loops, %d frames written to I2S\n", consumeLoops, consumeFrames)
	}
	if hasBufferCaps && wavBufCapacity > 0 {
		wavMs := uint32(0)
		if sampleRate > 0 {
			wavMs = uint32(wavBufCapacity) * 1000 / uint32(sampleRate)
		}
		h.E.Out.Printf("    WAV Buf:     %d frames/ch (%dms, %dKB total)\n", wavBufCapacity, wavMs, uint32(wavBufCapacity)*8*2*4/1024)
	}

	if pos >= len(payload) {
		h.E.Out.Printf("    %s\n", h.E.Out.C(engine.ColorYellow, "No channel data"))
		return
	}

	activeMask := payload[pos]; pos++
	if activeMask == 0 {
		h.E.Out.Printf("    %s\n", h.E.Out.C(engine.ColorYellow, "No active channels"))
		return
	}

	activeCount := 0
	for b := activeMask; b != 0; b >>= 1 {
		activeCount += int(b & 1)
	}
	h.E.Out.Printf("    Active:      %d channel(s) (mask: 0b%08b)\n", activeCount, activeMask)
	h.E.Out.Println("")

	outputNames := map[byte]string{hfxp.AudioOutputCh1: "ch1", hfxp.AudioOutputCh2: "ch2", hfxp.AudioOutputAll: "all"}

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

		status := h.E.Out.C(engine.ColorYellow, "- queued")
		if playing {
			status = h.E.Out.C(engine.ColorGreen, "> playing")
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
			bufColor := engine.ColorRed
			if wavBufFill >= 80 {
				bufColor = engine.ColorGreen
			} else if wavBufFill >= 40 {
				bufColor = engine.ColorYellow
			}
			bufStr = fmt.Sprintf(" buf=%s", h.E.Out.C(bufColor, fmt.Sprintf("%d%%", wavBufFill)))
		}

		h.E.Out.Printf("    ch%d: %s vol=%d%% %s%s%s%s%s\n", ch, status, vol, outName, loopStr, remainingStr, queueStr, bufStr)
		if fname != "" {
			h.E.Out.Printf("          file: %s\n", fname)
		}
		if wavRate > 0 {
			chStr := "mono"
			if wavCh == 2 {
				chStr = "stereo"
			}
			h.E.Out.Printf("          wav:  %dHz/%dbit/%s\n", wavRate, wavBits, chStr)
		}
	}
	h.E.Out.Println("")
}

// parseEngineStatus parses ENGINE_STATUS_RESP.
func (h *Handler) parseEngineStatus(payload []byte) {
	if len(payload) < 3 {
		h.E.Out.Println("  (engine status too short)")
		return
	}
	state := payload[0]
	toggle := payload[1] != 0
	active := payload[2] != 0

	stateName := hfxp.EngineStateName(state)
	stateColor := engine.ColorRed
	icon := "-"
	switch state {
	case 0:
		stateColor = engine.ColorRed; icon = "-"
	case 1:
		stateColor = engine.ColorYellow; icon = "*"
	case 2:
		stateColor = engine.ColorGreen; icon = ">"
	case 3:
		stateColor = engine.ColorYellow; icon = "~"
	}

	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, "Engine FX Status"))
	h.E.Out.Printf("    State:    %s %s\n", icon, h.E.Out.C(stateColor, stateName))
	h.E.Out.Printf("    Toggle:   %s\n", map[bool]string{true: "engaged", false: "disengaged"}[toggle])
	h.E.Out.Printf("    Active:   %s\n", map[bool]string{true: "yes", false: "no"}[active])
	h.E.Out.Println("")
}

// parseSdStatus parses SD_STATUS_RESP.
func (h *Handler) parseSdStatus(payload []byte) {
	if len(payload) < 1 {
		h.E.Out.Println("  (SD status too short)")
		return
	}
	initialized := payload[0] != 0

	cardTypes := map[byte]string{0: "NONE", 1: "MMC", 2: "SD", 3: "SDHC", 4: "UNKNOWN"}
	busModes := map[byte]string{0: "SPI", 1: "SDIO 1-bit", 2: "SDIO 4-bit"}

	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, "SD Card Status"))
	if initialized {
		h.E.Out.Printf("    Status: %s\n", h.E.Out.C(engine.ColorGreen, "initialized"))
		if len(payload) >= 14 {
			cardSize := protocol.ReadU32LE(payload, 1)
			totalSpace := protocol.ReadU32LE(payload, 5)
			freeSpace := protocol.ReadU32LE(payload, 9)
			fatType := payload[13]
			h.E.Out.Printf("    Card:   %d MB\n", cardSize)
			h.E.Out.Printf("    Total:  %d MB\n", totalSpace)
			h.E.Out.Printf("    Free:   %d MB\n", freeSpace)
			if fatType > 0 {
				h.E.Out.Printf("    FAT:    FAT%d\n", fatType)
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
			h.E.Out.Printf("    Type:   %s\n", typeName)
			h.E.Out.Printf("    Bus:    %s\n", busName)
			h.E.Out.Printf("    Used:   %d MB\n", usedSpace)
		}
	} else {
		h.E.Out.Printf("    Status: %s\n", h.E.Out.C(engine.ColorRed, "not initialized"))
		h.E.Out.Printf("    %s\n", h.E.Out.C(engine.ColorYellow, "Use 'sd.init' to remount"))
	}
	h.E.Out.Println("")
}

// parseFlashStatus parses Flash status response.
func (h *Handler) parseFlashStatus(payload []byte) {
	if len(payload) < 1 {
		h.E.Out.Println("  (flash status too short)")
		return
	}
	initialized := payload[0] != 0
	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, "Flash Status"))
	if initialized && len(payload) >= 13 {
		total := protocol.ReadU32LE(payload, 1)
		used := protocol.ReadU32LE(payload, 5)
		free := protocol.ReadU32LE(payload, 9)
		h.E.Out.Printf("    Status: %s\n", h.E.Out.C(engine.ColorGreen, "initialized"))
		h.E.Out.Printf("    Total:  %d bytes (%d KB)\n", total, total/1024)
		h.E.Out.Printf("    Used:   %d bytes (%d KB)\n", used, used/1024)
		h.E.Out.Printf("    Free:   %d bytes (%d KB)\n", free, free/1024)
	} else {
		h.E.Out.Printf("    Status: %s\n", h.E.Out.C(engine.ColorRed, "not initialized"))
	}
	h.E.Out.Println("")
}

// parseUsbDevices parses USB_DEVICES_RESP.
func (h *Handler) parseUsbDevices(payload []byte) {
	if len(payload) < 4 {
		h.E.Out.Println("  (USB devices too short)")
		return
	}

	pos := 0
	initialized := payload[pos] != 0; pos++
	taskRunning := payload[pos] != 0; pos++
	backendLen := int(payload[pos]); pos++

	if pos+backendLen > len(payload) {
		h.E.Out.Println("  (malformed USB response)")
		return
	}
	backend := string(payload[pos : pos+backendLen]); pos += backendLen
	if pos >= len(payload) {
		h.E.Out.Println("  (malformed USB response)")
		return
	}
	deviceCount := int(payload[pos]); pos++

	initStr := h.E.Out.C(engine.ColorRed, "not initialized")
	if initialized {
		initStr = h.E.Out.C(engine.ColorGreen, "initialized")
	}
	taskStr := h.E.Out.C(engine.ColorRed, "stopped")
	if taskRunning {
		taskStr = h.E.Out.C(engine.ColorGreen, "running")
	}

	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, fmt.Sprintf("USB Host (%s)", backend)))
	h.E.Out.Printf("    Status: %s, Task: %s\n", initStr, taskStr)
	h.E.Out.Printf("    CDC Devices: %d\n", deviceCount)

	if deviceCount == 0 {
		h.E.Out.Printf("    %s\n", h.E.Out.C(engine.ColorYellow, "(no USB devices)"))
		h.E.Out.Println("")
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
		stateColor := engine.ColorRed
		if state == 3 {
			stateColor = engine.ColorGreen
		} else if state >= 1 {
			stateColor = engine.ColorYellow
		}
		slaveText := ""
		if slaveType > 0 {
			slaveText = " -> " + hfxp.SlaveTypeName(slaveType)
		}
		h.E.Out.Printf("    [%d] addr=%d VID=%04X PID=%04X %s%s\n",
			i, addr, vid, pid, h.E.Out.C(stateColor, stateText), slaveText)
	}
	h.E.Out.Println("")
}

// parseCodecStatus parses CODEC_STATUS_RESP.
func (h *Handler) parseCodecStatus(payload []byte) {
	if len(payload) < 11 {
		h.E.Out.Println("  (codec status too short)")
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
		faultStr = h.E.Out.C(engine.ColorYellow, "read error")
	} else if faultStatus == 0 {
		faultStr = h.E.Out.C(engine.ColorGreen, "none")
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
		faultStr = h.E.Out.C(engine.ColorRed, fmt.Sprintf("%s (0x%02X)", strings.Join(bits, ", "), faultStatus))
	}

	initColor := engine.ColorRed
	if initialized {
		initColor = engine.ColorGreen
	}
	i2cColor := engine.ColorRed
	if i2cOK {
		i2cColor = engine.ColorGreen
	}

	display := codecName
	if display == "" {
		display = codecTypeStr
	}

	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, "═══ Codec Status ═══"))
	h.E.Out.Printf("    Model:          %s\n", display)
	h.E.Out.Printf("    Type:           %s\n", codecTypeStr)
	h.E.Out.Printf("    Initialized:    %s\n", h.E.Out.C(initColor, map[bool]string{true: "Yes", false: "No"}[initialized]))
	h.E.Out.Printf("    I2C Connected:  %s\n", h.E.Out.C(i2cColor, map[bool]string{true: "Yes", false: "No"}[i2cOK]))
	if sdaPin != 0xFF {
		h.E.Out.Printf("    I2C Pins:       SDA=GPIO%d, SCL=GPIO%d\n", sdaPin, sclPin)
	}
	if codecType == 1 { // TAS5825M-specific
		muteColor := engine.ColorGreen
		if muted {
			muteColor = engine.ColorYellow
		}
		h.E.Out.Printf("    Supply:         %s\n", supplyStr)
		h.E.Out.Printf("    Muted:          %s\n", h.E.Out.C(muteColor, map[bool]string{true: "Yes", false: "No"}[muted]))
		h.E.Out.Printf("    Digital Volume: 0x%02X (%s)\n", digitalVol, volDbStr)
		h.E.Out.Printf("    Device Ctrl:    0x%02X (%s)\n", deviceCtrl, ctrlStr)
		h.E.Out.Printf("    Fault Status:   %s\n", faultStr)
	}
	h.E.Out.Println("")
}

// parseSlaveInfo parses SLAVE_INFO_RESP payload.
func (h *Handler) parseSlaveInfo(payload []byte) {
	if len(payload) < 3 {
		h.E.Out.Println("  (slave info too short)")
		return
	}
	pos := 0
	stype := payload[pos]; pos++
	ready := payload[pos] != 0; pos++
	connected := payload[pos] != 0; pos++

	typeName := hfxp.SlaveTypeName(stype)
	statusColor := engine.ColorRed
	statusText := "disconnected"
	if ready {
		statusColor = engine.ColorGreen
		statusText = "ready"
	} else if connected {
		statusColor = engine.ColorYellow
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

	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorYellow, typeName+" Board Info"))
	h.E.Out.Printf("    Status:    %s\n", h.E.Out.C(statusColor, statusText))
	if name != "" {
		h.E.Out.Printf("    Name:      %s\n", name)
	}
	if version != "" {
		h.E.Out.Printf("    Version:   %s (build %d)\n", version, buildNum)
	}
	if platform != "" {
		h.E.Out.Printf("    Platform:  %s\n", platform)
	}
	if cpuMHz > 0 {
		h.E.Out.Printf("    CPU:       %d MHz\n", cpuMHz)
	}
	if freeRAM > 0 {
		h.E.Out.Printf("    Free RAM:  %d bytes\n", freeRAM)
	}
	h.E.Out.Println("")
}
