package hubfx

// ScaleFX Engine - HubFX Command Handler
// Commands for HubFX master hub controller (ESP32-S3).

import (
	"fmt"
	"os"
	"path/filepath"
	"scalefx/api"
	"scalefx/engine"
	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	hfxp "scalefx/protocol/hubfx"
	"strings"
	"time"
)

// Handler groups all HubFX commands and parsers.
type Handler struct {
	E *engine.Engine
}

// Register adds the HubFX command group and status parser to the engine.
func Register(eng *engine.Engine) {
	h := &Handler{E: eng}
	eng.RegisterStatusParser(pcore.CtrlHubFX, h.parseHubFXStatus)
	eng.AddGroup(h.commands())
}

func (h *Handler) commands() *engine.CmdGroup {
	return &engine.CmdGroup{
		Name:       "HubFX",
		Controller: pcore.CtrlHubFX,
		Color:      engine.ColorCyan,
		Commands: map[string]engine.CmdEntry{
			"slaves":        {h.cmdSlaves, "slaves", "List connected slaves", true},
			"slave.init":    {h.cmdSlaveInit, "slave.init <type>", "Init slave (gunfx|lightfx|gearcontrol|1|2|3)", true},
			"slave.info":    {h.cmdSlaveInfo, "slave.info <type>", "Query slave info", true},
			"audio.play":    {h.cmdAudioPlay, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]", "Play audio", true},
			"audio.stop":    {h.cmdAudioStop, "audio.stop [ch|all]", "Stop audio (default: all)", true},
			"audio.volume":  {h.cmdAudioVol, "audio.volume <ch|master> <vol>", "Set volume (0-100)", true},
			"audio.fade":    {h.cmdAudioFade, "audio.fade <ch>", "Fade out audio", true},
			"audio.queue":   {h.cmdAudioQueue, "audio.queue <ch> <path> [vol] [loop N]", "Queue sound after current", true},
			"audio.clear":   {h.cmdAudioClear, "audio.clear [ch|all]", "Clear audio queue", true},
			"audio.status":  {h.cmdAudioStatus, "audio.status", "Audio mixer status", true},
			"codec.status":  {h.cmdCodecStatus, "codec.status", "DAC codec status", true},
			"engine.start":  {h.cmdEngineStart, "engine.start", "Start engine effect", true},
			"engine.stop":   {h.cmdEngineStop, "engine.stop", "Stop engine effect", true},
			"engine.status": {h.cmdEngineStatus, "engine.status", "Engine status", true},
			"config.reload": {h.cmdConfigReload, "config.reload [path]", "Reload config from SD", true},
			"config.status": {h.cmdConfigStatus, "config.status", "Config status", true},
			"config.save":   {h.cmdConfigSave, "config.save [path]", "Save config to SD", true},
			"sd.init":       {h.cmdSdInit, "sd.init", "Initialize SD card", true},
			"sd.status":     {h.cmdSdStatus, "sd.status", "SD card status", true},
			"flash.status":  {h.cmdFlashStatus, "flash.status", "Flash status", true},
			"file.list":     {h.cmdFileList, "file.list <sd|flash> [path]", "List files", true},
			"file.delete":   {h.cmdFileDelete, "file.delete <sd|flash> <path>", "Delete file", true},
			"file.mkdir":    {h.cmdFileMkdir, "file.mkdir <sd|flash> <path>", "Create directory", true},
			"file.info":     {h.cmdFileInfo, "file.info <sd|flash> <path>", "File info", true},
			"file.tree":     {h.cmdFileTree, "file.tree <sd|flash> [path]", "Tree view", true},
			"file.cat":      {h.cmdFileCat, "file.cat <sd|flash> <path>", "Display file contents", true},
			"file.download": {h.cmdFileDownload, "file.download <sd|flash> <remote> <local>", "Download file", true},
			"file.upload":   {h.cmdFileUpload, "file.upload <sd|flash> <local> <remote> [--stream]", "Upload file", true},
			"file.cancel":   {h.cmdFileCancel, "file.cancel", "Cancel active upload", true},
			"usb.devices":   {h.cmdUsbDevices, "usb.devices", "List USB devices", true},
			"usb.reset":     {h.cmdUsbReset, "usb.reset", "Reset USB bus", true},
		},
	}
}

// ─── Slave Type Resolver ───

// ParseSlaveType resolves a slave type from name or numeric string.
func ParseSlaveType(s string) (byte, bool) {
	typeMap := map[string]byte{
		"gunfx": hfxp.SlaveTypeGunFx, "1": hfxp.SlaveTypeGunFx,
		"lightfx": hfxp.SlaveTypeLightFx, "2": hfxp.SlaveTypeLightFx,
		"gearcontrol": hfxp.SlaveTypeGearControl, "3": hfxp.SlaveTypeGearControl,
	}
	if v, ok := typeMap[strings.ToLower(s)]; ok {
		return v, true
	}
	return 0, false
}

// StorageTargetName returns the display name for a storage target.
func StorageTargetName(t byte) string {
	if t == hfxp.StorageTargetSd {
		return "SD"
	}
	return "Flash"
}

// ─── Slave Commands ───

func (h *Handler) cmdSlaves(_ []string) {
	h.E.Query(h.E.API.HubFx.SlaveList(), h.parseSlaveList)
}

func (h *Handler) cmdSlaveInit(args []string) {
	if !h.E.RequireArgs(args, 1, "slave.init <type> (gunfx|lightfx|gearcontrol or 1|2|3)") {
		return
	}
	slaveType, ok := ParseSlaveType(args[0])
	if !ok {
		h.E.Out.Error("Unknown slave type: %s", args[0])
		h.E.Out.Info("Valid types: gunfx (1), lightfx (2), gearcontrol (3)")
		return
	}
	h.E.Ack(h.E.API.HubFx.SlaveInit(slaveType), fmt.Sprintf("%s slave initialized", hfxp.SlaveTypeName(slaveType)))
}

func (h *Handler) cmdSlaveInfo(args []string) {
	if !h.E.RequireArgs(args, 1, "slave.info <type> (gunfx|lightfx|gearcontrol or 1|2|3)") || !h.E.RequireConn() {
		return
	}
	slaveType, ok := ParseSlaveType(args[0])
	if !ok {
		h.E.Out.Error("Unknown slave type: %s", args[0])
		return
	}
	h.E.Query(h.E.API.HubFx.SlaveInfo(slaveType), h.parseSlaveInfo)
}

// ─── Audio Commands ───

func (h *Handler) cmdAudioPlay(args []string) {
	if !h.E.RequireArgs(args, 2, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]") {
		return
	}
	ch := byte(engine.Atoi(args[0]))
	path := args[1]
	vol := byte(100)
	output := byte(hfxp.AudioOutputAll)
	loopMode := byte(hfxp.AudioLoopNone)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		switch arg {
		case "ch1":
			output = hfxp.AudioOutputCh1
		case "ch2":
			output = hfxp.AudioOutputCh2
		case "all":
			output = hfxp.AudioOutputAll
		case "loop":
			if i+1 < len(args) {
				i++
				if strings.ToLower(args[i]) == "inf" {
					loopMode = hfxp.AudioLoopInfinite
				} else {
					loopMode = hfxp.AudioLoopFinite
					loopCount = uint16(engine.Atoi(args[i]))
				}
			} else {
				loopMode = hfxp.AudioLoopInfinite
			}
		default:
			if n := engine.Atoi(arg); n > 0 {
				vol = byte(n)
			}
		}
		i++
	}

	outputName := ""
	switch output {
	case hfxp.AudioOutputCh1:
		outputName = " [CH1]"
	case hfxp.AudioOutputCh2:
		outputName = " [CH2]"
	}
	loopStr := ""
	if loopMode == hfxp.AudioLoopInfinite {
		loopStr = " (loop inf)"
	} else if loopMode == hfxp.AudioLoopFinite {
		loopStr = fmt.Sprintf(" (loop x%d)", loopCount)
	}
	h.E.Ack(h.E.API.HubFx.AudioPlay(ch, vol, output, loopMode, loopCount, path),
		fmt.Sprintf("Play ch%d: %s vol=%d%%%s%s", ch, path, vol, outputName, loopStr))
}

func (h *Handler) cmdAudioStop(args []string) {
	ch := byte(hfxp.AudioChAll)
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = hfxp.AudioChAll
		} else {
			ch = byte(engine.Atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != hfxp.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	h.E.Ack(h.E.API.HubFx.AudioStop(ch), fmt.Sprintf("Audio stop %s", target))
}

func (h *Handler) cmdAudioVol(args []string) {
	if !h.E.RequireArgs(args, 2, "audio.volume <ch|master> <volume>") {
		return
	}
	ch := byte(0)
	if strings.ToLower(args[0]) == "master" {
		ch = hfxp.AudioChAll
	} else {
		ch = byte(engine.Atoi(args[0]))
	}
	vol := byte(engine.Atoi(args[1]))
	target := "master"
	if ch != hfxp.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	h.E.Ack(h.E.API.HubFx.AudioVolume(ch, vol), fmt.Sprintf("Volume %s → %d%%", target, vol))
}

func (h *Handler) cmdAudioFade(args []string) {
	if !h.E.RequireArgs(args, 1, "audio.fade <ch>") {
		return
	}
	h.E.Ack(h.E.API.HubFx.AudioFade(byte(engine.Atoi(args[0]))), fmt.Sprintf("Fade out ch%s", args[0]))
}

func (h *Handler) cmdAudioQueue(args []string) {
	if !h.E.RequireArgs(args, 2, "audio.queue <ch> <path> [vol] [loop N]") {
		return
	}
	ch := byte(engine.Atoi(args[0]))
	path := args[1]
	vol := byte(100)
	loopCount := uint16(0)

	i := 2
	for i < len(args) {
		arg := strings.ToLower(args[i])
		if arg == "loop" && i+1 < len(args) {
			i++
			loopCount = uint16(engine.Atoi(args[i]))
		} else if n := engine.Atoi(arg); n > 0 {
			vol = byte(n)
		}
		i++
	}

	loopBehavior := byte(hfxp.AudioQueueFinishLoop)
	h.E.Ack(h.E.API.HubFx.AudioQueue(ch, vol, loopCount, loopBehavior, path),
		fmt.Sprintf("Queue ch%d: %s vol=%d%%", ch, path, vol))
}

func (h *Handler) cmdAudioClear(args []string) {
	ch := byte(hfxp.AudioChAll)
	if len(args) > 0 {
		if strings.ToLower(args[0]) == "all" {
			ch = hfxp.AudioChAll
		} else {
			ch = byte(engine.Atoi(args[0]))
		}
	}
	target := "all channels"
	if ch != hfxp.AudioChAll {
		target = fmt.Sprintf("ch%d", ch)
	}
	h.E.Ack(h.E.API.HubFx.AudioQueueClear(ch), fmt.Sprintf("Queue cleared %s", target))
}

func (h *Handler) cmdAudioStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.AudioStatus(), h.parseAudioStatus)
}

func (h *Handler) cmdCodecStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.CodecStatus(), h.parseCodecStatus)
}

// ─── Engine Commands ───

func (h *Handler) cmdEngineStart(_ []string) {
	h.E.Ack(h.E.API.HubFx.EngineStart(), "Engine start")
}
func (h *Handler) cmdEngineStop(_ []string) {
	h.E.Ack(h.E.API.HubFx.EngineStop(), "Engine stop")
}
func (h *Handler) cmdEngineStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.EngineStatus(), h.parseEngineStatus)
}

// ─── Config Commands ───

func (h *Handler) cmdConfigReload(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	h.E.Ack(h.E.API.HubFx.ConfigReload(path), "Config reload")
}

func (h *Handler) cmdConfigStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.ConfigStatus(), h.parseConfigStatus)
}

func (h *Handler) cmdConfigSave(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	h.E.Ack(h.E.API.HubFx.ConfigSave(path), "Config save")
}

// ─── Storage Commands ───

func (h *Handler) cmdSdInit(_ []string) { h.E.Ack(h.E.API.HubFx.SdInit(), "SD card remounted") }

func (h *Handler) cmdSdStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.SdStatus(), h.parseSdStatus)
}

func (h *Handler) cmdFlashStatus(_ []string) {
	h.E.Query(h.E.API.HubFx.FlashStatus(), h.parseFlashStatus)
}

func (h *Handler) cmdUsbDevices(_ []string) {
	h.E.Query(h.E.API.HubFx.UsbDevices(), h.parseUsbDevices)
}

func (h *Handler) cmdUsbReset(_ []string) { h.E.Ack(h.E.API.HubFx.UsbReset(), "USB bus reset") }

// ─── File Operations ───

func (h *Handler) cmdFileList(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	text, err := h.E.API.Files.List(target, path)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}
	h.FormatListing(text, fmt.Sprintf("%s:%s", StorageTargetName(target), path))
}

func (h *Handler) cmdFileDelete(args []string) {
	target, path := h.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.delete <sd|flash> <path>")
		return
	}
	h.E.Ack(h.E.API.Files.Delete(target, path), fmt.Sprintf("Delete %s:%s", StorageTargetName(target), path))
}

func (h *Handler) cmdFileMkdir(args []string) {
	target, path := h.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.mkdir <sd|flash> <path>")
		return
	}
	h.E.Ack(h.E.API.Files.Mkdir(target, path), fmt.Sprintf("Mkdir %s:%s", StorageTargetName(target), path))
}

func (h *Handler) cmdFileInfo(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.info <sd|flash> <path>")
		return
	}
	r := h.E.API.Files.Info(target, path)
	if !r.OK {
		h.E.Out.Error("%s", r.Error)
		return
	}
	if len(r.Response.Payload) >= 6 {
		exists := r.Response.Payload[0]
		isDir := r.Response.Payload[1]
		size := protocol.ReadU32LE(r.Response.Payload, 2)
		display := fmt.Sprintf("%s:%s", StorageTargetName(target), path)
		h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, display))
		if exists != 0 {
			kind := "file"
			if isDir != 0 {
				kind = "directory"
			}
			h.E.Out.Printf("    Type: %s\n", kind)
			if isDir == 0 {
				h.E.Out.Printf("    Size: %s (%d bytes)\n", FormatSize(size), size)
			}
		} else {
			h.E.Out.Printf("    %s\n", h.E.Out.C(engine.ColorRed, "Not found"))
		}
		h.E.Out.Println()
	} else {
		h.E.Out.Error("Response too short")
	}
}

func (h *Handler) cmdFileTree(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	text, err := h.E.API.Files.Tree(target, path)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}
	h.RenderTree(text, fmt.Sprintf("%s:%s", StorageTargetName(target), path))
}

func (h *Handler) cmdFileCat(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.cat <sd|flash> <path>")
		return
	}
	h.E.Out.Info("Reading %s:%s ...", StorageTargetName(target), path)
	text, err := h.E.API.Files.Cat(target, path)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}
	h.E.Out.Println()
	h.E.Out.Printf("%s", text)
	h.E.Out.Printf("\n    (%d bytes)\n", len(text))
}

func (h *Handler) cmdFileDownload(args []string) {
	if !h.E.RequireConn() {
		return
	}
	if len(args) < 3 {
		h.E.Out.Error("Usage: file.download <sd|flash> <remote> <local>")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hfxp.StorageTargetSd
	case "flash":
		target = hfxp.StorageTargetFlash
	default:
		h.E.Out.Error("Storage target must be 'sd' or 'flash'")
		return
	}
	remotePath := args[1]
	localPath := args[2]

	h.E.Out.Info("Downloading %s:%s ...", StorageTargetName(target), remotePath)
	result, err := h.E.API.Files.Download(target, remotePath, 60*time.Second)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}

	if dir := filepath.Dir(localPath); dir != "" && dir != "." {
		os.MkdirAll(dir, 0755)
	}
	if err := os.WriteFile(localPath, result.Data, 0644); err != nil {
		h.E.Out.Error("Failed to write local file: %v", err)
		return
	}
	h.E.Out.OK("Downloaded %d bytes → %s", len(result.Data), localPath)
}

func (h *Handler) cmdFileUpload(args []string) {
	if !h.E.RequireConn() {
		return
	}
	if len(args) < 3 {
		h.E.Out.Error("Usage: file.upload <sd|flash> <local> <remote> [--stream]")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hfxp.StorageTargetSd
	case "flash":
		target = hfxp.StorageTargetFlash
	default:
		h.E.Out.Error("Storage target must be 'sd' or 'flash'")
		return
	}
	localPath := args[1]
	remotePath := args[2]

	mode := api.UploadSync
	for _, a := range args[3:] {
		switch strings.ToLower(a) {
		case "--stream":
			mode = api.UploadStream
		}
	}

	fileData, err := os.ReadFile(localPath)
	if err != nil {
		h.E.Out.Error("Cannot read local file: %v", err)
		return
	}

	fileSize := len(fileData)
	modeName := "sync"
	switch mode {
	case api.UploadStream:
		modeName = "stream"
	}
	h.E.Out.Info("Uploading %s (%d bytes) → %s:%s [%s]", localPath, fileSize,
		StorageTargetName(target), remotePath, modeName)

	uploadStart := time.Now()
	result := h.E.API.Files.Upload(target, remotePath, fileData, mode,
		func(sent, total int) {
			h.E.Out.Printf("\r%s  ", engine.FormatProgressBar(sent, total, uploadStart, 30))
		})
	h.E.Out.Println()

	if !result.OK {
		h.E.Out.Error("Upload failed: %s", result.Error)
		return
	}

	h.E.Out.OK("Uploaded %d bytes in %.1fs (%.1f KB/s)", result.BytesTransferred,
		result.Elapsed.Seconds(), result.SpeedKBs)

	if result.LocalMD5 != "" {
		if result.MD5Match {
			h.E.Out.OK("MD5 verified: %s", result.RemoteMD5)
		} else {
			h.E.Out.Error("MD5 MISMATCH! local=%s remote=%s", result.LocalMD5, result.RemoteMD5)
		}
	}
}

func (h *Handler) cmdFileCancel(_ []string) {
	h.E.Ack(h.E.API.Files.CancelUpload(), "Upload cancelled")
}

// ─── Storage Helpers ───

func (h *Handler) parseStorageArgs(args []string, defaultPath string) (byte, string) {
	if !h.E.RequireArgs(args, 1, "<sd|flash> [path]") {
		return 255, ""
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hfxp.StorageTargetSd
	case "flash":
		target = hfxp.StorageTargetFlash
	default:
		h.E.Out.Error("Storage target must be 'sd' or 'flash'")
		return 255, ""
	}
	path := defaultPath
	if len(args) > 1 {
		path = args[1]
	}
	return target, path
}
