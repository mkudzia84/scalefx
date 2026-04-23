package main

// Studio backend (Wails v2). The App struct is the single Wails-bound type:
// each method here becomes a JS-callable binding. Domain-specific bindings
// live in app_config.go (YAML round-trip), app_files.go (File Manager) and
// app_firmware.go (build/flash, releases, esptool). This file owns the
// connection lifecycle, port watcher, command dispatch, and console echoes.

import (
	"context"
	"fmt"
	"scalefx/engine"
	"scalefx/engine/handlers"
	"scalefx/engine/handlers/gearcontrol"
	"scalefx/engine/handlers/gunfx"
	"scalefx/engine/handlers/hubfx"
	"scalefx/engine/handlers/lightfx"
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

	// Latest slave state from HubFX STATUS_BROADCAST (nil when not connected
	// to a hub or before the first broadcast arrives). GetSlaveInfo() reads
	// this; the OnStatusBroadcast listener keeps it fresh.
	slaveStatus *hubfx.StatusBroadcast
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
		// Cache the slave-ready bits and push a derived SlaveInfo list so the
		// tab bar can reflect online/offline state without a polling query.
		a.mu.Lock()
		a.slaveStatus = s
		slaves := a.buildSlaveInfoLocked()
		a.mu.Unlock()
		wailsRT.EventsEmit(a.ctx, "slaves:changed", slaves)
	})
	a.eng.OnDisconnect = func() {
		a.mu.Lock()
		a.slaveStatus = nil
		a.mu.Unlock()
		wailsRT.EventsEmit(ctx, "connection:changed", a.getConnectionInfo())
		wailsRT.EventsEmit(ctx, "slaves:changed", []SlaveInfo{})
	}

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

	// Send INIT with verbose flag so the device starts streaming STATUS_BROADCAST.
	// HubFX auto-inits at boot (cmdConnect already sets Initialized=true after the
	// IDENTIFY round-trip), but the firmware-side verbose flag is gated solely on
	// the INIT_FLAGS bitmask — without an INIT it never enables periodic broadcast.
	// Sending INIT is idempotent on the device (handleInit calls reset() then
	// re-applies flags), so we drop the !Initialized guard.
	if a.eng.Conn != nil && a.eng.ControllerType != "" {
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

// GetSlaveInfo returns the slave controllers known from the last HubFX
// STATUS_BROADCAST. Returns an empty slice for non-HubFX connections or when
// no broadcast has been received yet (slaves render as "offline" until the
// hub reports them ready).
func (a *App) GetSlaveInfo() []SlaveInfo {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.buildSlaveInfoLocked()
}

// buildSlaveInfoLocked derives the slave list from the cached StatusBroadcast.
// Caller must hold a.mu. The ready bit in SlaveMask is used as both the
// "connected" and "ready" signal — in practice the connected→ready transition
// takes ~milliseconds after auto-INIT, so the intermediate state is not worth
// a separate SLAVE_LIST poll. When the hub has never broadcast (cache nil) we
// still emit the three known slave slots so the tab bar renders them greyed
// out rather than hiding the whole slave row.
func (a *App) buildSlaveInfoLocked() []SlaveInfo {
	if a.eng.ControllerType != core.CtrlHubFX || a.eng.Conn == nil {
		return []SlaveInfo{}
	}
	s := a.slaveStatus
	gunReady := s != nil && s.GunFxReady
	lightReady := s != nil && s.LightFxReady
	gearReady := s != nil && s.GearCtrlReady
	return []SlaveInfo{
		{Type: "gunfx", Name: "GunFX", Connected: gunReady, Ready: gunReady},
		{Type: "lightfx", Name: "LightFX", Connected: lightReady, Ready: lightReady},
		{Type: "gearcontrol", Name: "GearControl", Connected: gearReady, Ready: gearReady},
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
