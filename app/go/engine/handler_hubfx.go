package engine

// ScaleFX Engine - HubFX Command Handler
// Commands for HubFX master hub controller (ESP32-S3).

import (
	"fmt"
	"os"
	"path/filepath"
	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"scalefx/protocol/hubfx"
	"strings"
	"time"
)

func (e *Engine) hubfxCommands() *CmdGroup {
	return &CmdGroup{
		Name:       "HubFX",
		Controller: core.CtrlHubFX,
		Color:      ColorCyan,
		Commands: map[string]CmdEntry{
			"slaves":        {e.cmdHubSlaves, "slaves", "List connected slaves", true},
			"slave.init":    {e.cmdHubSlaveInit, "slave.init <type>", "Init slave (gunfx|lightfx|gearcontrol|1|2|3)", true},
			"slave.info":    {e.cmdHubSlaveInfo, "slave.info <type>", "Query slave info", true},
			"audio.play":    {e.cmdHubAudioPlay, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]", "Play audio", true},
			"audio.stop":    {e.cmdHubAudioStop, "audio.stop [ch|all]", "Stop audio (default: all)", true},
			"audio.volume":  {e.cmdHubAudioVol, "audio.volume <ch|master> <vol>", "Set volume (0-100)", true},
			"audio.fade":    {e.cmdHubAudioFade, "audio.fade <ch>", "Fade out audio", true},
			"audio.queue":   {e.cmdHubAudioQueue, "audio.queue <ch> <path> [vol] [loop N]", "Queue sound after current", true},
			"audio.clear":   {e.cmdHubAudioClear, "audio.clear [ch|all]", "Clear audio queue", true},
			"audio.status":  {e.cmdHubAudioStatus, "audio.status", "Audio mixer status", true},
			"codec.status":  {e.cmdHubCodecStatus, "codec.status", "DAC codec status", true},
			"engine.start":  {e.cmdHubEngineStart, "engine.start", "Start engine effect", true},
			"engine.stop":   {e.cmdHubEngineStop, "engine.stop", "Stop engine effect", true},
			"engine.status": {e.cmdHubEngineStatus, "engine.status", "Engine status", true},
			"config.reload": {e.cmdHubConfigReload, "config.reload [path]", "Reload config from SD", true},
			"config.status": {e.cmdHubConfigStatus, "config.status", "Config status", true},
			"config.save":   {e.cmdHubConfigSave, "config.save [path]", "Save config to SD", true},
			"sd.init":       {e.cmdHubSdInit, "sd.init", "Initialize SD card", true},
			"sd.status":     {e.cmdHubSdStatus, "sd.status", "SD card status", true},
			"flash.status":  {e.cmdHubFlashStatus, "flash.status", "Flash status", true},
			"file.list":     {e.cmdHubFileList, "file.list <sd|flash> [path]", "List files", true},
			"file.delete":   {e.cmdHubFileDelete, "file.delete <sd|flash> <path>", "Delete file", true},
			"file.mkdir":    {e.cmdHubFileMkdir, "file.mkdir <sd|flash> <path>", "Create directory", true},
			"file.info":     {e.cmdHubFileInfo, "file.info <sd|flash> <path>", "File info", true},
			"file.tree":     {e.cmdHubFileTree, "file.tree <sd|flash> [path]", "Tree view", true},
			"file.cat":      {e.cmdHubFileCat, "file.cat <sd|flash> <path>", "Display file contents", true},
			"file.download": {e.cmdHubFileDownload, "file.download <sd|flash> <remote> <local>", "Download file", true},
			"file.upload":   {e.cmdHubFileUpload, "file.upload <sd|flash> <local> <remote> [--stream]", "Upload file", true},
			"file.cancel":   {e.cmdHubFileCancel, "file.cancel", "Cancel active upload", true},
			"usb.devices":   {e.cmdHubUsbDevices, "usb.devices", "List USB devices", true},
			"usb.reset":     {e.cmdHubUsbReset, "usb.reset", "Reset USB bus", true},
		},
	}
}

// ─── Slave Type Resolver ───

// ParseSlaveType resolves a slave type from name or numeric string.
func ParseSlaveType(s string) (byte, bool) {
	typeMap := map[string]byte{
		"gunfx": hubfx.SlaveTypeGunFx, "1": hubfx.SlaveTypeGunFx,
		"lightfx": hubfx.SlaveTypeLightFx, "2": hubfx.SlaveTypeLightFx,
		"gearcontrol": hubfx.SlaveTypeGearControl, "3": hubfx.SlaveTypeGearControl,
	}
	if v, ok := typeMap[strings.ToLower(s)]; ok {
		return v, true
	}
	return 0, false
}

// ─── Slave Commands ───

func (e *Engine) cmdHubSlaves(_ []string) {
	e.query(e.API.HubFx.SlaveList(), e.ParseSlaveList)
}

func (e *Engine) cmdHubSlaveInit(args []string) {
	if !e.requireArgs(args, 1, "slave.init <type> (gunfx|lightfx|gearcontrol or 1|2|3)") {
		return
	}
	slaveType, ok := ParseSlaveType(args[0])
	if !ok {
		e.Out.Error("Unknown slave type: %s", args[0])
		e.Out.Info("Valid types: gunfx (1), lightfx (2), gearcontrol (3)")
		return
	}
	e.ack(e.API.HubFx.SlaveInit(slaveType), fmt.Sprintf("%s slave initialized", hubfx.SlaveTypeName(slaveType)))
}

func (e *Engine) cmdHubSlaveInfo(args []string) {
	if !e.requireArgs(args, 1, "slave.info <type> (gunfx|lightfx|gearcontrol or 1|2|3)") || !e.requireConn() {
		return
	}
	slaveType, ok := ParseSlaveType(args[0])
	if !ok {
		e.Out.Error("Unknown slave type: %s", args[0])
		return
	}
	e.query(e.API.HubFx.SlaveInfo(slaveType), e.ParseSlaveInfo)
}

// ─── Audio Commands ───

func (e *Engine) cmdHubAudioPlay(args []string) {
	if !e.requireArgs(args, 2, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]") {
		return
	}
	ch := byte(Atoi(args[0]))
	path := args[1]
	vol := byte(100)
	output := byte(hubfx.AudioOutputAll)
	loopMode := byte(hubfx.AudioLoopNone)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		switch arg {
		case "ch1":
			output = hubfx.AudioOutputCh1
		case "ch2":
			output = hubfx.AudioOutputCh2
		case "all":
			output = hubfx.AudioOutputAll
		case "loop":
			if i+1 < len(args) {
				i++
				if strings.ToLower(args[i]) == "inf" {
					loopMode = hubfx.AudioLoopInfinite
				} else {
					loopMode = hubfx.AudioLoopFinite
					loopCount = uint16(Atoi(args[i]))
				}
			} else {
				loopMode = hubfx.AudioLoopInfinite
			}
		default:
			if n := Atoi(arg); n > 0 {
				vol = byte(n)
			}
		}
		i++
	}

	outputName := ""
	switch output {
	case hubfx.AudioOutputCh1:
		outputName = " [CH1]"
	case hubfx.AudioOutputCh2:
		outputName = " [CH2]"
	}
	loopStr := ""
	if loopMode == hubfx.AudioLoopInfinite {
		loopStr = " (loop inf)"
	} else if loopMode == hubfx.AudioLoopFinite {
		loopStr = fmt.Sprintf(" (loop x%d)", loopCount)
	}
	e.ack(e.API.HubFx.AudioPlay(ch, vol, output, loopMode, loopCount, path),
		fmt.Sprintf("Play ch%d: %s vol=%d%%%s%s", ch, path, vol, outputName, loopStr))
}

func (e *Engine) cmdHubAudioStop(args []string) {
	ch := byte(hubfx.AudioChAll) // default: all
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = hubfx.AudioChAll
		} else {
			ch = byte(Atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != hubfx.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	e.ack(e.API.HubFx.AudioStop(ch), fmt.Sprintf("Audio stop %s", target))
}

func (e *Engine) cmdHubAudioVol(args []string) {
	if !e.requireArgs(args, 2, "audio.volume <ch|master> <volume>") {
		return
	}
	ch := byte(0)
	if strings.ToLower(args[0]) == "master" {
		ch = hubfx.AudioChAll
	} else {
		ch = byte(Atoi(args[0]))
	}
	vol := byte(Atoi(args[1]))
	target := "master"
	if ch != hubfx.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	e.ack(e.API.HubFx.AudioVolume(ch, vol), fmt.Sprintf("Volume %s → %d%%", target, vol))
}

func (e *Engine) cmdHubAudioFade(args []string) {
	if !e.requireArgs(args, 1, "audio.fade <ch>") {
		return
	}
	e.ack(e.API.HubFx.AudioFade(byte(Atoi(args[0]))), fmt.Sprintf("Fade out ch%s", args[0]))
}

func (e *Engine) cmdHubAudioQueue(args []string) {
	if !e.requireArgs(args, 2, "audio.queue <ch> <path> [vol] [loop N]") {
		return
	}
	ch := byte(Atoi(args[0]))
	path := args[1]
	vol := byte(100)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		if arg == "loop" && i+1 < len(args) {
			i++
			loopCount = uint16(Atoi(args[i]))
		} else if n := Atoi(arg); n > 0 {
			vol = byte(n)
		}
		i++
	}

	loopBehavior := byte(hubfx.AudioQueueFinishLoop)
	e.ack(e.API.HubFx.AudioQueue(ch, vol, loopCount, loopBehavior, path),
		fmt.Sprintf("Queue ch%d: %s vol=%d%%", ch, path, vol))
}

func (e *Engine) cmdHubAudioClear(args []string) {
	ch := byte(hubfx.AudioChAll) // default: all
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = hubfx.AudioChAll
		} else {
			ch = byte(Atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != hubfx.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	e.ack(e.API.HubFx.AudioQueueClear(ch), fmt.Sprintf("Queue cleared %s", target))
}

func (e *Engine) cmdHubAudioStatus(_ []string) {
	e.query(e.API.HubFx.AudioStatus(), e.ParseAudioStatus)
}

func (e *Engine) cmdHubCodecStatus(_ []string) {
	e.query(e.API.HubFx.CodecStatus(), e.ParseCodecStatus)
}

// ─── Engine Commands ───

func (e *Engine) cmdHubEngineStart(_ []string) {
	e.ack(e.API.HubFx.EngineStart(), "Engine start")
}
func (e *Engine) cmdHubEngineStop(_ []string) {
	e.ack(e.API.HubFx.EngineStop(), "Engine stop")
}

func (e *Engine) cmdHubEngineStatus(_ []string) {
	e.query(e.API.HubFx.EngineStatus(), e.ParseEngineStatus)
}

// ─── Config Commands ───

func (e *Engine) cmdHubConfigReload(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	e.ack(e.API.HubFx.ConfigReload(path), "Config reload")
}

func (e *Engine) cmdHubConfigStatus(_ []string) {
	e.query(e.API.HubFx.ConfigStatus(), e.ParseConfigStatus)
}

func (e *Engine) cmdHubConfigSave(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	e.ack(e.API.HubFx.ConfigSave(path), "Config save")
}

// ─── Storage Commands ───

func (e *Engine) cmdHubSdInit(_ []string) { e.ack(e.API.HubFx.SdInit(), "SD card remounted") }

func (e *Engine) cmdHubSdStatus(_ []string) {
	e.query(e.API.HubFx.SdStatus(), e.ParseSdStatus)
}

func (e *Engine) cmdHubFlashStatus(_ []string) {
	e.query(e.API.HubFx.FlashStatus(), e.ParseFlashStatus)
}

func (e *Engine) cmdHubUsbDevices(_ []string) {
	e.query(e.API.HubFx.UsbDevices(), e.ParseUsbDevices)
}

func (e *Engine) cmdHubUsbReset(_ []string) { e.ack(e.API.HubFx.UsbReset(), "USB bus reset") }

// ─── File Operations (storage target pattern) ───

func (e *Engine) cmdHubFileList(args []string) {
	if !e.requireConn() {
		return
	}
	target, path := e.parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	text, err := e.API.Files.List(target, path)
	if err != nil {
		e.Out.Error("%v", err)
		return
	}
	e.FormatListing(text, fmt.Sprintf("%s:%s", StorageTargetName(target), path))
}

func (e *Engine) cmdHubFileDelete(args []string) {
	target, path := e.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		e.Out.Error("Usage: file.delete <sd|flash> <path>")
		return
	}
	e.ack(e.API.Files.Delete(target, path), fmt.Sprintf("Delete %s:%s", StorageTargetName(target), path))
}

func (e *Engine) cmdHubFileMkdir(args []string) {
	target, path := e.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		e.Out.Error("Usage: file.mkdir <sd|flash> <path>")
		return
	}
	e.ack(e.API.Files.Mkdir(target, path), fmt.Sprintf("Mkdir %s:%s", StorageTargetName(target), path))
}

func (e *Engine) cmdHubFileInfo(args []string) {
	if !e.requireConn() {
		return
	}
	target, path := e.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		e.Out.Error("Usage: file.info <sd|flash> <path>")
		return
	}
	r := e.API.Files.Info(target, path)
	if !r.OK {
		e.Out.Error("%s", r.Error)
		return
	}
	if len(r.Response.Payload) >= 6 {
		exists := r.Response.Payload[0]
		isDir := r.Response.Payload[1]
		size := protocol.ReadU32LE(r.Response.Payload, 2)
		display := fmt.Sprintf("%s:%s", StorageTargetName(target), path)
		e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, display))
		if exists != 0 {
			kind := "file"
			if isDir != 0 {
				kind = "directory"
			}
			e.Out.Printf("    Type: %s\n", kind)
			if isDir == 0 {
				e.Out.Printf("    Size: %s (%d bytes)\n", FormatSize(size), size)
			}
		} else {
			e.Out.Printf("    %s\n", e.Out.C(ColorRed, "Not found"))
		}
		e.Out.Println()
	} else {
		e.Out.Error("Response too short")
	}
}

func (e *Engine) cmdHubFileTree(args []string) {
	if !e.requireConn() {
		return
	}
	target, path := e.parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	text, err := e.API.Files.Tree(target, path)
	if err != nil {
		e.Out.Error("%v", err)
		return
	}
	e.RenderTree(text, fmt.Sprintf("%s:%s", StorageTargetName(target), path))
}

func (e *Engine) cmdHubFileCat(args []string) {
	if !e.requireConn() {
		return
	}
	target, path := e.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		e.Out.Error("Usage: file.cat <sd|flash> <path>")
		return
	}
	e.Out.Info("Reading %s:%s ...", StorageTargetName(target), path)
	text, err := e.API.Files.Cat(target, path)
	if err != nil {
		e.Out.Error("%v", err)
		return
	}
	e.Out.Println()
	e.Out.Printf("%s", text)
	e.Out.Printf("\n    (%d bytes)\n", len(text))
}

func (e *Engine) cmdHubFileDownload(args []string) {
	if !e.requireConn() {
		return
	}
	if len(args) < 3 {
		e.Out.Error("Usage: file.download <sd|flash> <remote> <local>")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hubfx.StorageTargetSd
	case "flash":
		target = hubfx.StorageTargetFlash
	default:
		e.Out.Error("Storage target must be 'sd' or 'flash'")
		return
	}
	remotePath := args[1]
	localPath := args[2]

	e.Out.Info("Downloading %s:%s ...", StorageTargetName(target), remotePath)
	result, err := e.API.Files.Download(target, remotePath, 60*time.Second)
	if err != nil {
		e.Out.Error("%v", err)
		return
	}

	if dir := filepath.Dir(localPath); dir != "" && dir != "." {
		os.MkdirAll(dir, 0755)
	}
	if err := os.WriteFile(localPath, result.Data, 0644); err != nil {
		e.Out.Error("Failed to write local file: %v", err)
		return
	}
	e.Out.OK("Downloaded %d bytes → %s", len(result.Data), localPath)
}

func (e *Engine) cmdHubFileUpload(args []string) {
	if !e.requireConn() {
		return
	}
	if len(args) < 3 {
		e.Out.Error("Usage: file.upload <sd|flash> <local> <remote> [--stream]")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hubfx.StorageTargetSd
	case "flash":
		target = hubfx.StorageTargetFlash
	default:
		e.Out.Error("Storage target must be 'sd' or 'flash'")
		return
	}
	localPath := args[1]
	remotePath := args[2]

	// Parse upload mode from flags
	mode := api.UploadSync
	for _, a := range args[3:] {
		switch strings.ToLower(a) {
		case "--stream":
			mode = api.UploadStream
		}
	}

	fileData, err := os.ReadFile(localPath)
	if err != nil {
		e.Out.Error("Cannot read local file: %v", err)
		return
	}

	fileSize := len(fileData)
	modeName := "sync"
	switch mode {
	case api.UploadStream:
		modeName = "stream"
	}
	e.Out.Info("Uploading %s (%d bytes) → %s:%s [%s]", localPath, fileSize,
		StorageTargetName(target), remotePath, modeName)

	uploadStart := time.Now()
	result := e.API.Files.Upload(target, remotePath, fileData, mode,
		func(sent, total int) {
			e.Out.Printf("\r%s  ", FormatProgressBar(sent, total, uploadStart, 30))
		})
	e.Out.Println()

	if !result.OK {
		e.Out.Error("Upload failed: %s", result.Error)
		return
	}

	e.Out.OK("Uploaded %d bytes in %.1fs (%.1f KB/s)", result.BytesTransferred,
		result.Elapsed.Seconds(), result.SpeedKBs)

	if result.LocalMD5 != "" {
		if result.MD5Match {
			e.Out.OK("MD5 verified: %s", result.RemoteMD5)
		} else {
			e.Out.Error("MD5 MISMATCH! local=%s remote=%s", result.LocalMD5, result.RemoteMD5)
		}
	}
}

func (e *Engine) cmdHubFileCancel(_ []string) {
	e.ack(e.API.Files.CancelUpload(), "Upload cancelled")
}

// ─── Storage Helpers ───

func (e *Engine) parseStorageArgs(args []string, defaultPath string) (byte, string) {
	if !e.requireArgs(args, 1, "<sd|flash> [path]") {
		return 255, ""
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hubfx.StorageTargetSd
	case "flash":
		target = hubfx.StorageTargetFlash
	default:
		e.Out.Error("Storage target must be 'sd' or 'flash'")
		return 255, ""
	}
	path := defaultPath
	if len(args) > 1 {
		path = args[1]
	}
	return target, path
}

// StorageTargetName returns the display name for a storage target.
func StorageTargetName(t byte) string {
	if t == hubfx.StorageTargetSd {
		return "SD"
	}
	return "Flash"
}
