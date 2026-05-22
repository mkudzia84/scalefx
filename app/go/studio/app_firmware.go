package main

// Firmware operations exposed to the GUI: build/flash, release fetch, esptool
// install. Progress is reported through the "firmware:progress" Wails event;
// reconnect-after-flash logic lives here too because it shares the helpers.

import (
	"scalefx/firmware"
	"scalefx/protocol"
	"strings"
	"time"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// ─── Types ───

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

// FirmwareVersionInfo is the typed return for GetFirmwareVersion.
type FirmwareVersionInfo struct {
	Version string `json:"version,omitempty"`
	Build   int    `json:"build,omitempty"`
	Error   string `json:"error,omitempty"`
}

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

// ToolsStatus describes the state of external tools (esptool, etc.).
type ToolsStatus struct {
	EsptoolInstalled bool   `json:"esptoolInstalled"`
	EsptoolPath      string `json:"esptoolPath"`
	EsptoolSource    string `json:"esptoolSource"` // "workspace", "colocated", "path", "python", or ""
}

// ─── Build / Flash (GUI bindings) ───

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
func (a *App) GetFirmwareVersion(controllerName string) FirmwareVersionInfo {
	ctrl, ok := firmware.Controllers[controllerName]
	if !ok {
		return FirmwareVersionInfo{Error: "unknown controller"}
	}
	opts := &firmware.Options{Controller: controllerName}
	version, buildNum, err := firmware.ExtractVersion(opts, ctrl)
	if err != nil {
		return FirmwareVersionInfo{Error: err.Error()}
	}
	return FirmwareVersionInfo{Version: version, Build: buildNum}
}

// BuildAndFlash runs the full build → flash → verify pipeline in the background.
// Progress emitted as "firmware:progress" Wails events.
func (a *App) BuildAndFlash(controllerName string, port string, noBuild bool, noClean bool, skipVerify bool) {
	a.diag.With(LvlInfo, "FW", "BuildAndFlash requested",
		map[string]any{"controller": controllerName, "port": port,
			"noBuild": noBuild, "noClean": noClean, "skipVerify": skipVerify})
	go a.runFirmwareOp(controllerName, port, noBuild, noClean, skipVerify)
}

func (a *App) runFirmwareOp(controllerName string, port string, noBuild bool, noClean bool, skipVerify bool) {
	// Disconnect if we're connected (flash needs the port)
	savedPort := ""
	a.mu.Lock()
	if a.c != nil {
		connPort := a.c.PortName()
		if port == "" || strings.EqualFold(connPort, port) {
			savedPort = connPort
			a.emitFwProgress("warning", "Disconnecting from "+connPort+"...")
			a.closeLocked()
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
		OnEvent:    a.firmwareEventForward,
	}

	err := firmware.Run(opts)

	if err != nil {
		a.emitFwError(err.Error())
	} else {
		a.reconnectAfterFlash(savedPort)
	}
}

// ─── Progress Helpers ───

func (a *App) emitFw(p FirmwareProgress) {
	wailsRT.EventsEmit(a.ctx, "firmware:progress", p)
}

func (a *App) emitFwProgress(typ string, msg string) {
	a.emitFw(FirmwareProgress{Message: msg, Type: typ})
}

// emitFwError emits a terminal error event (Type=error, Done=true, Error=msg).
func (a *App) emitFwError(msg string) {
	a.emitFw(FirmwareProgress{Message: msg, Type: "error", Done: true, Error: msg})
}

// emitFwDone emits a terminal event (Done=true) with the given type and message.
func (a *App) emitFwDone(typ string, msg string) {
	a.emitFw(FirmwareProgress{Message: msg, Type: typ, Done: true})
}

// emitFwReconnect emits an info event with Reconnecting=true (mid-flash phase).
func (a *App) emitFwReconnect(msg string) {
	a.emitFw(FirmwareProgress{Message: msg, Type: "info", Reconnecting: true})
}

// firmwareEventType converts a firmware.EventType to the Wails-event "type" string.
func firmwareEventType(t firmware.EventType) string {
	switch t {
	case firmware.EventOK:
		return "ok"
	case firmware.EventWarning:
		return "warning"
	case firmware.EventError:
		return "error"
	case firmware.EventStep:
		return "step"
	default:
		return "info"
	}
}

// firmwareEventForward forwards a firmware.Event as a "firmware:progress" Wails event.
func (a *App) firmwareEventForward(evt firmware.Event) {
	a.emitFw(FirmwareProgress{
		Step:    evt.Step,
		Total:   evt.Total,
		Message: evt.Message,
		Type:    firmwareEventType(evt.Type),
	})
}

// reconnectAfterFlash waits for the serial port to reappear after a flash
// operation, reconnects, and emits the final done event.
func (a *App) reconnectAfterFlash(savedPort string) {
	if savedPort == "" {
		a.emitFwDone("ok", "Flash complete!")
		return
	}

	a.emitFwReconnect("Flash complete — waiting for " + savedPort + " to reappear...")

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
		a.emitFwDone("warning", "Flash complete! Port "+savedPort+" did not reappear — reconnect manually")
		return
	}

	// Small extra delay for device to stabilize after USB re-enumeration
	time.Sleep(500 * time.Millisecond)

	a.emitFwReconnect("Reconnecting to " + savedPort + "...")

	a.mu.Lock()
	if err := a.openLocked(savedPort); err != nil {
		a.diag.Warn("FW", "reconnect to %s failed: %v", savedPort, err)
	}
	info := a.getConnectionInfo()
	a.mu.Unlock()

	wailsRT.EventsEmit(a.ctx, "connection:changed", info)

	if info.Connected {
		a.emitFwDone("ok", "Flash complete — reconnected to "+info.ControllerName+" "+info.FirmwareVer)
	} else {
		a.emitFwDone("warning", "Flash complete! Reconnect to "+savedPort+" failed — try manually")
	}
}

// ─── Remote Firmware Releases (GUI bindings) ───

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
	a.diag.With(LvlInfo, "FW", "FlashFromRelease requested",
		map[string]any{"controller": controller, "tag": tag, "port": port, "skipVerify": skipVerify})
	go a.runReleaseFlash(controller, tag, port, skipVerify)
}

func (a *App) runReleaseFlash(controller string, tag string, port string, skipVerify bool) {
	savedPort := ""

	// Find the release
	releases, err := firmware.FetchReleases(controller, nil)
	if err != nil {
		a.emitFwError("Failed to fetch releases: " + err.Error())
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
		a.emitFwError("Release not found: " + tag)
		return
	}

	// Disconnect if needed
	a.mu.Lock()
	if a.c != nil {
		connPort := a.c.PortName()
		if port == "" || strings.EqualFold(connPort, port) {
			savedPort = connPort
			a.emitFwProgress("warning", "Disconnecting from "+connPort+"...")
			a.closeLocked()
		}
	}
	a.mu.Unlock()

	opts := &firmware.Options{
		Controller: controller,
		Port:       port,
		SkipVerify: skipVerify,
		Timeout:    15,
		OnEvent:    a.firmwareEventForward,
	}

	err = firmware.FlashRelease(*rel, opts)

	if err != nil {
		a.emitFwError(err.Error())
	} else {
		a.reconnectAfterFlash(savedPort)
	}
}

// ─── External Tools (GUI bindings) ───

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
		opts := &firmware.Options{OnEvent: a.firmwareEventForward}

		path, err := firmware.DownloadEsptool(opts)
		if err != nil {
			a.emitFwError("esptool download failed: " + err.Error())
			return
		}

		a.emitFwDone("ok", "esptool installed: "+path)
	}()
}
