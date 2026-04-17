package main

import (
	"context"
	"scalefx/engine"
	"scalefx/engine/handlers"
	"scalefx/engine/handlers/gearcontrol"
	"scalefx/engine/handlers/gunfx"
	"scalefx/engine/handlers/hubfx"
	"scalefx/engine/handlers/lightfx"
	"scalefx/firmware"
	"scalefx/protocol"
	"scalefx/protocol/core"
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
	}
	return info
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

