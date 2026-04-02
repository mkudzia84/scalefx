package main

// ScaleFX CLI - HubFX Command Handler
// Commands for HubFX master hub controller (ESP32-S3).
// Aligned with Python CLI: tests/cli/handlers/hubfx.py

import (
	"fmt"
	"strings"
	"time"
)

func (c *CLI) hubfxCommands() *cmdGroup {
	return &cmdGroup{
		Name:       "HubFX",
		Controller: CtrlHubFX,
		Color:      colorCyan,
		Commands: map[string]cmdEntry{
			"slaves":        {c.cmdHubSlaves, "slaves", "List connected slaves", true},
			"slave.init":    {c.cmdHubSlaveInit, "slave.init <type>", "Init slave (gunfx|lightfx|gearcontrol|1|2|3)", true},
			"slave.info":    {c.cmdHubSlaveInfo, "slave.info <type>", "Query slave info", true},
			"audio.play":    {c.cmdHubAudioPlay, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]", "Play audio", true},
			"audio.stop":    {c.cmdHubAudioStop, "audio.stop [ch|all]", "Stop audio (default: all)", true},
			"audio.volume":  {c.cmdHubAudioVol, "audio.volume <ch|master> <vol>", "Set volume (0-100)", true},
			"audio.fade":    {c.cmdHubAudioFade, "audio.fade <ch>", "Fade out audio", true},
			"audio.queue":   {c.cmdHubAudioQueue, "audio.queue <ch> <path> [vol] [loop N]", "Queue sound after current", true},
			"audio.clear":   {c.cmdHubAudioClear, "audio.clear [ch|all]", "Clear audio queue", true},
			"audio.status":  {c.cmdHubAudioStatus, "audio.status", "Audio mixer status", true},
			"codec.status":  {c.cmdHubCodecStatus, "codec.status", "DAC codec status", true},
			"engine.start":  {c.cmdHubEngineStart, "engine.start", "Start engine effect", true},
			"engine.stop":   {c.cmdHubEngineStop, "engine.stop", "Stop engine effect", true},
			"engine.status": {c.cmdHubEngineStatus, "engine.status", "Engine status", true},
			"config.reload": {c.cmdHubConfigReload, "config.reload [path]", "Reload config from SD", true},
			"config.status": {c.cmdHubConfigStatus, "config.status", "Config status", true},
			"config.save":   {c.cmdHubConfigSave, "config.save [path]", "Save config to SD", true},
			"sd.init":       {c.cmdHubSdInit, "sd.init", "Initialize SD card", true},
			"sd.status":     {c.cmdHubSdStatus, "sd.status", "SD card status", true},
			"flash.status":  {c.cmdHubFlashStatus, "flash.status", "Flash status", true},
			"file.list":     {c.cmdHubFileList, "file.list <sd|flash> [path]", "List files", true},
			"file.delete":   {c.cmdHubFileDelete, "file.delete <sd|flash> <path>", "Delete file", true},
			"file.mkdir":    {c.cmdHubFileMkdir, "file.mkdir <sd|flash> <path>", "Create directory", true},
			"file.info":     {c.cmdHubFileInfo, "file.info <sd|flash> <path>", "File info", true},
			"file.tree":     {c.cmdHubFileTree, "file.tree <sd|flash> [path]", "Tree view", true},
			"usb.devices":   {c.cmdHubUsbDevices, "usb.devices", "List USB devices", true},
			"usb.reset":     {c.cmdHubUsbReset, "usb.reset", "Reset USB bus", true},
		},
	}
}

// ─── Slave Type Resolver ───

// parseSlaveType resolves a slave type from name or numeric string.
func parseSlaveType(s string) (byte, bool) {
	typeMap := map[string]byte{
		"gunfx": SlaveGunFX, "1": SlaveGunFX,
		"lightfx": SlaveLightFX, "2": SlaveLightFX,
		"gearcontrol": SlaveGearControl, "3": SlaveGearControl,
	}
	if v, ok := typeMap[strings.ToLower(s)]; ok {
		return v, true
	}
	return 0, false
}

// ─── Slave Commands ───

func (c *CLI) cmdHubSlaves(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubSlaveList())
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubSLAVE_LIST_RESP {
		ParseSlaveList(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "Slave list requested")
	}
}

func (c *CLI) cmdHubSlaveInit(args []string) {
	if !requireArgs(args, 1, "slave.init <type> (gunfx|lightfx|gearcontrol or 1|2|3)") {
		return
	}
	slaveType, ok := parseSlaveType(args[0])
	if !ok {
		PrintError("Unknown slave type: %s", args[0])
		PrintInfo("Valid types: gunfx (1), lightfx (2), gearcontrol (3)")
		return
	}
	c.sendACK(CmdHubSlaveInit(slaveType), fmt.Sprintf("%s slave initialized", SlaveTypeName(slaveType)))
}

func (c *CLI) cmdHubSlaveInfo(args []string) {
	if !requireArgs(args, 1, "slave.info <type> (gunfx|lightfx|gearcontrol or 1|2|3)") || !c.requireConn() {
		return
	}
	slaveType, ok := parseSlaveType(args[0])
	if !ok {
		PrintError("Unknown slave type: %s", args[0])
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubSlaveInfo(slaveType))
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubSLAVE_INFO_RESP {
		ParseSlaveInfo(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "Slave info requested")
	}
}

// ─── Audio Commands ───

func (c *CLI) cmdHubAudioPlay(args []string) {
	if !requireArgs(args, 2, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]") {
		return
	}
	ch := byte(atoi(args[0]))
	path := args[1]
	vol := byte(100)
	output := byte(AudioOutputALL)
	loopMode := byte(AudioLoopNone)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		switch arg {
		case "ch1":
			output = AudioOutputCH1
		case "ch2":
			output = AudioOutputCH2
		case "all":
			output = AudioOutputALL
		case "loop":
			if i+1 < len(args) {
				i++
				if strings.ToLower(args[i]) == "inf" {
					loopMode = AudioLoopInfinite
				} else {
					loopMode = AudioLoopFinite
					loopCount = uint16(atoi(args[i]))
				}
			} else {
				loopMode = AudioLoopInfinite
			}
		default:
			if n := atoi(arg); n > 0 {
				vol = byte(n)
			}
		}
		i++
	}

	outputName := ""
	switch output {
	case AudioOutputCH1:
		outputName = " [CH1]"
	case AudioOutputCH2:
		outputName = " [CH2]"
	}
	loopStr := ""
	if loopMode == AudioLoopInfinite {
		loopStr = " (loop inf)"
	} else if loopMode == AudioLoopFinite {
		loopStr = fmt.Sprintf(" (loop x%d)", loopCount)
	}
	c.sendACK(CmdHubAudioPlay(ch, vol, output, loopMode, loopCount, path),
		fmt.Sprintf("Play ch%d: %s vol=%d%%%s%s", ch, path, vol, outputName, loopStr))
}

func (c *CLI) cmdHubAudioStop(args []string) {
	ch := byte(AudioChAll) // default: all
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = AudioChAll
		} else {
			ch = byte(atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	c.sendACK(CmdHubAudioStop(ch), fmt.Sprintf("Audio stop %s", target))
}

func (c *CLI) cmdHubAudioVol(args []string) {
	if !requireArgs(args, 2, "audio.volume <ch|master> <volume>") {
		return
	}
	ch := byte(0)
	if strings.ToLower(args[0]) == "master" {
		ch = AudioChAll
	} else {
		ch = byte(atoi(args[0]))
	}
	vol := byte(atoi(args[1]))
	target := "master"
	if ch != AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	c.sendACK(CmdHubAudioVolume(ch, vol), fmt.Sprintf("Volume %s → %d%%", target, vol))
}

func (c *CLI) cmdHubAudioFade(args []string) {
	if !requireArgs(args, 1, "audio.fade <ch>") {
		return
	}
	c.sendACK(CmdHubAudioFade(byte(atoi(args[0]))), fmt.Sprintf("Fade out ch%s", args[0]))
}

func (c *CLI) cmdHubAudioQueue(args []string) {
	if !requireArgs(args, 2, "audio.queue <ch> <path> [vol] [loop N]") {
		return
	}
	ch := byte(atoi(args[0]))
	path := args[1]
	vol := byte(100)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		if arg == "loop" && i+1 < len(args) {
			i++
			loopCount = uint16(atoi(args[i]))
		} else if n := atoi(arg); n > 0 {
			vol = byte(n)
		}
		i++
	}

	// Queue uses FINISH_LOOP behavior (0)
	loopBehavior := byte(AudioQueueFinishLoop)
	c.sendACK(CmdHubAudioQueue(ch, vol, loopCount, loopBehavior, path),
		fmt.Sprintf("Queue ch%d: %s vol=%d%%", ch, path, vol))
}

func (c *CLI) cmdHubAudioClear(args []string) {
	ch := byte(AudioChAll) // default: all
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = AudioChAll
		} else {
			ch = byte(atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	c.sendACK(CmdHubAudioQueueClear(ch), fmt.Sprintf("Queue cleared %s", target))
}

func (c *CLI) cmdHubAudioStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubAudioStatusReq())
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubAUDIO_STATUS_RESP {
		ParseAudioStatus(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "Audio status requested")
	}
}

func (c *CLI) cmdHubCodecStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubCodecStatusReq())
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubCODEC_STATUS_RESP {
		ParseCodecStatus(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "Codec status requested")
	}
}

// ─── Engine Commands ───

func (c *CLI) cmdHubEngineStart(_ []string) { c.sendACK(CmdHubEngineStart(), "Engine start") }
func (c *CLI) cmdHubEngineStop(_ []string)  { c.sendACK(CmdHubEngineStop(), "Engine stop") }

func (c *CLI) cmdHubEngineStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubEngineStatusReq())
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubENGINE_STATUS_RESP {
		ParseEngineStatus(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "Engine status requested")
	}
}

// ─── Config Commands ───

func (c *CLI) cmdHubConfigReload(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	c.sendACK(CmdHubConfigReload(path), "Config reload")
}

func (c *CLI) cmdHubConfigStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubConfigStatus())
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubCONFIG_STATUS_RESP {
		ParseConfigStatus(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "Config status requested")
	}
}

func (c *CLI) cmdHubConfigSave(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	c.sendACK(CmdHubConfigSave(path), "Config save")
}

// ─── Storage Commands ───

func (c *CLI) cmdHubSdInit(_ []string) { c.sendACK(CmdHubSDInit(0), "SD card remounted") }

func (c *CLI) cmdHubSdStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubSDStatusReq())
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubSD_STATUS_RESP {
		ParseSdStatus(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "SD status requested")
	}
}

func (c *CLI) cmdHubFlashStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubFlashStatusReq())
	if err != nil {
		PrintError("%v", err)
		return
	}
	// Flash status comes back as FLASH_STATUS_REQ (repurposed as both request and response)
	if resp.PacketType == HubFLASH_STATUS_REQ {
		ParseFlashStatus(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "Flash status requested")
	}
}

func (c *CLI) cmdHubUsbDevices(_ []string) {
	if !c.requireConn() {
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubUSBDevicesReq())
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.PacketType == HubUSB_DEVICES_RESP {
		ParseUsbDevices(resp.Payload)
	} else if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
	} else {
		PrintACKResult(resp, "USB devices requested")
	}
}

func (c *CLI) cmdHubUsbReset(_ []string) { c.sendACK(CmdHubUSBResetBus(), "USB bus reset") }

// ─── File Operations (storage target pattern) ───

func (c *CLI) cmdHubFileList(args []string) {
	if !c.requireConn() {
		return
	}
	target, path := parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	result, err := c.conn.SendAndReceiveStream(CmdHubFileList(path, target), 10*time.Second)
	if err != nil {
		PrintError("%v", err)
		return
	}
	text := string(result.Data)
	formatListing(text, fmt.Sprintf("%s:%s", storageTargetName(target), path))
}

func (c *CLI) cmdHubFileDelete(args []string) {
	target, path := parseStorageArgs(args, "")
	if target == 255 || path == "" {
		PrintError("Usage: file.delete <sd|flash> <path>")
		return
	}
	c.sendACK(CmdHubFileDelete(path, target), fmt.Sprintf("Delete %s:%s", storageTargetName(target), path))
}

func (c *CLI) cmdHubFileMkdir(args []string) {
	target, path := parseStorageArgs(args, "")
	if target == 255 || path == "" {
		PrintError("Usage: file.mkdir <sd|flash> <path>")
		return
	}
	c.sendACK(CmdHubFileMkdir(path, target), fmt.Sprintf("Mkdir %s:%s", storageTargetName(target), path))
}

func (c *CLI) cmdHubFileInfo(args []string) {
	if !c.requireConn() {
		return
	}
	target, path := parseStorageArgs(args, "")
	if target == 255 || path == "" {
		PrintError("Usage: file.info <sd|flash> <path>")
		return
	}
	resp, err := c.conn.SendAndWait(CmdHubFileInfo(path, target))
	if err != nil {
		PrintError("%v", err)
		return
	}
	if resp.IsNACK() {
		PrintError("NACK: %s", resp.ErrorMessage())
		return
	}
	if resp.PacketType == HubFILE_INFO_RESP && len(resp.Payload) >= 6 {
		exists := resp.Payload[0]
		isDir := resp.Payload[1]
		size := ReadU32LE(resp.Payload, 2)
		display := fmt.Sprintf("%s:%s", storageTargetName(target), path)
		fmt.Printf("\n  %s%s%s\n", colorCyan, display, colorReset)
		if exists != 0 {
			kind := "file"
			if isDir != 0 {
				kind = "directory"
			}
			fmt.Printf("    Type: %s\n", kind)
			if isDir == 0 {
				fmt.Printf("    Size: %s (%d bytes)\n", formatSize(size), size)
			}
		} else {
			fmt.Printf("    %sNot found%s\n", colorRed, colorReset)
		}
		fmt.Println()
	} else {
		PrintError("Unexpected response: 0x%02X", resp.PacketType)
	}
}

func (c *CLI) cmdHubFileTree(args []string) {
	if !c.requireConn() {
		return
	}
	target, path := parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	result, err := c.conn.SendAndReceiveStream(CmdHubFileTree(path, target), 30*time.Second)
	if err != nil {
		PrintError("%v", err)
		return
	}
	text := string(result.Data)
	renderTree(text, fmt.Sprintf("%s:%s", storageTargetName(target), path))
}

// ─── Storage Helpers ───

func parseStorageArgs(args []string, defaultPath string) (byte, string) {
	if !requireArgs(args, 1, "<sd|flash> [path]") {
		return 255, ""
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = StorageTargetSD
	case "flash":
		target = StorageTargetFlash
	default:
		PrintError("Storage target must be 'sd' or 'flash'")
		return 255, ""
	}
	path := defaultPath
	if len(args) > 1 {
		path = args[1]
	}
	return target, path
}

func storageTargetName(t byte) string {
	if t == StorageTargetSD {
		return "SD"
	}
	return "Flash"
}
