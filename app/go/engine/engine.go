package engine

// ScaleFX Engine - Core Engine
// Owns connection, API client, command dispatch, and listener lifecycle.
// Both the CLI and GUI console embed this struct to get shared behavior.

import (
	"fmt"
	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"strings"
	"sync/atomic"
	"time"
)

// Engine is the shared command engine used by both CLI and GUI.
type Engine struct {
	Out Output

	Conn           *protocol.Connection
	API            *api.Client
	Port           string
	Verbose        bool
	ControllerType string
	Initialized    bool
	Info           *InitReadyInfo

	// Command groups (populated on first call to GetGroups)
	groups   []*CmdGroup
	flatCmds map[string][]flatEntry

	// Listener control
	listenerRunning atomic.Bool

	// Keepalive
	KeepaliveInterval time.Duration

	// Callbacks for external consumers (GUI)
	OnDisconnect     func()
	PromptSelectPort func(ports []string) string // returns selected port or "" to cancel

	// Registries for modular handler dispatch
	statusParsers          map[string]func([]byte)                  // key: controller type (e.g., core.CtrlGunFX)
	statusBroadcastParsers map[string]func([]byte)                  // key: controller type — silent parsers for broadcast
	asyncHandlers          map[protocol.PacketType]func([]byte)     // key: packet type
}

// NewEngine creates a new engine with the given output backend.
func NewEngine(out Output, port string, verbose bool) *Engine {
	return &Engine{
		Out:               out,
		Port:              port,
		Verbose:           verbose,
		KeepaliveInterval: 5 * time.Second,
	}
}

// ─── Command Registration ───

// GetGroups returns all command groups. Groups are populated
// by calling RegisterDefaults / AddGroup before first Dispatch.
func (e *Engine) GetGroups() []*CmdGroup {
	if e.groups == nil {
		e.groups = []*CmdGroup{}
	}
	return e.groups
}

// AddGroup registers an additional command group (e.g. CLI-only commands).
// Must be called before the first Dispatch() or GetGroups() call.
func (e *Engine) AddGroup(g *CmdGroup) {
	e.groups = append(e.GetGroups(), g)
	e.flatCmds = nil // invalidate cache
}

// RegisterStatusParser registers a module-specific STATUS data parser.
// Called from handler Register() functions to extend ParseStatusPayload dispatch.
func (e *Engine) RegisterStatusParser(controllerType string, parser func([]byte)) {
	if e.statusParsers == nil {
		e.statusParsers = make(map[string]func([]byte))
	}
	e.statusParsers[controllerType] = parser
}

// RegisterStatusBroadcastParser registers a parser for periodic STATUS_BROADCAST
// packets from a given controller type. Unlike statusParsers (which print to console),
// broadcast parsers are called silently and should fire OnStatusBroadcast.
func (e *Engine) RegisterStatusBroadcastParser(controllerType string, parser func([]byte)) {
	if e.statusBroadcastParsers == nil {
		e.statusBroadcastParsers = make(map[string]func([]byte))
	}
	e.statusBroadcastParsers[controllerType] = parser
}

// HandleStatusBroadcast routes a STATUS_BROADCAST payload to the appropriate
// controller's registered broadcast parser. Each board handler registers a
// silent parser in its Register() that decodes the payload and fires the
// handler's typed On*Broadcast listener.
func (e *Engine) HandleStatusBroadcast(source byte, data []byte) {
	ctrlType := sourceToControllerType(source)
	if ctrlType == "" {
		return
	}
	if parser, ok := e.statusBroadcastParsers[ctrlType]; ok {
		parser(data)
	}
}

// SetControllerType records the detected peer controller type and propagates
// derived settings (e.g. file-upload chunk size tuned to peer COBS capacity).
// Call whenever IDENTIFY/INIT reveals the controller type, so the API layer
// can pick correct wire-format parameters before the first dependent call.
func (e *Engine) SetControllerType(ct string) {
	e.ControllerType = ct
	if e.API == nil || e.API.Files == nil {
		return
	}
	switch ct {
	case core.CtrlHubFX:
		e.API.Files.SetPeerMaxPayload(api.Esp32MaxPayload)
	default:
		e.API.Files.SetPeerMaxPayload(api.PicoMaxPayload)
	}
}

// sourceToControllerType maps StatusUpdateSource bytes to controller type strings.
func sourceToControllerType(source byte) string {
	switch source {
	case core.StatusUpdateSourceGunFX:
		return core.CtrlGunFX
	case core.StatusUpdateSourceLightFX:
		return core.CtrlLightFX
	case core.StatusUpdateSourceGearControl:
		return core.CtrlGearControl
	case core.StatusUpdateSourceHubFX:
		return core.CtrlHubFX
	default:
		return ""
	}
}

// RegisterAsyncHandler registers a handler for unsolicited packet types.
// Called from handler Register() functions to extend HandleAsyncPacket dispatch.
func (e *Engine) RegisterAsyncHandler(packetType protocol.PacketType, handler func([]byte)) {
	if e.asyncHandlers == nil {
		e.asyncHandlers = make(map[protocol.PacketType]func([]byte))
	}
	e.asyncHandlers[packetType] = handler
}

// FlatCommands returns all commands across all groups, keyed by command name.
// Multiple entries per name occur when controller groups share a name
// (e.g. both LightFX and GearControl define "reset"). Use resolveCommand to
// pick the right entry based on the currently connected controller.
func (e *Engine) FlatCommands() map[string][]flatEntry {
	if e.flatCmds != nil {
		return e.flatCmds
	}
	cmds := make(map[string][]flatEntry)
	for _, g := range e.GetGroups() {
		for name, entry := range g.Commands {
			cmds[name] = append(cmds[name], flatEntry{entry, g.Controller})
		}
	}
	// Built-in aliases
	cmds["help"] = []flatEntry{{CmdEntry{e.CmdHelp, "help [command]", "Show help", false}, ""}}
	cmds["?"] = []flatEntry{{CmdEntry{e.CmdHelp, "?", "Show help", false}, ""}}
	e.flatCmds = cmds
	return cmds
}

// resolveCommand selects the best entry for a command name based on the
// currently connected controller:
//  1. Entry whose controller matches e.ControllerType.
//  2. Universal entry (controller == "").
//  3. First entry (deterministic — caller still validates controller match).
func (e *Engine) resolveCommand(entries []flatEntry) flatEntry {
	if e.ControllerType != "" {
		for _, en := range entries {
			if en.controller == e.ControllerType {
				return en
			}
		}
	}
	for _, en := range entries {
		if en.controller == "" {
			return en
		}
	}
	return entries[0]
}

// ─── Command Dispatch ───

// Dispatch parses a command line and dispatches to the appropriate handler.
func (e *Engine) Dispatch(line string) {
	parts := strings.Fields(line)
	if len(parts) == 0 {
		return
	}
	cmd := strings.ToLower(parts[0])
	args := parts[1:]

	cmds := e.FlatCommands()
	entries, ok := cmds[cmd]
	if !ok {
		// Try prefix match
		var matches []string
		for k := range cmds {
			if strings.HasPrefix(k, cmd) {
				matches = append(matches, k)
			}
		}
		if len(matches) == 1 {
			entries = cmds[matches[0]]
			ok = true
		} else if len(matches) > 1 {
			e.Out.Error("Ambiguous command '%s'. Matches: %s", cmd, strings.Join(matches, ", "))
			return
		} else {
			e.Out.Error("Unknown command: %s (type 'help' for commands)", cmd)
			return
		}
	}
	entry := e.resolveCommand(entries)

	// Check connection
	if entry.RequireInit || (entry.controller != "" && e.Conn == nil) {
		if e.Conn == nil {
			e.Out.Error("Not connected. Use 'connect <port>' first.")
			return
		}
	}

	// Check controller type mismatch — bail without dispatching, otherwise we
	// would send a packet meant for one controller to a different one.
	if entry.controller != "" && e.ControllerType != "" && entry.controller != e.ControllerType {
		e.Out.Error("Command '%s' is for %s but connected to %s",
			cmd, ControllerLabels[entry.controller], ControllerLabels[e.ControllerType])
		return
	}

	// Check init
	if entry.RequireInit && !e.Initialized {
		e.Out.Warning("Controller not initialized. Use 'init' first.")
	}

	e.Out.Debug("dispatch: cmd=%q controller=%q args=%v", cmd, entry.controller, args)
	entry.Handler(args)
}

// ─── Listener & Keepalive ───

// StartListenerLoop registers the async packet callback and starts
// the connection-level keepalive goroutine. The keepalive mirrors the
// C++ SerialBus::processKeepalive() pattern: only sends a KEEPALIVE
// packet when the connection has been idle for KeepaliveInterval,
// so regular command traffic suppresses redundant heartbeats.
func (e *Engine) StartListenerLoop() {
	if e.Conn == nil || e.listenerRunning.Load() {
		return
	}

	e.Conn.SetCallback(e.HandleAsyncPacket)
	e.Conn.OnPortError = e.HandlePortLoss
	e.listenerRunning.Store(true)

	// Start idle-aware keepalive on the connection itself
	e.Conn.StartKeepalive(e.KeepaliveInterval)
}

// StopListenerLoop stops the keepalive and clears the async callback.
func (e *Engine) StopListenerLoop() {
	if e.listenerRunning.Load() {
		if e.Conn != nil {
			e.Conn.StopKeepalive()
		}
		e.listenerRunning.Store(false)
	}
}

// HandleAsyncPacket handles unsolicited packets from the device.
func (e *Engine) HandleAsyncPacket(resp *protocol.Response) {
	if DebugBuild {
		e.Out.Debug("async: type=%s(0x%02X) tag=%d payload_len=%d",
			protocol.PacketTypeName(resp.PacketType), byte(resp.PacketType), resp.Tag, len(resp.Payload))
	}
	switch resp.PacketType {
	case core.LogMessage:
		e.ParseLogMessage(resp.Payload)
	case core.Ack:
		// Silently ignore async ACKs (e.g., keepalive responses)
	case core.Nack:
		errCode := byte(0)
		if len(resp.Payload) > 0 {
			errCode = resp.Payload[0]
		}
		e.Out.Warning("NACK (async): %s (0x%02X)", protocol.ErrorName(protocol.ErrorCode(errCode)), errCode)
	default:
		// Delegate to registered module-specific async handlers
		if handler, ok := e.asyncHandlers[resp.PacketType]; ok {
			handler(resp.Payload)
		} else {
			name := protocol.PacketTypeName(resp.PacketType)
			if e.Verbose {
				e.Out.Printf("  %s[%s tag=%d %d bytes]%s\n",
					e.Out.C(ColorGray, ""), name, resp.Tag, len(resp.Payload), e.Out.C(ColorReset, ""))
			}
		}
	}
}

// Cleanup tears down the connection.
func (e *Engine) Cleanup() {
	if e.Conn != nil {
		e.StopListenerLoop()
		e.Conn.Close()
	}
}

// HandlePortLoss is called (on a goroutine) when the serial port reports a
// persistent I/O error, typically from physical USB disconnect. It tears down
// the connection state and fires OnDisconnect so the GUI/CLI can react.
func (e *Engine) HandlePortLoss() {
	e.Out.Error("Device disconnected (port lost)")

	e.listenerRunning.Store(false)
	if e.Conn != nil {
		e.Conn.Close()
	}
	e.Conn = nil
	e.API = nil
	e.ControllerType = ""
	e.Initialized = false
	e.Info = nil

	if e.OnDisconnect != nil {
		e.OnDisconnect()
	}
}

// ─── Help System ───

// CmdHelp shows help for all commands or a single command.
func (e *Engine) CmdHelp(args []string) {
	cmds := e.FlatCommands()

	// Single command help
	if len(args) > 0 {
		cmd := strings.ToLower(args[0])
		if entries, ok := cmds[cmd]; ok && len(entries) > 0 {
			if len(entries) == 1 {
				e.Out.Printf("  %s — %s\n", entries[0].Usage, entries[0].Description)
				return
			}
			e.Out.Printf("  '%s' has %d variants:\n", cmd, len(entries))
			for _, en := range entries {
				label := "Universal"
				if en.controller != "" {
					if l, ok := ControllerLabels[en.controller]; ok {
						label = l
					}
				}
				e.Out.Printf("    %s — %s %s\n",
					en.Usage, en.Description,
					e.Out.C(ColorGray, "("+label+")"))
			}
			return
		}
		e.Out.Error("Unknown command: %s", cmd)
		return
	}

	// Show connection status at top
	if e.Info != nil {
		e.PrintConnectionStatus()
	}

	// Group help — filter by connected controller
	for _, g := range e.GetGroups() {
		if g.Controller != "" && e.ControllerType != "" && e.ControllerType != g.Controller {
			continue
		}
		e.PrintGroupHelp(g)
	}

	// "help" and "quit" at the bottom
	e.Out.Printf("\n%s── Other %s\n", e.Out.C(ColorGray, ""), strings.Repeat("─", 42)+e.Out.C(ColorReset, ""))
	e.Out.Printf("  %s  %s\n", e.Out.C(ColorGray, fmt.Sprintf("%-40s", "help [command]")), e.Out.C(ColorGray, "Show help"))
	e.Out.Printf("  %s  %s\n", e.Out.C(ColorGray, fmt.Sprintf("%-40s", "quit")), e.Out.C(ColorGray, "Exit CLI"))

	// Hint about hidden commands when connected
	if e.ControllerType != "" {
		hiddenCount := 0
		for _, g := range e.GetGroups() {
			if g.Controller != "" && g.Controller != e.ControllerType {
				hiddenCount += len(g.Commands)
			}
		}
		if hiddenCount > 0 {
			e.Out.Printf("\n  %s\n", e.Out.C(ColorGray, fmt.Sprintf("(%d commands for other controllers hidden)", hiddenCount)))
		}
	}

	e.Out.Println()
}

// ─── Output Helpers ───

// PrintACKResult prints ACK or NACK result for a command.
func (e *Engine) PrintACKResult(resp *protocol.Response, okMsg string) {
	if resp == nil {
		e.Out.Error("No response (timeout)")
	} else if resp.IsACK() {
		e.Out.OK(okMsg)
	} else if resp.IsNACK() {
		code := resp.ErrorCode()
		name := protocol.ErrorName(code)
		msg := resp.ErrorMessage()
		e.Out.Error("NACK: %s (0x%02X) %s", name, code, msg)
	} else {
		e.Out.Warning("Unexpected: 0x%02X", resp.PacketType)
	}
}

// GetPrompt generates a prompt string based on connection state.
func (e *Engine) GetPrompt() string {
	if e.ControllerType != "" {
		color := ControllerColors[e.ControllerType]
		if color == 0 {
			color = ColorCyan
		}
		label := ControllerLabels[e.ControllerType]
		if label == "" {
			label = e.ControllerType
		}
		return e.Out.C(color, label+">") + " "
	}
	if e.Conn != nil {
		return e.Out.C(ColorYellow, "connected>") + " "
	}
	return e.Out.C(ColorGray, "scalefx>") + " "
}

// PrintGroupHelp renders a single command group with colored header and sorted columns.
func (e *Engine) PrintGroupHelp(group *CmdGroup) {
	if len(group.Commands) == 0 {
		return
	}

	// Collect and sort command names
	commands := make([]string, 0, len(group.Commands))
	for name := range group.Commands {
		commands = append(commands, name)
	}
	sortStrings(commands)

	// Colored group header
	header := fmt.Sprintf("── %s ", group.Name)
	padLen := 50 - len(header)
	if padLen < 2 {
		padLen = 2
	}
	header += strings.Repeat("─", padLen)
	e.Out.Printf("\n%s\n", e.Out.C(group.Color, header))

	// Max usage width
	maxWidth := 0
	for _, name := range commands {
		entry := group.Commands[name]
		if len(entry.Usage) > maxWidth {
			maxWidth = len(entry.Usage)
		}
	}
	if maxWidth > 40 {
		maxWidth = 40
	}

	// Print each command
	for _, name := range commands {
		entry := group.Commands[name]
		e.Out.Printf("  %s  %s\n",
			e.Out.C(group.Color, fmt.Sprintf("%-*s", maxWidth, entry.Usage)),
			e.Out.C(ColorGray, entry.Description))
	}
}

// PrintConnectionStatus prints a summary of the current connection.
func (e *Engine) PrintConnectionStatus() {
	if e.Info != nil {
		label := ControllerLabels[e.ControllerType]
		if label == "" {
			label = e.ControllerType
		}
		color := ControllerColors[e.ControllerType]
		if color == 0 {
			color = ColorCyan
		}
		status := e.Out.C(ColorYellow, "connected")
		if e.Initialized {
			status = e.Out.C(ColorGreen, "initialized")
		}
		e.Out.Printf("  %s v%s (build %d) — %s\n",
			e.Out.C(color, label), e.Info.Version, e.Info.Build, status)
	}
}

// PrintBanner prints the CLI startup banner.
func (e *Engine) PrintBanner() {
	e.Out.Printf("%s %s — type %s for commands, %s to exit\n",
		e.Out.C(ColorCyan, "ScaleFX CLI"),
		e.Out.C(ColorGray, "(Go)"),
		e.Out.C(ColorWhite, "help"),
		e.Out.C(ColorWhite, "quit"))
}

// ─── Sort helper (avoids importing sort for one call) ───

func sortStrings(s []string) {
	for i := 1; i < len(s); i++ {
		for j := i; j > 0 && s[j] < s[j-1]; j-- {
			s[j], s[j-1] = s[j-1], s[j]
		}
	}
}
