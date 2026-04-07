package main

import (
	"context"
	"scalefx/engine"
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

// ─── App struct ───

type App struct {
	ctx context.Context
	eng *engine.Engine
	out *GUIOutput
	mu  sync.Mutex

	// Port watcher
	stopPortWatcher chan struct{}
}

func NewApp() *App {
	out := &GUIOutput{}
	eng := engine.NewEngine(out, "", false)
	eng.PromptSelectPort = func(ports []string) string { return "" }

	return &App{eng: eng, out: out}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	a.out.ctx = ctx
	a.eng.OnDisconnect = func() {
		wailsRT.EventsEmit(ctx, "connection:changed", a.getConnectionInfo())
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

	// Auto-init slave controllers (HubFX auto-inits on boot, handled in engine)
	if a.eng.Conn != nil && !a.eng.Initialized && a.eng.ControllerType != "" {
		a.eng.Dispatch("init")
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
