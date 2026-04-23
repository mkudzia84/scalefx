package hubfx

// ScaleFX Engine - HubFX Command Handler
// Wires up the HubFX-only command group + the universal storage/config
// group, the periodic STATUS_BROADCAST observer chain, and the small
// shared helpers (slave-type resolver, target-name pretty-print,
// <sd|flash> + path argument parser). Per-domain command bodies live in
// slaves.go / audio.go / engine.go / storage.go / files.go.

import (
	"scalefx/engine"
	pcore "scalefx/protocol/core"
	hfxp "scalefx/protocol/hubfx"
	"strings"
)

// Handler groups all HubFX commands, decoders, and parsers.
//
// Broadcast observers are silent by default — the CLI prints via the
// synchronous `status` command path. Studio subscribes by calling
// handler.OnStatusBroadcast.Add(fn).
type Handler struct {
	E *engine.Engine

	OnStatusBroadcast engine.Observers[StatusBroadcast] // periodic STATUS_BROADCAST
}

// Register adds the HubFX command group and status parser to the engine.
// Returns the Handler so external consumers can install listeners.
func Register(eng *engine.Engine) *Handler {
	h := &Handler{E: eng}
	eng.RegisterStatusParser(pcore.CtrlHubFX, func(data []byte) {
		if s := DecodeStatusBroadcast(data); s != nil {
			h.FormatStatusBroadcast(s)
		} else {
			h.E.Out.Printf("  HubFX: (incomplete: %d bytes)\n", len(data))
		}
	})
	eng.RegisterStatusBroadcastParser(pcore.CtrlHubFX, func(data []byte) {
		if h.OnStatusBroadcast.Len() == 0 {
			return
		}
		if s := DecodeStatusBroadcast(data); s != nil {
			h.OnStatusBroadcast.Fire(s)
		}
	})
	eng.AddGroup(h.commands())
	eng.AddGroup(h.storageCommands())
	return h
}

// commands returns the HubFX-only command group (audio, engine, codec, slaves, USB host).
// These commands map to packet types handled only by the HubFX master firmware.
func (h *Handler) commands() *engine.CmdGroup {
	return &engine.CmdGroup{
		Name:       "HubFX",
		Controller: pcore.CtrlHubFX,
		Prefix:     "hub",
		Color:      engine.ColorCyan,
		Commands: []engine.CmdEntry{
			{"slaves", h.cmdSlaves, "slaves", "List connected slaves", true},
			{"slave.init", h.cmdSlaveInit, "slave.init <type>", "Init slave (gunfx|lightfx|gearcontrol|1|2|3)", true},
			{"slave.info", h.cmdSlaveInfo, "slave.info <type>", "Query slave info", true},
			{"audio.play", h.cmdAudioPlay, "audio.play <ch> <path> [vol] [ch1|ch2] [loop [N|inf]]", "Play audio", true},
			{"audio.stop", h.cmdAudioStop, "audio.stop [ch|all]", "Stop audio (default: all)", true},
			{"audio.volume", h.cmdAudioVol, "audio.volume <ch|master> <vol>", "Set volume (0-100)", true},
			{"audio.fade", h.cmdAudioFade, "audio.fade <ch>", "Fade out audio", true},
			{"audio.queue", h.cmdAudioQueue, "audio.queue <ch> <path> [vol] [loop N]", "Queue sound after current", true},
			{"audio.clear", h.cmdAudioClear, "audio.clear [ch|all]", "Clear audio queue", true},
			{"audio.status", h.cmdAudioStatus, "audio.status", "Audio mixer status", true},
			{"codec.status", h.cmdCodecStatus, "codec.status", "DAC codec status", true},
			{"engine.start", h.cmdEngineStart, "engine.start", "Start engine effect", true},
			{"engine.stop", h.cmdEngineStop, "engine.stop", "Stop engine effect", true},
			{"engine.status", h.cmdEngineStatus, "engine.status", "Engine status", true},
			{"usb.devices", h.cmdUsbDevices, "usb.devices", "List USB devices", true},
			{"usb.reset", h.cmdUsbReset, "usb.reset", "Reset USB bus", true},
		},
	}
}

// storageCommands returns the universal storage + config command group.
// Every controller firmware registers StorageServer + ConfigServer, so these
// commands work on HubFX, LightFX, and GearControl alike (no Controller filter).
func (h *Handler) storageCommands() *engine.CmdGroup {
	g := &engine.CmdGroup{
		Name:  "Storage & Config",
		Color: engine.ColorYellow,
		Commands: []engine.CmdEntry{
			{"sd.init", h.cmdSdInit, "sd.init", "Initialize SD card", true},
			{"sd.status", h.cmdSdStatus, "sd.status", "SD card status", true},
			{"flash.status", h.cmdFlashStatus, "flash.status", "Flash status", true},
			{"file.list", h.cmdFileList, "file.list <sd|flash> [path]", "List files", true},
			{"file.delete", h.cmdFileDelete, "file.delete [-r] <sd|flash> <path>", "Delete file (or tree with -r)", true},
			{"file.mkdir", h.cmdFileMkdir, "file.mkdir [-p] <sd|flash> <path>", "Create directory (mkdir -p with -p)", true},
			{"file.info", h.cmdFileInfo, "file.info <sd|flash> <path>", "File info", true},
			{"file.tree", h.cmdFileTree, "file.tree <sd|flash> [path]", "Tree view", true},
			{"file.cat", h.cmdFileCat, "file.cat <sd|flash> <path>", "Display file contents", true},
			{"file.download", h.cmdFileDownload, "file.download <sd|flash> <remote> <local>", "Download file", true},
			{"file.upload", h.cmdFileUpload, "file.upload <sd|flash> <local> <remote> [--stream]", "Upload file", true},
			{"file.upload-batch", h.cmdFileUploadBatch,
				"file.upload-batch <sd|flash> <remote-cwd> <local1> [local2 ...] [--stream]",
				"Upload files/dirs preserving structure under remote-cwd", true},
			{"file.cancel", h.cmdFileCancel, "file.cancel", "Cancel active upload", true},
		},
	}
	g.Commands = append(g.Commands, h.E.ConfigCommands()...)
	return g
}

// ─── Shared Helpers ───

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

// parseStorageArgs validates and decodes the leading "<sd|flash> [path]" arg
// pair shared by every file-system command. Returns target=255 on error so
// callers can early-return without needing a separate ok flag.
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
