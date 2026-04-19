package main

import (
	"context"
	"fmt"
	"scalefx/api"
	"scalefx/engine"
	"scalefx/engine/handlers"
	"scalefx/engine/handlers/gearcontrol"
	"scalefx/engine/handlers/gunfx"
	"scalefx/engine/handlers/hubfx"
	"scalefx/engine/handlers/lightfx"
	"scalefx/firmware"
	"os"
	"path/filepath"
	"scalefx/protocol"
	"scalefx/protocol/core"
	hfxp "scalefx/protocol/hubfx"
	"sort"
	"strings"
	"sync"
	"time"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// ─── Types exposed to frontend ───

type PortInfo struct {
	Name        string `json:"name"`
	Description string `json:"description"`
}

type ConnectionInfo struct {
	Connected      bool   `json:"connected"`
	Initialized    bool   `json:"initialized"`
	Port           string `json:"port"`
	ControllerType string `json:"controllerType"`
	ControllerName string `json:"controllerName"`
	FirmwareVer    string `json:"firmwareVersion"`
	Build          uint32 `json:"build"`
	Platform       string `json:"platform"`
	CPUMHz         uint32 `json:"cpuMHz"`
	FreeRAM        uint32 `json:"freeRAM"`
	// Capabilities is the bitmask the firmware advertised in IDENTIFY/INIT_READY
	// (mirrors core.Cap* — flash, sd, audio, usb_host, engine, config, slave_bus).
	// 0 means "legacy firmware" — UI should fall back to probing.
	Capabilities uint32 `json:"capabilities"`
}

type SlaveInfo struct {
	Type      string `json:"type"`
	Name      string `json:"name"`
	Connected bool   `json:"connected"`
	Ready     bool   `json:"ready"`
}

// GearControl event payloads are defined in engine/handlers/gearcontrol/types.go
// (StatusBroadcast, CalibStatus, SeqStatus, DoorStatus). We emit those directly
// as Wails events — never re-decode here. See CLAUDE.md Rule 19.

// ─── App struct ───

type App struct {
	ctx context.Context
	eng *engine.Engine
	reg *handlers.Registry
	out *GUIOutput
	mu  sync.Mutex

	// Port watcher
	stopPortWatcher chan struct{}
}

func NewApp() *App {
	out := &GUIOutput{}
	eng := engine.NewEngine(out, "", false)
	reg := handlers.RegisterDefaults(eng)
	eng.PromptSelectPort = func(ports []string) string { return "" }

	return &App{eng: eng, reg: reg, out: out}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	a.out.ctx = ctx
	a.eng.OnDisconnect = func() {
		wailsRT.EventsEmit(ctx, "connection:changed", a.getConnectionInfo())
	}

	// Typed event listeners — installed on handlers from the registry.
	// Each board package owns its wire→struct decoding; we only forward the
	// decoded structs to the frontend as Wails events. See CLAUDE.md Rule 19.
	a.reg.GearControl.OnStatusBroadcast.Add(func(s *gearcontrol.StatusBroadcast) {
		wailsRT.EventsEmit(a.ctx, "gearcontrol:status", s)
	})
	a.reg.GearControl.OnCalibStatus.Add(func(c *gearcontrol.CalibStatus) {
		wailsRT.EventsEmit(a.ctx, "gearcontrol:calib", c)
	})
	a.reg.GearControl.OnSeqStatus.Add(func(s *gearcontrol.SeqStatus) {
		wailsRT.EventsEmit(a.ctx, "gearcontrol:seq", s)
	})
	a.reg.GearControl.OnDoorStatus.Add(func(d *gearcontrol.DoorStatus) {
		wailsRT.EventsEmit(a.ctx, "gearcontrol:door", d)
	})
	a.reg.GunFX.OnStatusBroadcast.Add(func(s *gunfx.StatusBroadcast) {
		wailsRT.EventsEmit(a.ctx, "gunfx:status", s)
	})
	a.reg.LightFX.OnStatusBroadcast.Add(func(s *lightfx.StatusBroadcast) {
		wailsRT.EventsEmit(a.ctx, "lightfx:status", s)
	})
	a.reg.LightFX.OnLandingLightStatus.Add(func(s *lightfx.LandingLightStatus) {
		wailsRT.EventsEmit(a.ctx, "lightfx:landing", s)
	})
	a.reg.HubFX.OnStatusBroadcast.Add(func(s *hubfx.StatusBroadcast) {
		wailsRT.EventsEmit(a.ctx, "hubfx:status", s)
	})

	a.startPortWatcher()
}

func (a *App) shutdown(_ context.Context) {
	a.stopPortWatcherLoop()
	a.eng.Cleanup()
}

// ─── Port Watcher ───

func (a *App) startPortWatcher() {
	a.stopPortWatcher = make(chan struct{})
	go func() {
		var lastPorts string
		for {
			select {
			case <-a.stopPortWatcher:
				return
			case <-time.After(1 * time.Second):
			}
			detailed := protocol.ListPortsDetailed()
			names := make([]string, len(detailed))
			for i, p := range detailed {
				names[i] = p.Name
			}
			sort.Strings(names)
			key := strings.Join(names, ",")
			if key != lastPorts {
				lastPorts = key
				// Rebuild in sorted order with descriptions
				byName := make(map[string]string, len(detailed))
				for _, p := range detailed {
					byName[p.Name] = p.Description
				}
				items := make([]PortInfo, len(names))
				for i, n := range names {
					items[i] = PortInfo{Name: n, Description: byName[n]}
				}
				wailsRT.EventsEmit(a.ctx, "ports:changed", items)
			}
		}
	}()
}

func (a *App) stopPortWatcherLoop() {
	if a.stopPortWatcher != nil {
		close(a.stopPortWatcher)
	}
}

// ─── Exposed Methods ───

func (a *App) ListPorts() []PortInfo {
	ports := protocol.ListPortsDetailed()
	result := make([]PortInfo, len(ports))
	for i, p := range ports {
		result[i] = PortInfo{Name: p.Name, Description: p.Description}
	}
	return result
}

func (a *App) Connect(port string) ConnectionInfo {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.eng.Dispatch("connect " + port)

	// Auto-init with verbose flag for live status broadcast
	if a.eng.Conn != nil && !a.eng.Initialized && a.eng.ControllerType != "" {
		a.eng.Dispatch("init direct verbose")
	}

	info := a.getConnectionInfo()
	wailsRT.EventsEmit(a.ctx, "connection:changed", info)
	return info
}

func (a *App) Disconnect() ConnectionInfo {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.eng.Dispatch("disconnect")

	info := a.getConnectionInfo()
	wailsRT.EventsEmit(a.ctx, "connection:changed", info)
	return info
}

func (a *App) SendCommand(cmd string) {
	trimmed := strings.TrimSpace(cmd)
	if trimmed == "" {
		return
	}
	if trimmed == "quit" || trimmed == "exit" || trimmed == "q" {
		wailsRT.Quit(a.ctx)
		return
	}

	a.mu.Lock()
	defer a.mu.Unlock()

	// Echo command
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{
		Type:    "command",
		Content: escapeHTML(trimmed),
	})

	a.eng.Dispatch(trimmed)

	wailsRT.EventsEmit(a.ctx, "connection:changed", a.getConnectionInfo())
}

func (a *App) GetConnectionInfo() ConnectionInfo {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.getConnectionInfo()
}

// echoCommand mirrors SendCommand's "command" echo so GUI-initiated actions
// (File Manager, Save Configuration, etc.) show up in the console the same
// way typed commands do.
func (a *App) echoCommand(cmd string) {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{
		Type:    "command",
		Content: escapeHTML(cmd),
	})
}

// echoOutput prints a plain line to the console (used for stream replies
// like `file.list`).
func (a *App) echoOutput(text string) {
	if a.ctx == nil || text == "" {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{
		Type:    "output",
		Content: escapeHTML(text),
	})
}

// echoOK / echoError report the outcome of a GUI-initiated action.
func (a *App) echoOK(format string, args ...any) {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{
		Type:    "ok",
		Content: escapeHTML(fmt.Sprintf(format, args...)),
	})
}

func (a *App) echoError(format string, args ...any) {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{
		Type:    "error",
		Content: escapeHTML(fmt.Sprintf(format, args...)),
	})
}

// GetSlaveInfo returns the slave controllers known from the last HubFX status.
// Returns an empty slice for non-HubFX connections.
func (a *App) GetSlaveInfo() []SlaveInfo {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.eng.ControllerType != core.CtrlHubFX || a.eng.Conn == nil {
		return []SlaveInfo{}
	}

	// Return the standard slave types with connected/ready inferred from engine
	// (the slaves command queries the device; here we return the known types)
	return []SlaveInfo{
		{Type: "gunfx", Name: "GunFX", Connected: false, Ready: false},
		{Type: "lightfx", Name: "LightFX", Connected: false, Ready: false},
		{Type: "gearcontrol", Name: "GearControl", Connected: false, Ready: false},
	}
}

// ─── Internal ───

func (a *App) getConnectionInfo() ConnectionInfo {
	info := ConnectionInfo{
		Connected:   a.eng.Conn != nil,
		Initialized: a.eng.Initialized,
		Port:        a.eng.Port,
	}
	if a.eng.ControllerType != "" {
		info.ControllerType = a.eng.ControllerType
		info.ControllerName = engine.ControllerLabels[a.eng.ControllerType]
	}
	if a.eng.Info != nil {
		info.FirmwareVer = a.eng.Info.Version
		info.Build = a.eng.Info.Build
		info.Platform = a.eng.Info.Platform
		info.CPUMHz = a.eng.Info.CPUMHz
		info.FreeRAM = a.eng.Info.FreeRAM
		info.Capabilities = a.eng.Info.Capabilities
	}
	return info
}

// ─── Config File Operations (GUI bindings) ───

// configPathFor returns the canonical per-board YAML path on the device's
// flash. Mirrors firmware schemas' defaultPath(): gearcontrol → /gearcontrol.yaml,
// lightfx → /lightfx.yaml, hubfx → /hubfx.yaml, gunfx → /gunfx.yaml (reserved;
// no schema yet). Empty controller type falls back to /config.yaml so legacy
// boards still round-trip until they are re-flashed with the new firmware.
func (a *App) configPathFor(ct string) string {
	switch ct {
	case "gearcontrol":
		return "/gearcontrol.yaml"
	case "lightfx":
		return "/lightfx.yaml"
	case "hubfx":
		return "/hubfx.yaml"
	case "gunfx":
		return "/gunfx.yaml"
	default:
		return "/config.yaml"
	}
}

// DownloadConfig reads /config.yaml from the connected board's flash and
// returns its contents as a string. Returns an empty string + nil error when
// the file does not exist (fresh board) so the caller can treat that case as
// "start from defaults" rather than propagating an error.
func (a *App) DownloadConfig() (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	path := a.configPathFor(a.eng.ControllerType)
	a.echoCommand(fmt.Sprintf("config.load (%s)", path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return "", fmt.Errorf("not connected")
	}
	result, err := a.eng.API.Files.Download(hfxp.StorageTargetFlash, path, 10*time.Second)
	if err != nil {
		msg := err.Error()
		if strings.Contains(msg, "not found") || strings.Contains(msg, "NOT_FOUND") {
			a.echoOutput(fmt.Sprintf("%s not found — using defaults", path))
			return "", nil
		}
		a.echoError("config load failed: %s", msg)
		return "", err
	}
	a.echoOK("Loaded %s (%d bytes)", path, len(result.Data))
	return string(result.Data), nil
}

// UploadConfig writes the supplied YAML to /config.yaml on the connected
// board's flash, then triggers a config reload so the new values take effect
// without a reboot. Emits "fs:progress" events so the Studio can show the
// shared UploadProgressDialog.
func (a *App) UploadConfig(yaml string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	data := []byte(yaml)
	path := a.configPathFor(a.eng.ControllerType)
	a.echoCommand(fmt.Sprintf("config.save (%d bytes → %s)", len(data), path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "uploading", "path": path, "sent": 0, "total": len(data),
	})
	res := a.eng.API.Files.Upload(hfxp.StorageTargetFlash, path,
		data, api.UploadSync, func(sent, total int) {
			wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
				"phase": "uploading", "path": path, "sent": sent, "total": total,
			})
		})
	if !res.OK {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": res.Error})
		a.echoError("upload failed: %s", res.Error)
		return fmt.Errorf("upload failed: %s", res.Error)
	}
	a.echoCommand("config.reload")
	reload := a.eng.API.HubFx.ConfigReload("")
	if !reload.OK {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": reload.Error})
		a.echoError("uploaded but reload failed: %s", reload.Error)
		return fmt.Errorf("uploaded but reload failed: %s", reload.Error)
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase":    "done",
		"path":     path,
		"sent":     int(res.BytesTransferred),
		"total":    int(res.BytesTransferred),
		"md5Match": res.MD5Match,
		"speedKBs": res.SpeedKBs,
	})
	a.echoOK("Saved %s (%d bytes, %.1f KB/s) — reloaded",
		path, res.BytesTransferred, res.SpeedKBs)
	return nil
}

// SaveTextFile prompts the user for a destination via the native save dialog
// and writes `content` there. Returns the chosen path; empty string if the
// user cancelled (treated as non-error).
func (a *App) SaveTextFile(content, defaultName, title string) (string, error) {
	if title == "" {
		title = "Save File"
	}
	path, err := wailsRT.SaveFileDialog(a.ctx, wailsRT.SaveDialogOptions{
		Title:           title,
		DefaultFilename: defaultName,
	})
	if err != nil {
		return "", err
	}
	if path == "" {
		return "", nil
	}
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		return "", err
	}
	return path, nil
}

// OpenTextFile prompts the user for a file via the native open dialog and
// returns the file's contents as a string. Path is returned so the caller
// can show "imported from …" feedback. Empty path + nil error = cancelled.
type OpenedFile struct {
	Path    string `json:"path"`
	Content string `json:"content"`
}

func (a *App) OpenTextFile(title string) (OpenedFile, error) {
	if title == "" {
		title = "Open File"
	}
	path, err := wailsRT.OpenFileDialog(a.ctx, wailsRT.OpenDialogOptions{
		Title: title,
	})
	if err != nil {
		return OpenedFile{}, err
	}
	if path == "" {
		return OpenedFile{}, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return OpenedFile{}, err
	}
	return OpenedFile{Path: path, Content: string(data)}, nil
}

// ─── File System Operations (File Manager dialog) ───

// FsEntry is one row of a directory listing.
type FsEntry struct {
	Name  string `json:"name"`
	IsDir bool   `json:"isDir"`
	Size  uint32 `json:"size"`
}

// FsStorageStatus reports which targets are online and their capacity.
// Missing/uninitialized targets have Available=false; frontend hides them.
type FsStorageStatus struct {
	FlashAvailable bool   `json:"flashAvailable"`
	FlashTotal     uint32 `json:"flashTotal"`
	FlashUsed      uint32 `json:"flashUsed"`
	FlashFree      uint32 `json:"flashFree"`

	SdAvailable bool   `json:"sdAvailable"`
	SdCardMB    uint32 `json:"sdCardMB"`
	SdTotalMB   uint32 `json:"sdTotalMB"`
	SdUsedMB    uint32 `json:"sdUsedMB"`
	SdFreeMB    uint32 `json:"sdFreeMB"`
	SdCardType  string `json:"sdCardType"`
	SdBusMode   string `json:"sdBusMode"`
}

// targetByte maps the frontend "flash" | "sd" string to the protocol byte.
func targetByte(target string) (byte, error) {
	switch target {
	case "flash":
		return hfxp.StorageTargetFlash, nil
	case "sd":
		return hfxp.StorageTargetSd, nil
	default:
		return 0, fmt.Errorf("unknown storage target: %s", target)
	}
}

// FsStorageStatus queries both flash and SD status. Always returns a status
// struct — unavailable targets simply have Available=false.
//
// Capability gating: when the firmware advertises CapFlash/CapSd in IDENTIFY
// (Rule 11 append-only field), we skip queries for missing interfaces — both
// to avoid round-trips and to give the frontend an authoritative "this board
// has no SD slot" signal. Legacy firmware (caps==0) falls through to the
// probe-everything behaviour for back-compat.
func (a *App) FsStorageStatus() (FsStorageStatus, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	out := FsStorageStatus{}
	a.echoCommand("flash.status")
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return out, fmt.Errorf("not connected")
	}

	caps := a.eng.Capabilities()
	probeFlash := caps == 0 || caps&core.CapFlash != 0
	probeSd := caps == 0 || caps&core.CapSd != 0

	if probeFlash {
		// Flash (present on every controller that registers StorageServer).
		if res := a.eng.API.HubFx.FlashStatus(); res.OK && res.Response != nil {
			p := res.Response.Payload
			switch {
			case len(p) >= 13 && p[0] != 0:
				out.FlashAvailable = true
				out.FlashTotal = protocol.ReadU32LE(p, 1)
				out.FlashUsed = protocol.ReadU32LE(p, 5)
				out.FlashFree = protocol.ReadU32LE(p, 9)
				a.echoOutput(fmt.Sprintf("flash: total=%d used=%d free=%d",
					out.FlashTotal, out.FlashUsed, out.FlashFree))
			case len(p) >= 1 && p[0] == 0:
				a.echoOutput(fmt.Sprintf("flash: not initialised on device (payload len=%d)", len(p)))
			default:
				a.echoOutput(fmt.Sprintf("flash: unexpected response (len=%d)", len(p)))
			}
		} else {
			msg := "no response"
			if res.Error != "" {
				msg = res.Error
			}
			a.echoError("flash.status failed: %s", msg)
		}
	} else {
		a.echoOutput("flash: not advertised by board")
	}

	a.echoCommand("sd.status")
	if probeSd {
		// SD (NACK'd with NOT_SUPPORTED on boards with no slot — treat as unavailable)
		if res := a.eng.API.HubFx.SdStatus(); res.OK && res.Response != nil {
			p := res.Response.Payload
			if len(p) >= 1 && p[0] != 0 {
				out.SdAvailable = true
				if len(p) >= 14 {
					out.SdCardMB = protocol.ReadU32LE(p, 1)
					out.SdTotalMB = protocol.ReadU32LE(p, 5)
					out.SdFreeMB = protocol.ReadU32LE(p, 9)
				}
				if len(p) >= 20 {
					cardTypes := map[byte]string{0: "NONE", 1: "MMC", 2: "SD", 3: "SDHC", 4: "UNKNOWN"}
					busModes := map[byte]string{0: "SPI", 1: "SDIO 1-bit", 2: "SDIO 4-bit"}
					out.SdCardType = cardTypes[p[14]]
					out.SdBusMode = busModes[p[15]]
					out.SdUsedMB = protocol.ReadU32LE(p, 16)
				}
				a.echoOutput(fmt.Sprintf("sd: card=%s bus=%s total=%dMB used=%dMB free=%dMB",
					out.SdCardType, out.SdBusMode, out.SdTotalMB, out.SdUsedMB, out.SdFreeMB))
			} else {
				a.echoOutput("sd: not available")
			}
		} else {
			a.echoOutput("sd: not supported")
		}
	} else {
		a.echoOutput("sd: not advertised by board")
	}
	return out, nil
}

// DeviceCapabilities returns the bitmask of CoreCapability flags the
// connected board advertised in IDENTIFY/INIT_READY. Returns 0 when no
// device is connected, identification hasn't completed yet, or the firmware
// pre-dates the capabilities field — frontends should treat 0 as "unknown,
// fall back to probing" rather than "nothing supported".
func (a *App) DeviceCapabilities() uint32 {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.eng.Capabilities()
}

// FsList returns a directory listing. Parses the server's
// "d|f\tname\tsize\n" text output into structured entries.
func (a *App) FsList(target, path string) ([]FsEntry, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.list %s %s", target, path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return nil, fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return nil, err
	}
	text, err := a.eng.API.Files.List(tb, path)
	if err != nil {
		a.echoError("%v", err)
		return nil, err
	}
	a.echoOutput(text)

	entries := []FsEntry{}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" {
			continue
		}
		parts := strings.Split(line, "\t")
		if len(parts) < 3 {
			continue
		}
		var size uint32
		fmt.Sscanf(parts[2], "%d", &size)
		entries = append(entries, FsEntry{
			Name:  parts[1],
			IsDir: parts[0] == "d",
			Size:  size,
		})
	}
	return entries, nil
}

// FsMkdir creates a directory on the selected target.
func (a *App) FsMkdir(target, path string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.mkdir %s %s", target, path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return err
	}
	res := a.eng.API.Files.Mkdir(tb, path, true) // GUI always uses mkdir -p (idempotent, creates ancestors)
	if !res.OK {
		a.echoError("mkdir failed: %s", res.Error)
		return fmt.Errorf("mkdir failed: %s", res.Error)
	}
	a.echoOK("Mkdir %s:%s", target, path)
	return nil
}

// FsDelete removes a file or directory (recursive by default for directories).
func (a *App) FsDelete(target, path string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.delete -r %s %s", target, path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return err
	}
	res := a.eng.API.Files.Delete(tb, path, true) // GUI delete is always recursive
	if !res.OK {
		a.echoError("delete failed: %s", res.Error)
		return fmt.Errorf("delete failed: %s", res.Error)
	}
	a.echoOK("Delete %s:%s", target, path)
	return nil
}

// FsDownloadToDisk pulls a file off the device and writes it to localPath.
// If localPath is empty, opens a Save dialog.
// Emits "fs:progress" events during transfer.
func (a *App) FsDownloadToDisk(target, remotePath, localPath string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.eng.Conn == nil {
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		return err
	}

	if localPath == "" {
		suggested := remotePath
		if i := strings.LastIndex(suggested, "/"); i >= 0 {
			suggested = suggested[i+1:]
		}
		chosen, err := wailsRT.SaveFileDialog(a.ctx, wailsRT.SaveDialogOptions{
			Title:           "Save " + remotePath,
			DefaultFilename: suggested,
		})
		if err != nil {
			return err
		}
		if chosen == "" {
			return fmt.Errorf("cancelled")
		}
		localPath = chosen
	}

	a.echoCommand(fmt.Sprintf("file.download %s %s", target, remotePath))
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "downloading", "path": remotePath, "sent": 0, "total": 0,
	})
	result, err := a.eng.API.Files.Download(tb, remotePath, 60*time.Second)
	if err != nil {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": err.Error()})
		a.echoError("%v", err)
		return err
	}
	if err := os.WriteFile(localPath, result.Data, 0644); err != nil {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": err.Error()})
		a.echoError("write %s: %v", localPath, err)
		return err
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "done", "path": remotePath, "sent": len(result.Data), "total": len(result.Data),
	})
	a.echoOK("Downloaded %s (%d bytes) → %s", remotePath, len(result.Data), localPath)
	return nil
}

// FsUploadFromDisk reads localPath and uploads it to remotePath on the target.
// Mode is auto-picked via api.PickUploadMode: flash always sync, SD switches
// to batch (UploadStream) above api.LargeFileBatchThreshold.
// If localPath is empty, opens an Open dialog.
func (a *App) FsUploadFromDisk(target, remotePath, localPath string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.eng.Conn == nil {
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		return err
	}

	if localPath == "" {
		chosen, err := wailsRT.OpenFileDialog(a.ctx, wailsRT.OpenDialogOptions{
			Title: "Upload file to " + target,
		})
		if err != nil {
			return err
		}
		if chosen == "" {
			return fmt.Errorf("cancelled")
		}
		localPath = chosen
	}

	data, err := os.ReadFile(localPath)
	if err != nil {
		return err
	}

	mode := api.PickUploadMode(tb, len(data))
	modeName := uploadModeName(mode)

	a.echoCommand(fmt.Sprintf("file.upload %s %s (%s, %d bytes from %s)",
		target, remotePath, modeName, len(data), localPath))
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "uploading", "path": remotePath, "sent": 0, "total": len(data),
	})
	res := a.eng.API.Files.Upload(tb, remotePath, data, mode, func(sent, total int) {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
			"phase": "uploading", "path": remotePath, "sent": sent, "total": total,
		})
	})
	if !res.OK {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": res.Error})
		a.echoError("upload failed: %s", res.Error)
		return fmt.Errorf("upload failed: %s", res.Error)
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase":    "done",
		"path":     remotePath,
		"sent":     int(res.BytesTransferred),
		"total":    int(res.BytesTransferred),
		"md5Match": res.MD5Match,
		"speedKBs": res.SpeedKBs,
	})
	md5tag := ""
	if res.MD5Match {
		md5tag = " ✓ MD5 match"
	}
	a.echoOK("Uploaded %s (%d bytes, %.1f KB/s)%s",
		remotePath, res.BytesTransferred, res.SpeedKBs, md5tag)
	return nil
}

// uploadModeName returns the short label used in echoes / logs.
func uploadModeName(m api.UploadMode) string {
	switch m {
	case api.UploadStream:
		return "batch"
	case api.UploadSync:
		return "sync"
	default:
		return fmt.Sprintf("mode=%d", m)
	}
}

// FsUploadBatch uploads a list of files or directory trees to target under
// remoteCwd. Each entry in localPaths is a disk path; files land at
// remoteCwd/<basename>, directories are walked recursively preserving tree
// structure (e.g. dropping "a/b/log.txt" while cwd=/tmp uploads to
// /tmp/a/b/log.txt). Progress events on "fs:progress" are enriched with
// batchIndex/batchTotal (file count) and batchBytesSent/batchBytesTotal.
// Mode is auto-picked per file via api.PickUploadMode: flash always sync,
// SD switches to batch above api.LargeFileBatchThreshold.
func (a *App) FsUploadBatch(target, remoteCwd string, localPaths []string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.eng.Conn == nil {
		a.emitBatchError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.emitBatchError(err.Error())
		return err
	}
	if len(localPaths) == 0 {
		a.emitBatchError("nothing to upload")
		return fmt.Errorf("nothing to upload")
	}

	type job struct {
		local  string
		remote string
		size   int64
	}

	// Walk every input path, collect files + directories (preserving tree).
	jobs := make([]job, 0, len(localPaths))
	dirs := make([]string, 0)
	dirSeen := map[string]bool{}
	var totalBytes int64

	// addDir records a directory we'll create via mkdir -p (idempotent;
	// firmware resolves missing ancestors). Dedupe skips redundant calls.
	addDir := func(remote string) {
		remote = normalizeRemote(remote)
		if remote == "" || remote == "/" || dirSeen[remote] {
			return
		}
		dirSeen[remote] = true
		dirs = append(dirs, remote)
	}

	for _, lp := range localPaths {
		info, err := os.Stat(lp)
		if err != nil {
			a.emitBatchError(fmt.Sprintf("stat %s: %v", lp, err))
			return err
		}
		base := filepath.Base(lp)
		if info.IsDir() {
			rootRemote := joinRemote(remoteCwd, base)
			addDir(rootRemote)
			err := filepath.Walk(lp, func(p string, fi os.FileInfo, werr error) error {
				if werr != nil {
					return werr
				}
				rel, rerr := filepath.Rel(lp, p)
				if rerr != nil {
					return rerr
				}
				rel = filepath.ToSlash(rel)
				if rel == "." {
					return nil
				}
				remote := joinRemote(rootRemote, rel)
				if fi.IsDir() {
					addDir(remote)
					return nil
				}
				jobs = append(jobs, job{local: p, remote: remote, size: fi.Size()})
				totalBytes += fi.Size()
				return nil
			})
			if err != nil {
				a.emitBatchError(fmt.Sprintf("walk %s: %v", lp, err))
				return err
			}
		} else {
			remote := joinRemote(remoteCwd, base)
			jobs = append(jobs, job{local: lp, remote: remote, size: info.Size()})
			totalBytes += info.Size()
			// Top-level file lands under remoteCwd — ensure the cwd exists.
			addDir(remoteCwd)
		}
	}

	if len(jobs) == 0 {
		a.emitBatchError("no files to upload (only empty directories)")
		return fmt.Errorf("no files to upload")
	}

	a.echoCommand(fmt.Sprintf("file.upload-batch %s %s (%d files, %s, mode=auto)",
		target, remoteCwd, len(jobs), humanBytes(uint64(totalBytes))))
	// mkdir -p per directory — firmware resolves ancestors, and the call is
	// idempotent when the directory already exists.
	for _, d := range dirs {
		res := a.eng.API.Files.Mkdir(tb, d, true)
		if !res.OK {
			a.echoError("mkdir -p %s failed: %s", d, res.Error)
			a.emitBatchError(fmt.Sprintf("mkdir %s: %s", d, res.Error))
			return fmt.Errorf("mkdir %s: %s", d, res.Error)
		}
	}

	// Upload loop. Emit progress with per-file + batch counters.
	var bytesDoneBefore int64 = 0
	for idx, j := range jobs {
		data, err := os.ReadFile(j.local)
		if err != nil {
			a.emitBatchError(fmt.Sprintf("read %s: %v", j.local, err))
			return err
		}
		mode := api.PickUploadMode(tb, len(data))
		a.echoOutput(fmt.Sprintf("[%d/%d] %s → %s (%s, %s)",
			idx+1, len(jobs), j.local, j.remote, humanBytes(uint64(len(data))), uploadModeName(mode)))
		a.emitBatchProgress("uploading", j.remote, 0, len(data),
			idx+1, len(jobs), bytesDoneBefore, totalBytes, 0, nil)

		res := a.eng.API.Files.Upload(tb, j.remote, data, mode, func(sent, total int) {
			a.emitBatchProgress("uploading", j.remote, sent, total,
				idx+1, len(jobs), bytesDoneBefore+int64(sent), totalBytes, 0, nil)
		})
		if !res.OK {
			a.echoError("upload %s failed: %s", j.remote, res.Error)
			a.emitBatchError(fmt.Sprintf("%s: %s", j.remote, res.Error))
			return fmt.Errorf("upload %s: %s", j.remote, res.Error)
		}
		bytesDoneBefore += int64(res.BytesTransferred)
		md5 := res.MD5Match
		a.emitBatchProgress("uploading", j.remote,
			int(res.BytesTransferred), int(res.BytesTransferred),
			idx+1, len(jobs), bytesDoneBefore, totalBytes, res.SpeedKBs, &md5)
		md5tag := ""
		if res.MD5Match {
			md5tag = " ✓ MD5"
		}
		a.echoOK("  uploaded %s (%.1f KB/s)%s", j.remote, res.SpeedKBs, md5tag)
	}

	a.emitBatchProgress("done", "", int(totalBytes), int(totalBytes),
		len(jobs), len(jobs), totalBytes, totalBytes, 0, nil)
	a.echoOK("batch complete: %d file(s), %s",
		len(jobs), humanBytes(uint64(totalBytes)))
	return nil
}

// joinRemote appends a slash-separated segment to a remote path.
func joinRemote(dir, name string) string {
	name = filepath.ToSlash(name)
	name = strings.TrimLeft(name, "/")
	if dir == "" || dir == "/" {
		return "/" + name
	}
	return strings.TrimRight(dir, "/") + "/" + name
}

func normalizeRemote(p string) string {
	p = filepath.ToSlash(p)
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	return p
}

func humanBytes(n uint64) string {
	switch {
	case n < 1024:
		return fmt.Sprintf("%d B", n)
	case n < 1024*1024:
		return fmt.Sprintf("%.1f KB", float64(n)/1024)
	case n < 1024*1024*1024:
		return fmt.Sprintf("%.1f MB", float64(n)/1048576)
	default:
		return fmt.Sprintf("%.2f GB", float64(n)/1073741824)
	}
}

func (a *App) emitBatchProgress(phase, path string, sent, total, batchIdx, batchTotal int,
	batchBytesSent, batchBytesTotal int64, speedKBs float64, md5Match *bool,
) {
	ev := map[string]any{
		"phase":           phase,
		"path":            path,
		"sent":            sent,
		"total":           total,
		"batchIndex":      batchIdx,
		"batchTotal":      batchTotal,
		"batchBytesSent":  batchBytesSent,
		"batchBytesTotal": batchBytesTotal,
	}
	if speedKBs > 0 {
		ev["speedKBs"] = speedKBs
	}
	if md5Match != nil {
		ev["md5Match"] = *md5Match
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", ev)
}

func (a *App) emitBatchError(msg string) {
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "error", "error": msg,
	})
}

// ─── Firmware Operations (GUI bindings) ───

// FirmwareTarget describes a controller target for the GUI dropdown.
type FirmwareTarget struct {
	Name     string `json:"name"`
	Platform string `json:"platform"`
	SubDir   string `json:"subDir"`
}

// FirmwareProgress is emitted as "firmware:progress" events.
type FirmwareProgress struct {
	Step         int    `json:"step"`
	Total        int    `json:"total"`
	Message      string `json:"message"`
	Type         string `json:"type"` // "info", "ok", "warning", "error", "step"
	Done         bool   `json:"done"`
	Error        string `json:"error,omitempty"`
	Reconnecting bool   `json:"reconnecting,omitempty"` // true while waiting for port & reconnecting
}

// GetFirmwareTargets returns all available controller targets.
func (a *App) GetFirmwareTargets() []FirmwareTarget {
	var targets []FirmwareTarget
	for _, name := range firmware.ControllerNames() {
		ctrl := firmware.Controllers[name]
		platform := "Pico (UF2)"
		if ctrl.IsESP32() {
			platform = "ESP32-S3 (UART)"
		}
		targets = append(targets, FirmwareTarget{
			Name:     name,
			Platform: platform,
			SubDir:   ctrl.SubDir,
		})
	}
	return targets
}

// GetFirmwareVersion reads the current version from source for a controller.
func (a *App) GetFirmwareVersion(controllerName string) map[string]interface{} {
	ctrl, ok := firmware.Controllers[controllerName]
	if !ok {
		return map[string]interface{}{"error": "unknown controller"}
	}
	opts := &firmware.Options{Controller: controllerName}
	version, buildNum, err := firmware.ExtractVersion(opts, ctrl)
	if err != nil {
		return map[string]interface{}{"error": err.Error()}
	}
	return map[string]interface{}{
		"version": version,
		"build":   buildNum,
	}
}

// BuildAndFlash runs the full build → flash → verify pipeline in the background.
// Progress emitted as "firmware:progress" Wails events.
func (a *App) BuildAndFlash(controllerName string, port string, noBuild bool, noClean bool, skipVerify bool) {
	go a.runFirmwareOp(controllerName, port, noBuild, noClean, skipVerify)
}

func (a *App) runFirmwareOp(controllerName string, port string, noBuild bool, noClean bool, skipVerify bool) {
	// Disconnect if we're connected (flash needs the port)
	savedPort := ""
	a.mu.Lock()
	if a.eng.Conn != nil {
		connPort := a.eng.Conn.PortName()
		if port == "" || strings.EqualFold(connPort, port) {
			savedPort = connPort
			a.emitFwProgress("warning", "Disconnecting from "+connPort+"...")
			a.eng.Dispatch("disconnect")
			// OnDisconnect callback emits connection:changed automatically
		}
	}
	a.mu.Unlock()

	opts := &firmware.Options{
		Controller: controllerName,
		Port:       port,
		NoBuild:    noBuild,
		NoClean:    noClean,
		SkipVerify: skipVerify,
		Timeout:    15,
		OnEvent: func(evt firmware.Event) {
			typ := "info"
			switch evt.Type {
			case firmware.EventOK:
				typ = "ok"
			case firmware.EventWarning:
				typ = "warning"
			case firmware.EventError:
				typ = "error"
			case firmware.EventStep:
				typ = "step"
			}
			wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
				Step:    evt.Step,
				Total:   evt.Total,
				Message: evt.Message,
				Type:    typ,
			})
		},
	}

	err := firmware.Run(opts)

	if err != nil {
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: err.Error(),
			Type:    "error",
			Done:    true,
			Error:   err.Error(),
		})
	} else {
		a.reconnectAfterFlash(savedPort)
	}
}

func (a *App) emitFwProgress(typ string, msg string) {
	wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
		Message: msg,
		Type:    typ,
	})
}

// reconnectAfterFlash waits for the serial port to reappear after a flash
// operation, reconnects, and emits the final done event.
func (a *App) reconnectAfterFlash(savedPort string) {
	if savedPort == "" {
		// No prior connection — just report success
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: "Flash complete!",
			Type:    "ok",
			Done:    true,
		})
		return
	}

	// Tell the frontend we're in the reconnect phase
	wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
		Message:      "Flash complete — waiting for " + savedPort + " to reappear...",
		Type:         "info",
		Reconnecting: true,
	})

	// Wait up to 10 seconds for the port to come back
	deadline := time.Now().Add(10 * time.Second)
	found := false
	for time.Now().Before(deadline) {
		time.Sleep(500 * time.Millisecond)
		for _, p := range protocol.ListPorts() {
			if strings.EqualFold(p, savedPort) {
				found = true
				break
			}
		}
		if found {
			break
		}
	}

	if !found {
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: "Flash complete! Port " + savedPort + " did not reappear — reconnect manually",
			Type:    "warning",
			Done:    true,
		})
		return
	}

	// Small extra delay for device to stabilize after USB re-enumeration
	time.Sleep(500 * time.Millisecond)

	wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
		Message:      "Reconnecting to " + savedPort + "...",
		Type:         "info",
		Reconnecting: true,
	})

	a.mu.Lock()
	a.eng.Dispatch("connect " + savedPort)
	// Auto-init slave controllers
	if a.eng.Conn != nil && !a.eng.Initialized && a.eng.ControllerType != "" {
		a.eng.Dispatch("init")
	}
	info := a.getConnectionInfo()
	a.mu.Unlock()

	wailsRT.EventsEmit(a.ctx, "connection:changed", info)

	if info.Connected {
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: "Flash complete — reconnected to " + info.ControllerName + " " + info.FirmwareVer,
			Type:    "ok",
			Done:    true,
		})
	} else {
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: "Flash complete! Reconnect to " + savedPort + " failed — try manually",
			Type:    "warning",
			Done:    true,
		})
	}
}

// ─── Remote Firmware Releases (GUI bindings) ───

// ReleaseInfo is the frontend-friendly release representation.
type ReleaseInfo struct {
	Controller string `json:"controller"`
	Version    string `json:"version"`
	Tag        string `json:"tag"`
	Name       string `json:"name"`
	Body       string `json:"body"` // release notes (markdown)
	Prerelease bool   `json:"prerelease"`
	Published  string `json:"published"`
	AssetName  string `json:"assetName"`
	AssetSize  int64  `json:"assetSize"`
}

// GetReleases fetches available firmware releases from GitHub.
// Pass controller="" to get all, or a specific name like "gunfx".
func (a *App) GetReleases(controller string) []ReleaseInfo {
	releases, err := firmware.FetchReleases(controller, nil)
	if err != nil {
		a.emitFwProgress("error", "Failed to fetch releases: "+err.Error())
		return []ReleaseInfo{}
	}

	var result []ReleaseInfo
	for _, r := range releases {
		result = append(result, ReleaseInfo{
			Controller: r.Controller,
			Version:    r.Version,
			Tag:        r.Tag,
			Name:       r.Name,
			Body:       r.Body,
			Prerelease: r.Prerelease,
			Published:  r.Published,
			AssetName:  r.AssetName,
			AssetSize:  r.AssetSize,
		})
	}
	return result
}

// FlashFromRelease downloads a release from GitHub and flashes it to the board.
// Progress emitted as "firmware:progress" Wails events.
func (a *App) FlashFromRelease(controller string, tag string, port string, skipVerify bool) {
	go a.runReleaseFlash(controller, tag, port, skipVerify)
}

// ─── External Tools (GUI bindings) ───

// ToolsStatus describes the state of external tools (esptool, etc.).
type ToolsStatus struct {
	EsptoolInstalled bool   `json:"esptoolInstalled"`
	EsptoolPath      string `json:"esptoolPath"`
	EsptoolSource    string `json:"esptoolSource"` // "workspace", "colocated", "path", "python", or ""
}

// GetToolsStatus checks whether external tools (esptool) are available.
func (a *App) GetToolsStatus() ToolsStatus {
	opts := &firmware.Options{}
	info := firmware.ResolveEsptoolOrPython(opts)
	if info == nil {
		return ToolsStatus{}
	}
	return ToolsStatus{
		EsptoolInstalled: true,
		EsptoolPath:      info.Path,
		EsptoolSource:    info.Source,
	}
}

// DownloadEsptool downloads the standalone esptool binary (no Python needed).
// Progress emitted as "firmware:progress" events.
func (a *App) DownloadEsptool() {
	go func() {
		opts := &firmware.Options{
			OnEvent: func(evt firmware.Event) {
				typ := "info"
				switch evt.Type {
				case firmware.EventOK:
					typ = "ok"
				case firmware.EventError:
					typ = "error"
				}
				wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
					Message: evt.Message,
					Type:    typ,
				})
			},
		}

		path, err := firmware.DownloadEsptool(opts)
		if err != nil {
			wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
				Message: "esptool download failed: " + err.Error(),
				Type:    "error",
				Done:    true,
				Error:   err.Error(),
			})
			return
		}

		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: "esptool installed: " + path,
			Type:    "ok",
			Done:    true,
		})
	}()
}

func (a *App) runReleaseFlash(controller string, tag string, port string, skipVerify bool) {
	savedPort := ""

	// Find the release
	releases, err := firmware.FetchReleases(controller, nil)
	if err != nil {
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: "Failed to fetch releases: " + err.Error(),
			Type:    "error",
			Done:    true,
			Error:   err.Error(),
		})
		return
	}

	var rel *firmware.Release
	for i, r := range releases {
		if r.Tag == tag {
			rel = &releases[i]
			break
		}
	}
	if rel == nil {
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: "Release not found: " + tag,
			Type:    "error",
			Done:    true,
			Error:   "release not found",
		})
		return
	}

	// Disconnect if needed
	a.mu.Lock()
	if a.eng.Conn != nil {
		connPort := a.eng.Conn.PortName()
		if port == "" || strings.EqualFold(connPort, port) {
			savedPort = connPort
			a.emitFwProgress("warning", "Disconnecting from "+connPort+"...")
			a.eng.Dispatch("disconnect")
			// OnDisconnect callback emits connection:changed automatically
		}
	}
	a.mu.Unlock()

	opts := &firmware.Options{
		Controller: controller,
		Port:       port,
		SkipVerify: skipVerify,
		Timeout:    15,
		OnEvent: func(evt firmware.Event) {
			typ := "info"
			switch evt.Type {
			case firmware.EventOK:
				typ = "ok"
			case firmware.EventWarning:
				typ = "warning"
			case firmware.EventError:
				typ = "error"
			case firmware.EventStep:
				typ = "step"
			case firmware.EventProgress:
				typ = "info"
			}
			wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
				Step:    evt.Step,
				Total:   evt.Total,
				Message: evt.Message,
				Type:    typ,
			})
		},
	}

	err = firmware.FlashRelease(*rel, opts)

	if err != nil {
		wailsRT.EventsEmit(a.ctx, "firmware:progress", FirmwareProgress{
			Message: err.Error(),
			Type:    "error",
			Done:    true,
			Error:   err.Error(),
		})
	} else {
		a.reconnectAfterFlash(savedPort)
	}
}

