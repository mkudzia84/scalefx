package engine

// ScaleFX Engine - Core Engine
// Owns connection, API client, command dispatch, and listener lifecycle.
// Both the CLI and GUI console embed this struct to get shared behavior.

import (
	"fmt"
	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"scalefx/protocol/gearcontrol"
	"scalefx/protocol/lightfx"
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
	flatCmds map[string]flatEntry

	// Listener control
	listenerRunning atomic.Bool

	// Keepalive
	KeepaliveInterval time.Duration

	// Callbacks for external consumers (GUI)
	OnDisconnect   func()
	PromptSelectPort func(ports []string) string // returns selected port or "" to cancel
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

// GetGroups returns all command groups, building them once.
func (e *Engine) GetGroups() []*CmdGroup {
	if e.groups == nil {
		e.groups = []*CmdGroup{
			e.coreCommands(),
			e.firmwareCommands(),
			e.gunfxCommands(),
			e.gearcontrolCommands(),
			e.lightfxCommands(),
			e.hubfxCommands(),
		}
	}
	return e.groups
}

// FlatCommands returns all commands across all groups, keyed by command name.
func (e *Engine) FlatCommands() map[string]flatEntry {
	if e.flatCmds != nil {
		return e.flatCmds
	}
	cmds := make(map[string]flatEntry)
	for _, g := range e.GetGroups() {
		for name, entry := range g.Commands {
			cmds[name] = flatEntry{entry, g.Controller}
		}
	}
	// Built-in aliases
	cmds["help"] = flatEntry{CmdEntry{e.CmdHelp, "help [command]", "Show help", false}, ""}
	cmds["?"] = flatEntry{CmdEntry{e.CmdHelp, "?", "Show help", false}, ""}
	e.flatCmds = cmds
	return cmds
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
	entry, ok := cmds[cmd]
	if !ok {
		// Try prefix match
		var matches []string
		for k := range cmds {
			if strings.HasPrefix(k, cmd) {
				matches = append(matches, k)
			}
		}
		if len(matches) == 1 {
			entry = cmds[matches[0]]
			ok = true
		} else if len(matches) > 1 {
			e.Out.Error("Ambiguous command '%s'. Matches: %s", cmd, strings.Join(matches, ", "))
			return
		} else {
			e.Out.Error("Unknown command: %s (type 'help' for commands)", cmd)
			return
		}
	}

	// Check connection
	if entry.RequireInit || (entry.controller != "" && e.Conn == nil) {
		if e.Conn == nil {
			e.Out.Error("Not connected. Use 'connect <port>' first.")
			return
		}
	}

	// Check init
	if entry.RequireInit && !e.Initialized {
		e.Out.Warning("Controller not initialized. Use 'init' first.")
	}

	// Check controller type mismatch
	if entry.controller != "" && e.ControllerType != "" && entry.controller != e.ControllerType {
		e.Out.Warning("Command '%s' is for %s but connected to %s",
			cmd, ControllerLabels[entry.controller], ControllerLabels[e.ControllerType])
	}

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
	switch resp.PacketType {
	case core.LogMessage:
		e.ParseLogMessage(resp.Payload)
	case gearcontrol.GearCalibStatus:
		e.ParseGearCalibStatus(resp.Payload)
	case gearcontrol.GearSeqStatus:
		e.ParseGearSeqStatus(resp.Payload)
	case gearcontrol.GearDoorStatus:
		e.ParseGearDoorStatus(resp.Payload)
	case lightfx.LandingLightStatus:
		e.ParseLandingLightStatus(resp.Payload)
	case core.Ack:
		// Silently ignore async ACKs (e.g., keepalive responses)
	case core.Nack:
		errCode := byte(0)
		if len(resp.Payload) > 0 {
			errCode = resp.Payload[0]
		}
		e.Out.Warning("NACK (async): %s (0x%02X)", protocol.ErrorName(protocol.ErrorCode(errCode)), errCode)
	default:
		name := protocol.PacketTypeName(resp.PacketType)
		if e.Verbose {
			e.Out.Printf("  %s[%s tag=%d %d bytes]%s\n",
				e.Out.C(ColorGray, ""), name, resp.Tag, len(resp.Payload), e.Out.C(ColorReset, ""))
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
		if entry, ok := cmds[cmd]; ok {
			e.Out.Printf("  %s — %s\n", entry.Usage, entry.Description)
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
