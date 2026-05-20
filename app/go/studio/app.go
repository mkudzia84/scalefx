package main

// Studio backend (Wails v2). The App struct is the single Wails-bound type:
// each method here becomes a JS-callable binding. Domain-specific bindings
// live in app_config.go (YAML round-trip), app_files.go (File Manager) and
// app_firmware.go (build/flash, releases, esptool). This file owns the
// connection lifecycle, port watcher, command dispatch, and console echoes.

import (
	"context"
	"encoding/json"
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
	"sync/atomic"
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
	// Capabilities is the bitmask the firmware advertised in
	// IDENTIFY / INIT_READY (mirrors core.Cap*).
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
	ctx  context.Context
	eng  *engine.Engine
	reg  *handlers.Registry
	out  *GUIOutput
	diag *Diag
	mu   sync.Mutex

	// Port watcher
	stopPortWatcher chan struct{}

	// Heartbeat goroutine
	stopHeartbeat chan<- struct{}

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

	return &App{eng: eng, reg: reg, out: out, diag: NewDiag()}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	a.out.ctx = ctx
	a.diag.SetCtx(ctx)
	a.diag.Info("APP", "Studio starting up — go=%s os=%s build=studio",
		runtimeGoVersion(), runtimeOS())
	a.diag.Info("APP", "process pid=%d, working dir=%s", processPID(), workingDir())
	if p := a.diag.LogPath(); p != "" {
		a.diag.Info("APP", "diagnostic log: %s", p)
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
		// Cache the slave-ready bits and push a derived SlaveInfo list so the
		// tab bar can reflect online/offline state without a polling query.
		a.mu.Lock()
		a.slaveStatus = s
		slaves := a.buildSlaveInfoLocked()
		a.mu.Unlock()
		wailsRT.EventsEmit(a.ctx, "slaves:changed", slaves)
	})
	a.eng.OnDisconnect = func() {
		a.diag.Warn("CONN", "Engine fired OnDisconnect — port lost or remote shutdown")
		a.mu.Lock()
		a.slaveStatus = nil
		a.mu.Unlock()
		wailsRT.EventsEmit(ctx, "connection:changed", a.getConnectionInfo())
		wailsRT.EventsEmit(ctx, "slaves:changed", []SlaveInfo{})
	}

	a.startPortWatcher()

	// Heartbeat: 1 line every 10 s while running. Captures connection state,
	// goroutine count, heap. If this stops appearing in the terminal, the
	// app is wedged.
	a.stopHeartbeat = a.diag.StartHeartbeat(10*time.Second, func() map[string]any {
		a.mu.Lock()
		ctype := a.eng.ControllerType
		hasConn := a.eng.Conn != nil
		hasSlave := a.slaveStatus != nil
		a.mu.Unlock()
		return map[string]any{
			"connected":   hasConn,
			"controller":  ctype,
			"initialized": a.eng.Initialized,
			"hub_status":  hasSlave,
		}
	})
}

func (a *App) shutdown(_ context.Context) {
	a.diag.Info("APP", "shutdown requested")
	if a.stopHeartbeat != nil {
		close(a.stopHeartbeat)
	}
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
				added, removed := diffPortLists(lastPorts, key)
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
				a.diag.With(LvlInfo, "PORTS", "port list changed",
					map[string]any{"added": added, "removed": removed, "total": len(items)})
				wailsRT.EventsEmit(a.ctx, "ports:changed", items)
			}
		}
	}()
}

// installAsyncDiag attaches the protocol Connection's async callback so
// every unsolicited packet (STATUS_BROADCAST, LOG_MESSAGE, IDENTIFY-on-
// reboot, …) leaves a one-line breadcrumb in the diag log. The engine's
// handlers still process the packet — we just observe.
//
// At debug-level only — STATUS at 1 Hz × N panels would otherwise drown
// the GUI console. Filter out the periodic STATUS_BROADCAST stream
// after the first tick so the line shows up once and then quiets down.
func (a *App) installAsyncDiag() {
	if a.eng.Conn == nil {
		return
	}
	prevCB := a.eng.Conn.GetCallback()
	statusSeen := atomic.Bool{}
	a.eng.Conn.SetCallback(func(r *protocol.Response) {
		if r != nil {
			ptype := byte(r.PacketType)
			tag := r.Tag
			plen := len(r.Payload)
			pname := protocol.PacketTypeName(r.PacketType)
			// Periodic STATUS broadcast: log first arrival, then suppress.
			isStatus := ptype == 0xF4 || ptype == 0xEF
			if isStatus {
				if statusSeen.CompareAndSwap(false, true) {
					a.diag.With(LvlInfo, "RX", "first STATUS broadcast received",
						map[string]any{"type": pname, "len": plen})
				}
			} else {
				a.diag.With(LvlDebug, "RX", "async packet",
					map[string]any{"type": pname, "tag": tag, "len": plen})
			}
		}
		if prevCB != nil {
			prevCB(r)
		}
	})
	a.diag.Debug("CONN", "async packet observer installed")
}

// diffPortLists returns the elements added / removed between two
// comma-joined port lists. Used purely for log breadcrumbs.
func diffPortLists(prev, next string) (added, removed []string) {
	p := stringSetCSV(prev)
	n := stringSetCSV(next)
	for v := range n {
		if !p[v] {
			added = append(added, v)
		}
	}
	for v := range p {
		if !n[v] {
			removed = append(removed, v)
		}
	}
	sort.Strings(added)
	sort.Strings(removed)
	return added, removed
}

func stringSetCSV(s string) map[string]bool {
	out := map[string]bool{}
	if s == "" {
		return out
	}
	for _, v := range strings.Split(s, ",") {
		out[v] = true
	}
	return out
}

func (a *App) stopPortWatcherLoop() {
	if a.stopPortWatcher != nil {
		close(a.stopPortWatcher)
	}
}

// ─── Exposed Methods ───

func (a *App) ListPorts() []PortInfo {
	defer a.diag.Around("ListPorts", nil)()
	ports := protocol.ListPortsDetailed()
	result := make([]PortInfo, len(ports))
	for i, p := range ports {
		result[i] = PortInfo{Name: p.Name, Description: p.Description}
	}
	a.diag.With(LvlDebug, "PORTS", "ListPorts result",
		map[string]any{"count": len(result)})
	return result
}

func (a *App) Connect(port string) ConnectionInfo {
	defer a.diag.Around("Connect", map[string]any{"port": port})()
	a.mu.Lock()
	defer a.mu.Unlock()

	a.diag.Info("CONN", "Connecting to %s", port)
	a.eng.Dispatch("connect " + port)

	// Send INIT with verbose flag so the device starts streaming STATUS_BROADCAST.
	// HubFX auto-inits at boot (cmdConnect already sets Initialized=true after the
	// IDENTIFY round-trip), but the firmware-side verbose flag is gated solely on
	// the INIT_FLAGS bitmask — without an INIT it never enables periodic broadcast.
	// Sending INIT is idempotent on the device (handleInit calls reset() then
	// re-applies flags), so we drop the !Initialized guard.
	if a.eng.Conn != nil && a.eng.ControllerType != "" {
		a.eng.Dispatch("init direct verbose")
		a.installAsyncDiag() // start logging unsolicited packets
	} else if a.eng.Conn == nil {
		a.diag.Warn("CONN", "Connect to %s did not establish a session", port)
	}

	info := a.getConnectionInfo()
	a.diag.With(LvlInfo, "CONN", "connection state",
		map[string]any{
			"connected":   info.Connected,
			"initialized": info.Initialized,
			"controller":  info.ControllerType,
			"name":        info.ControllerName,
			"version":     info.FirmwareVer,
			"build":       info.Build,
			"caps":        info.Capabilities,
		})
	wailsRT.EventsEmit(a.ctx, "connection:changed", info)
	return info
}

func (a *App) Disconnect() ConnectionInfo {
	defer a.diag.Around("Disconnect", nil)()
	a.mu.Lock()
	defer a.mu.Unlock()

	a.diag.Info("CONN", "Disconnect requested")
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
		a.diag.Info("CMD", "user typed `%s` — quitting", trimmed)
		wailsRT.Quit(a.ctx)
		return
	}
	// Built-in /diag commands — never reach the engine. Lets the user
	// flip debug logging on/off and dump the recent ring buffer
	// without an external CLI.
	if a.handleDiagSlash(trimmed) {
		return
	}
	defer a.diag.Around("SendCommand", map[string]any{"cmd": trimmed})()

	a.mu.Lock()
	defer a.mu.Unlock()

	// Echo command
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{
		Type:    "command",
		Content: escapeHTML(trimmed),
	})

	before := a.getConnectionInfo()
	a.eng.Dispatch(trimmed)
	after := a.getConnectionInfo()

	// Only re-emit `connection:changed` when state ACTUALLY changed —
	// otherwise every slider drag / LED toggle in a panel wakes up
	// every component subscribed to connection state and floods the
	// diag log with no-op `connection:changed` events.
	if before != after {
		wailsRT.EventsEmit(a.ctx, "connection:changed", after)
	}
}

// handleDiagSlash recognises `/diag …` slash commands and returns true
// if the input was consumed (the caller skips engine dispatch). The
// commands surface through the regular console:output channel so they
// look like any other command.
//
//   /diag debug on | off | toggle   — flip verbose logging
//   /diag dump                       — emit the recent event ring as JSON
//   /diag clear                      — reset the ring (after a repro)
//   /diag                            — short summary
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
			case "toggle":
				// already flipped above
			}
		}
		a.diag.SetDebug(on)
	case len(parts) >= 2 && parts[1] == "dump":
		evs := a.diag.Snapshot()
		blob, _ := json.MarshalIndent(evs, "", "  ")
		// Send the JSON as a plain output line so it can be selected
		// and copied. Stdout already has it (see diag.log).
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

// DiagLogPath returns the on-disk path the diagnostic log is being
// written to (empty if the file could not be opened). Bound for the
// frontend so the user can show "Open log…" in a menu, and also useful
// when the agent needs to know where to `cat` from outside the app.
func (a *App) DiagLogPath() string {
	return a.diag.LogPath()
}

// LogFrontend is the JS-side bridge into the diag system. The
// frontend's window.onerror / unhandledrejection / wrapped console.error
// hooks call this so JS exceptions and warnings show up in the same
// stream as Go-side events. `level` is debug | info | warn | error;
// `tag` is FE.<area> by convention (FE.RENDER, FE.WAILS, FE.UNCAUGHT).
func (a *App) LogFrontend(level, tag, msg string, fields map[string]any) {
	if level == "" {
		level = LvlInfo
	}
	if tag == "" {
		tag = "FE"
	}
	a.diag.With(level, tag, msg, fields)
}

// DiagSnapshot returns the recent event ring buffer for the frontend's
// "Copy diagnostics" button or for an automated script.
func (a *App) DiagSnapshot() []DiagEvent {
	defer a.diag.Around("DiagSnapshot", nil)()
	return a.diag.Snapshot()
}

// SetDiagDebug toggles verbose diagnostic logging at runtime. Returns
// the new state so the frontend can render the toggle without a separate
// query.
func (a *App) SetDiagDebug(on bool) bool {
	return a.diag.SetDebug(on)
}

// DiagDebugEnabled returns the current debug-logging state.
func (a *App) DiagDebugEnabled() bool {
	return a.diag.DebugEnabled()
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
	port := a.eng.Port
	if a.eng.Conn != nil {
		// Live connection wins — `eng.Port` is just whatever was passed
		// on the cmdline at startup and stays empty for GUI-initiated
		// connects (Studio passes the port via Dispatch("connect ...")
		// rather than wiring eng.Port).
		port = a.eng.Conn.PortName()
	}
	info := ConnectionInfo{
		Connected:   a.eng.Conn != nil,
		Initialized: a.eng.Initialized,
		Port:        port,
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
