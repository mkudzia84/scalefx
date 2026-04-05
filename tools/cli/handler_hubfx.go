package main

// ScaleFX CLI - HubFX Command Handler
// Commands for HubFX master hub controller (ESP32-S3).
// Aligned with Python CLI: tests/cli/handlers/hubfx.py

import (
	"fmt"
	"os"
	"path/filepath"
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
			"file.cat":      {c.cmdHubFileCat, "file.cat <sd|flash> <path>", "Display file contents", true},
			"file.download": {c.cmdHubFileDownload, "file.download <sd|flash> <remote> <local>", "Download file", true},
			"file.upload":   {c.cmdHubFileUpload, "file.upload <sd|flash> <local> <remote> [--stream]", "Upload file", true},
			"file.cancel":   {c.cmdHubFileCancel, "file.cancel", "Cancel active upload", true},
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
	c.query(NewHubFxApi(c.conn).SlaveList(), ParseSlaveList)
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
	c.ack(NewHubFxApi(c.conn).SlaveInit(slaveType), fmt.Sprintf("%s slave initialized", SlaveTypeName(slaveType)))
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
	c.query(NewHubFxApi(c.conn).SlaveInfo(slaveType), ParseSlaveInfo)
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
	c.ack(NewHubFxApi(c.conn).AudioPlay(ch, vol, output, loopMode, loopCount, path),
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
	c.ack(NewHubFxApi(c.conn).AudioStop(ch), fmt.Sprintf("Audio stop %s", target))
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
	c.ack(NewHubFxApi(c.conn).AudioVolume(ch, vol), fmt.Sprintf("Volume %s → %d%%", target, vol))
}

func (c *CLI) cmdHubAudioFade(args []string) {
	if !requireArgs(args, 1, "audio.fade <ch>") {
		return
	}
	c.ack(NewHubFxApi(c.conn).AudioFade(byte(atoi(args[0]))), fmt.Sprintf("Fade out ch%s", args[0]))
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
	c.ack(NewHubFxApi(c.conn).AudioQueue(ch, vol, loopCount, loopBehavior, path),
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
	c.ack(NewHubFxApi(c.conn).AudioQueueClear(ch), fmt.Sprintf("Queue cleared %s", target))
}

func (c *CLI) cmdHubAudioStatus(_ []string) {
	c.query(NewHubFxApi(c.conn).AudioStatus(), ParseAudioStatus)
}

func (c *CLI) cmdHubCodecStatus(_ []string) {
	c.query(NewHubFxApi(c.conn).CodecStatus(), ParseCodecStatus)
}

// ─── Engine Commands ───

func (c *CLI) cmdHubEngineStart(_ []string) { c.ack(NewHubFxApi(c.conn).EngineStart(), "Engine start") }
func (c *CLI) cmdHubEngineStop(_ []string)  { c.ack(NewHubFxApi(c.conn).EngineStop(), "Engine stop") }

func (c *CLI) cmdHubEngineStatus(_ []string) {
	c.query(NewHubFxApi(c.conn).EngineStatus(), ParseEngineStatus)
}

// ─── Config Commands ───

func (c *CLI) cmdHubConfigReload(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	c.ack(NewHubFxApi(c.conn).ConfigReload(path), "Config reload")
}

func (c *CLI) cmdHubConfigStatus(_ []string) {
	c.query(NewHubFxApi(c.conn).ConfigStatus(), ParseConfigStatus)
}

func (c *CLI) cmdHubConfigSave(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	c.ack(NewHubFxApi(c.conn).ConfigSave(path), "Config save")
}

// ─── Storage Commands ───

func (c *CLI) cmdHubSdInit(_ []string) { c.ack(NewHubFxApi(c.conn).SdInit(), "SD card remounted") }

func (c *CLI) cmdHubSdStatus(_ []string) {
	c.query(NewHubFxApi(c.conn).SdStatus(), ParseSdStatus)
}

func (c *CLI) cmdHubFlashStatus(_ []string) {
	c.query(NewHubFxApi(c.conn).FlashStatus(), ParseFlashStatus)
}

func (c *CLI) cmdHubUsbDevices(_ []string) {
	c.query(NewHubFxApi(c.conn).UsbDevices(), ParseUsbDevices)
}

func (c *CLI) cmdHubUsbReset(_ []string) { c.ack(NewHubFxApi(c.conn).UsbReset(), "USB bus reset") }

// ─── File Operations (storage target pattern) ───

func (c *CLI) cmdHubFileList(args []string) {
	if !c.requireConn() {
		return
	}
	target, path := parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	text, err := NewFileApi(c.conn).List(target, path)
	if err != nil {
		PrintError("%v", err)
		return
	}
	formatListing(text, fmt.Sprintf("%s:%s", storageTargetName(target), path))
}

func (c *CLI) cmdHubFileDelete(args []string) {
	target, path := parseStorageArgs(args, "")
	if target == 255 || path == "" {
		PrintError("Usage: file.delete <sd|flash> <path>")
		return
	}
	c.ack(NewFileApi(c.conn).Delete(target, path), fmt.Sprintf("Delete %s:%s", storageTargetName(target), path))
}

func (c *CLI) cmdHubFileMkdir(args []string) {
	target, path := parseStorageArgs(args, "")
	if target == 255 || path == "" {
		PrintError("Usage: file.mkdir <sd|flash> <path>")
		return
	}
	c.ack(NewFileApi(c.conn).Mkdir(target, path), fmt.Sprintf("Mkdir %s:%s", storageTargetName(target), path))
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
	r := NewFileApi(c.conn).Info(target, path)
	if !r.OK {
		PrintError("%s", r.Error)
		return
	}
	if len(r.Response.Payload) >= 6 {
		exists := r.Response.Payload[0]
		isDir := r.Response.Payload[1]
		size := ReadU32LE(r.Response.Payload, 2)
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
		PrintError("Response too short")
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
	text, err := NewFileApi(c.conn).Tree(target, path)
	if err != nil {
		PrintError("%v", err)
		return
	}
	renderTree(text, fmt.Sprintf("%s:%s", storageTargetName(target), path))
}

func (c *CLI) cmdHubFileCat(args []string) {
	if !c.requireConn() {
		return
	}
	target, path := parseStorageArgs(args, "")
	if target == 255 || path == "" {
		PrintError("Usage: file.cat <sd|flash> <path>")
		return
	}
	PrintInfo("Reading %s:%s ...", storageTargetName(target), path)
	text, err := NewFileApi(c.conn).Cat(target, path)
	if err != nil {
		PrintError("%v", err)
		return
	}
	fmt.Println()
	fmt.Print(text)
	fmt.Printf("\n    (%d bytes)\n", len(text))
}

func (c *CLI) cmdHubFileDownload(args []string) {
	if !c.requireConn() {
		return
	}
	if len(args) < 3 {
		PrintError("Usage: file.download <sd|flash> <remote> <local>")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = StorageTargetSD
	case "flash":
		target = StorageTargetFlash
	default:
		PrintError("Storage target must be 'sd' or 'flash'")
		return
	}
	remotePath := args[1]
	localPath := args[2]

	PrintInfo("Downloading %s:%s ...", storageTargetName(target), remotePath)
	result, err := NewFileApi(c.conn).Download(target, remotePath, 60*time.Second)
	if err != nil {
		PrintError("%v", err)
		return
	}

	if dir := filepath.Dir(localPath); dir != "" && dir != "." {
		os.MkdirAll(dir, 0755)
	}
	if err := os.WriteFile(localPath, result.Data, 0644); err != nil {
		PrintError("Failed to write local file: %v", err)
		return
	}
	PrintOK("Downloaded %d bytes → %s", len(result.Data), localPath)
}

func (c *CLI) cmdHubFileUpload(args []string) {
	if !c.requireConn() {
		return
	}
	if len(args) < 3 {
		PrintError("Usage: file.upload <sd|flash> <local> <remote> [--stream]")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = StorageTargetSD
	case "flash":
		target = StorageTargetFlash
	default:
		PrintError("Storage target must be 'sd' or 'flash'")
		return
	}
	localPath := args[1]
	remotePath := args[2]

	// Parse upload mode from flags
	mode := UploadSync
	for _, a := range args[3:] {
		switch strings.ToLower(a) {
		case "--stream":
			mode = UploadStream
		}
	}

	fileData, err := os.ReadFile(localPath)
	if err != nil {
		PrintError("Cannot read local file: %v", err)
		return
	}

	fileSize := len(fileData)
	modeName := "sync"
	switch mode {
	case UploadStream:
		modeName = "stream"
	}
	PrintInfo("Uploading %s (%d bytes) → %s:%s [%s]", localPath, fileSize,
		storageTargetName(target), remotePath, modeName)

	uploadStart := time.Now()
	result := NewFileApi(c.conn).Upload(target, remotePath, fileData, mode,
		func(sent, total int) {
			fmt.Printf("\r%s  ", FormatProgressBar(sent, total, uploadStart, 30))
		})
	fmt.Println()

	if !result.OK {
		PrintError("Upload failed: %s", result.Error)
		return
	}

	PrintOK("Uploaded %d bytes in %.1fs (%.1f KB/s)", result.BytesTransferred,
		result.Elapsed.Seconds(), result.SpeedKBs)

	if result.LocalMD5 != "" {
		if result.MD5Match {
			PrintOK("MD5 verified: %s", result.RemoteMD5)
		} else {
			PrintError("MD5 MISMATCH! local=%s remote=%s", result.LocalMD5, result.RemoteMD5)
		}
	}
}

func (c *CLI) cmdHubFileCancel(_ []string) {
	c.ack(NewFileApi(c.conn).CancelUpload(), "Upload cancelled")
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
