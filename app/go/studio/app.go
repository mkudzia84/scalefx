package main

// Studio backend (Wails v2).  The App struct is the single Wails-bound
// type: each method here becomes a JS-callable binding.  Domain-specific
// bindings live in app_config.go (config round-trip), app_files.go (File
// Manager), app_topology.go (capabilities / system-info) and
// app_firmware.go (build / flash / releases).  This file owns the
// connection lifecycle, port watcher, and console echoes.
//
// Built on the typed `scalefx/client` API (the same one the CLI uses).
// The legacy `scalefx/engine` string-command dispatcher + per-board
// `handlers` observer framework were retired with the generic-expander
// migration; per-board control returns later, rebuilt on the topology
// surface rather than the deprecated slave-STATUS handlers.

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"scalefx/client"
	"scalefx/console"
	"scalefx/devicemodel"
	"scalefx/protocol"

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
	// Capabilities is the bitmask the firmware advertised in
	// IDENTIFY / INIT_READY (mirrors core.Cap*).
	Capabilities uint32 `json:"capabilities"`
}

// controllerLabels maps a board kind to a human-readable display name.
var controllerLabels = map[string]string{
	"hubfx":       "HubFX",
	"lightfx":     "LightFX",
	"gunfx":       "GunFX",
	"gearcontrol": "GearControl",
	"noop":        "NoOp",
}

func controllerLabel(kind string) string {
	if l, ok := controllerLabels[kind]; ok {
		return l
	}
	return kind
}

// ─── App struct ───

type App struct {
	ctx  context.Context
	diag *Diag
	mu   sync.Mutex

	// Live client (nil when disconnected) + cached IDENTIFY.
	c             *client.Client
	id            client.Identity
	kind          string // board kind ("hubfx"/"lightfx"/…) or ""
	initialized   bool
	connectedPort string // port name we Open()'d — for disconnect detection

	// Shared command session (drives the same command set as the CLI;
	// output is rendered into the GUI console).  See app_console.go.
	con *console.App

	// Serial-port enumeration + disconnect detection (see port_watcher.go).
	ports *PortWatcher

	// Working device model (ports + roles + domain claims).  Guarded by
	// dmMu; assembled from the topology snapshot.  See app_devicemodel.go.
	dm   *devicemodel.Model
	dmMu sync.Mutex

	// Per-input-port configuration (protocol, channel count, channel→
	// function map).  Guarded by dmMu alongside the model.  See app_input.go.
	inputs map[devicemodel.PortRef]*devicemodel.InputPortConfig

	// Operator-assigned port names (overlay; survives topology refresh).
	// Guarded by dmMu.
	portNames map[devicemodel.PortRef]string

	// Servo motion profile overlay per port — Rule 42 storage, Rule 44
	// editing surface.  Editing happens in feature panels (GunFx Turret
	// section); the profile is persisted into /hubfx.yaml's ports[]
	// block on `SaveHubConfig`, and pushed live via `ServoSetProfile`
	// debounced ~350 ms.  Guarded by dmMu.
	portProfiles map[devicemodel.PortRef]ServoMotionProfileDTO

	// `/hubfx.yaml` top-level blocks that Studio doesn't expose in any
	// UI yet but MUST round-trip through Save (else firmware defaults
	// kick in and silently disable effects on every Apply — that bit
	// us with `features.enginefx`/`features.gunfx` 2026-05-23).
	// Populated on LoadHubConfig, emitted on SaveHubConfig.  Nil when
	// the file had no such block; Save uses `defaultFeatures()` /
	// equivalent so a fresh first save doesn't kill anything.
	hubAudio     *yamlAudio
	hubFeatures  *yamlFeatures
	hubTelemetry *yamlTelemetry

	// Heartbeat goroutine
	stopHeartbeat chan<- struct{}
}

func NewApp() *App {
	return &App{
		diag:         NewDiag(),
		inputs:       map[devicemodel.PortRef]*devicemodel.InputPortConfig{},
		portNames:    map[devicemodel.PortRef]string{},
		portProfiles: map[devicemodel.PortRef]ServoMotionProfileDTO{},
	}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	a.diag.SetCtx(ctx)
	a.setupConsole()
	a.diag.Info("APP", "Studio starting up — go=%s os=%s build=studio",
		runtimeGoVersion(), runtimeOS())
	a.diag.Info("APP", "process pid=%d, working dir=%s", processPID(), workingDir())
	if p := a.diag.LogPath(); p != "" {
		a.diag.Info("APP", "diagnostic log: %s", p)
	}

	a.ports = NewPortWatcher(time.Second, a.onPortsChanged, a.onPortVanished)
	a.ports.Start()

	// Heartbeat: 1 line every 10 s while running.
	a.stopHeartbeat = a.diag.StartHeartbeat(10*time.Second, func() map[string]any {
		a.mu.Lock()
		connected := a.c != nil
		kind := a.kind
		init := a.initialized
		a.mu.Unlock()
		return map[string]any{
			"connected":   connected,
			"controller":  kind,
			"initialized": init,
		}
	})
}

func (a *App) shutdown(_ context.Context) {
	a.diag.Info("APP", "shutdown requested")
	if a.stopHeartbeat != nil {
		close(a.stopHeartbeat)
	}
	if a.ports != nil {
		a.ports.Stop()
	}
	a.mu.Lock()
	a.closeLocked()
	a.mu.Unlock()
}

// ─── Port Watcher callbacks ───

// onPortsChanged relays a port-list change to the frontend.  Wired into
// the PortWatcher at startup.
func (a *App) onPortsChanged(items []PortInfo, added, removed []string) {
	a.diag.With(LvlInfo, "PORTS", "port list changed",
		map[string]any{"added": added, "removed": removed, "total": len(items)})
	wailsRT.EventsEmit(a.ctx, "ports:changed", items)
}

// onPortVanished tears down the client when the port we hold disappears.
// The PortWatcher fires this once per disconnect for the watched port.
func (a *App) onPortVanished(port string) {
	a.mu.Lock()
	if a.c == nil || a.connectedPort != port {
		a.mu.Unlock()
		return
	}
	a.diag.Warn("CONN", "connected port %s vanished — disconnecting", port)
	a.closeLocked()
	info := a.getConnectionInfo()
	a.mu.Unlock()
	wailsRT.EventsEmit(a.ctx, "connection:changed", info)
}

// installAsyncDiag leaves a one-line breadcrumb in the diag log for every
// unsolicited packet.  STATUS broadcasts are logged once then suppressed.
// It subscribes through the client's Events facet (the single async
// owner) rather than hijacking the connection callback — additional
// consumers (live effect panels, the device-model refresher) attach the
// same way without clobbering each other.  A fresh client is created on
// every connect, so its subscriber list starts empty each time.
func (a *App) installAsyncDiag() {
	if a.c == nil {
		return
	}
	var statusSeen atomic.Bool
	a.c.Events.OnAny(func(r *protocol.Response) {
		if r == nil {
			return
		}
		ptype := byte(r.PacketType)
		pname := protocol.PacketTypeName(r.PacketType)
		isStatus := ptype == 0xF4 || ptype == 0xEF || ptype == 0xF3
		if isStatus {
			if statusSeen.CompareAndSwap(false, true) {
				a.diag.With(LvlInfo, "RX", "first STATUS broadcast received",
					map[string]any{"type": pname, "len": len(r.Payload)})
			}
			return
		}
		a.diag.With(LvlDebug, "RX", "async packet",
			map[string]any{"type": pname, "tag": r.Tag, "len": len(r.Payload)})
	})
	a.diag.Debug("CONN", "async packet observer installed")
}

// ─── Exposed Methods ───

func (a *App) ListPorts() []PortInfo {
	defer a.diag.Around("ListPorts", nil)()
	ports := protocol.ListPortsDetailed()
	result := make([]PortInfo, len(ports))
	for i, p := range ports {
		result[i] = PortInfo{Name: p.Name, Description: p.Description}
	}
	a.diag.With(LvlDebug, "PORTS", "ListPorts result", map[string]any{"count": len(result)})
	return result
}

func (a *App) Connect(port string) ConnectionInfo {
	defer a.diag.Around("Connect", map[string]any{"port": port})()
	a.mu.Lock()
	defer a.mu.Unlock()

	a.diag.Info("CONN", "Connecting to %s", port)
	if err := a.openLocked(port); err != nil {
		a.diag.Warn("CONN", "Connect to %s failed: %v", port, err)
		a.echoError("connect %s: %v", port, err)
		info := a.getConnectionInfo()
		wailsRT.EventsEmit(a.ctx, "connection:changed", info)
		return info
	}

	info := a.getConnectionInfo()
	a.diag.With(LvlInfo, "CONN", "connection state", map[string]any{
		"connected": info.Connected, "controller": info.ControllerType,
		"name": info.ControllerName, "version": info.FirmwareVer,
		"build": info.Build, "caps": info.Capabilities,
	})
	wailsRT.EventsEmit(a.ctx, "connection:changed", info)
	return info
}

func (a *App) Disconnect() ConnectionInfo {
	defer a.diag.Around("Disconnect", nil)()
	a.mu.Lock()
	defer a.mu.Unlock()

	a.diag.Info("CONN", "Disconnect requested")
	a.closeLocked()

	info := a.getConnectionInfo()
	wailsRT.EventsEmit(a.ctx, "connection:changed", info)
	return info
}

// SendCommand runs one console line through the shared `scalefx/console`
// command set (the same verbs the CLI exposes), rendering colored output
// into the GUI console.  `/diag …` slash commands and quit are handled
// locally; connection verbs are intercepted (Studio owns the connection).
func (a *App) SendCommand(cmd string) {
	trimmed := strings.TrimSpace(cmd)
	if trimmed == "" {
		return
	}
	if trimmed == "quit" || trimmed == "exit" || trimmed == "q" {
		a.diag.Info("CMD", "user typed `%s` — quitting", trimmed)
		wailsRT.Quit(a.ctx)
		return
	}
	if a.handleDiagSlash(trimmed) {
		return
	}
	a.dispatchConsole(trimmed)
}

// handleDiagSlash recognises `/diag …` slash commands.
func (a *App) handleDiagSlash(cmd string) bool {
	if !strings.HasPrefix(cmd, "/diag") {
		return false
	}
	parts := strings.Fields(cmd)
	switch {
	case len(parts) == 1:
		a.diag.Info("DIAG", "debug=%s, ring size=%d, run /diag dump to copy events",
			onOff(a.diag.DebugEnabled()), len(a.diag.Snapshot()))
	case len(parts) >= 2 && parts[1] == "debug":
		on := !a.diag.DebugEnabled()
		if len(parts) >= 3 {
			switch strings.ToLower(parts[2]) {
			case "on", "1", "true", "yes":
				on = true
			case "off", "0", "false", "no":
				on = false
			}
		}
		a.diag.SetDebug(on)
	case len(parts) >= 2 && parts[1] == "wire":
		// Toggle per-packet wire trace.  WireLogger is installed at
		// Connect; this just flips Connection.SetVerbose so the
		// callback fires (or stops firing) without reconnecting.
		c := a.snapshotClient()
		if c == nil {
			a.diag.Warn("DIAG", "/diag wire — not connected")
			return true
		}
		on := !c.Conn().Verbose()
		if len(parts) >= 3 {
			switch strings.ToLower(parts[2]) {
			case "on", "1", "true", "yes":
				on = true
			case "off", "0", "false", "no":
				on = false
			}
		}
		c.Conn().SetVerbose(on)
		a.diag.Info("DIAG", "wire trace %s (per-packet TX/RX via WIRE tag in console)", onOff(on))
	case len(parts) >= 2 && parts[1] == "dump":
		evs := a.diag.Snapshot()
		blob, _ := json.MarshalIndent(evs, "", "  ")
		wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{
			Type:    "output",
			Content: fmt.Sprintf(`<pre class="diag-dump">%s</pre>`, escapeHTML(string(blob))),
		})
		a.diag.Info("DIAG", "snapshot dumped (%d events)", len(evs))
	case len(parts) >= 2 && parts[1] == "clear":
		a.diag.mu.Lock()
		a.diag.ring = a.diag.ring[:0]
		a.diag.mu.Unlock()
		a.diag.Info("DIAG", "ring buffer cleared")
	default:
		a.diag.Warn("DIAG", "unknown subcommand: %s", cmd)
	}
	return true
}

func (a *App) DiagLogPath() string { return a.diag.LogPath() }

func (a *App) LogFrontend(level, tag, msg string, fields map[string]any) {
	if level == "" {
		level = LvlInfo
	}
	if tag == "" {
		tag = "FE"
	}
	a.diag.With(level, tag, msg, fields)
}

func (a *App) DiagSnapshot() []DiagEvent {
	defer a.diag.Around("DiagSnapshot", nil)()
	return a.diag.Snapshot()
}

func (a *App) SetDiagDebug(on bool) bool { return a.diag.SetDebug(on) }

func (a *App) DiagDebugEnabled() bool { return a.diag.DebugEnabled() }

func (a *App) GetConnectionInfo() ConnectionInfo {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.getConnectionInfo()
}

// ─── Internal ───

// openLocked opens `port`, runs IDENTIFY (read-only — does not activate
// hardware), and caches the result.  Caller holds a.mu.  Returns the open
// error; an IDENTIFY failure is logged but leaves the connection open.
func (a *App) openLocked(port string) error {
	a.closeLocked()
	// 5 s timeout: topology queries (PORT_LIST / ROLE_LIST) aggregate the
	// hub + every expander, with the hub forwarding to a possibly-slow
	// expander — the 2 s wire default is too tight once an expander is on
	// the bus.
	c, id, err := client.Connect(port, client.Options{Timeout: 5 * time.Second})
	if err != nil && c == nil {
		return err
	}
	a.c = c
	a.connectedPort = port
	if a.ports != nil {
		a.ports.SetWatched(port)
	}
	if err != nil {
		a.diag.Warn("CONN", "IDENTIFY failed on %s: %v", port, err)
	} else {
		a.id = id
		a.kind = string(id.Kind())
		a.initialized = true
	}
	a.installAsyncDiag()
	a.installInputStream()
	a.installEngineStream()
	a.installGunFxStream()
	a.installLandingStream()
	a.installWireLogger()
	return nil
}

// installWireLogger pipes the protocol layer's per-packet TX/RX trace
// into the studio's diag system at DEBUG level (so it appears in the
// console when /diag debug on is enabled, alongside the existing
// command-level logs).  Toggle with /diag wire on.
func (a *App) installWireLogger() {
	if a.c == nil {
		return
	}
	a.c.Conn().SetWireLogger(func(dir, name string, tag byte, payloadLen int) {
		a.diag.With(LvlDebug, "WIRE",
			fmt.Sprintf("%s %s tag=%d [%d bytes]", dir, name, tag, payloadLen),
			nil)
	})
}

// closeLocked tears down the live client and resets cached state.  Caller
// holds a.mu.  Idempotent.
func (a *App) closeLocked() {
	if a.c != nil {
		a.c.Close()
		a.c = nil
	}
	if a.ports != nil {
		a.ports.SetWatched("")
	}
	a.id = client.Identity{}
	a.initialized = false
	a.kind = ""
	a.connectedPort = ""
}

func (a *App) getConnectionInfo() ConnectionInfo {
	info := ConnectionInfo{}
	if a.c == nil {
		return info
	}
	info.Connected = true
	info.Port = a.c.PortName()
	info.Initialized = a.initialized
	info.ControllerType = a.kind
	info.ControllerName = controllerLabel(a.kind)
	info.FirmwareVer = a.id.FirmwareVersion
	info.Build = a.id.BuildNumber
	info.Platform = a.id.Platform
	info.CPUMHz = a.id.CPUFreqMHz
	info.FreeRAM = a.id.FreeRAMBytes
	info.Capabilities = a.id.Capabilities
	return info
}
