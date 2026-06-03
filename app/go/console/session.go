package console

// Session core for the shared ScaleFX command surface.  The `App` type
// holds the live client + cached board identity and dispatches one input
// line to the matching command (registered in the cmd_*.go files).  Both
// scalefx-cli (interactive REPL) and ScaleFX Studio (GUI console) drive
// the SAME command set through this type — the CLI owns the connection
// via Connect(); Studio owns it externally and injects it via Attach().
//
// All command output flows through the package `out` writer (see term.go)
// so a host can redirect it: the CLI leaves it at os.Stdout; Studio points
// it at an ANSI→HTML sink that feeds the GUI console.

import (
	"fmt"
	"strings"

	"scalefx/client"
	"scalefx/protocol/core"
)

// App is the shared command session.
type App struct {
	c                *client.Client
	verbose          bool
	baud             int
	boardKind        string          // populated from IDENTIFY at connect time; "" before
	boardName        string          // e.g. "HubFx-6D60"
	boardCaps        uint32          // compiled-in CoreCapability bitmask
	boardEnabledCaps uint32          // runtime-enabled subset of boardCaps
	target           byte            // active storage backend (TargetSD / TargetFlash)
	cwd              map[byte]string // per-target current working directory
	inputVerbose     bool            // 'input-verbose on' gate — when false, 'subscribe' suppresses the per-frame [RC] flood
}

// NewApp creates a session.  `baud` 0 = default wire baud; `verbose`
// toggles wire-level logging on connections this session opens.
func NewApp(baud int, verbose bool) *App {
	return &App{
		verbose: verbose,
		baud:    baud,
		target:  0, // 0 == TargetSD; explicit so tests don't trip on order
		cwd:     map[byte]string{0: "/", 1: "/"},
	}
}

// Attach binds an externally-owned client + cached identity.  Used by
// Studio, which manages the connection lifecycle itself; the session then
// dispatches commands against that client without ever calling Connect().
// Pass a nil client to detach.
func (a *App) Attach(c *client.Client, caps, enabledCaps uint32, kind, name string) {
	a.c = c
	a.boardCaps = caps
	a.boardEnabledCaps = enabledCaps
	a.boardKind = kind
	a.boardName = name
}

// Client returns the live client (nil when disconnected).
func (a *App) Client() *client.Client { return a.c }

// Connected reports whether a client is attached.
func (a *App) Connected() bool { return a.c != nil }

// Shutdown closes the session's own client (no-op for Attach'd clients the
// host still owns — callers that Attach should not call Shutdown).
func (a *App) Shutdown() {
	if a.c != nil {
		a.c.Close()
		a.c = nil
	}
}

// Prompt renders the interactive REPL prompt.
func (a *App) Prompt() string {
	if a.c == nil {
		return cDim("scalefx") + cBold(cMagenta("» "))
	}
	label := a.c.PortName()
	if a.boardKind != "" {
		label = a.boardKind + cDim(":") + a.c.PortName()
	}
	return cCyan(label) + cBold(cMagenta(" » "))
}

// Banner prints the CLI startup banner to the output writer.
func (a *App) Banner() {
	fmt.Fprintln(out, cBold(cMagenta("ScaleFX CLI"))+cDim(" — generic-expander build"))
	fmt.Fprintln(out, cDim("type `help` for commands, `quit` to exit"))
}

// Connect opens `portName`, runs IDENTIFY, and caches board identity.
func (a *App) Connect(portName string) error {
	if a.c != nil {
		a.c.Close()
		a.c = nil
		a.boardKind = ""
		a.boardName = ""
	}
	opts := client.Options{Baud: a.baud, Verbose: a.verbose}
	c, id, err := client.Connect(portName, opts)
	if err != nil {
		if c != nil {
			c.Close()
		}
		return err
	}
	a.c = c
	Ok("connected: %s", cBold(portName))

	if k := id.Kind(); k != client.BoardUnknown {
		a.boardKind = string(k)
	}
	a.boardName = id.DeviceName
	a.boardCaps = id.Capabilities
	a.boardEnabledCaps = id.EnabledCapabilities
	printIdentityBanner(id)
	return nil
}

// printIdentityBanner shows the board type, firmware, and feature catalog.
func printIdentityBanner(id client.Identity) {
	kind := "unknown"
	if k := id.Kind(); k != client.BoardUnknown {
		kind = string(k)
	}
	KVf("board", "%s %s %s build %s (%s)",
		cBold(cCyan(kind)),
		cMagenta("—"),
		cBold(id.DeviceName)+cDim(" v")+id.FirmwareVersion,
		cDim(fmt.Sprintf("%d", id.BuildNumber)),
		cDim(id.Platform))

	caps := id.CapabilityNames()
	coloured := make([]string, len(caps))
	for i, n := range caps {
		coloured[i] = cGreen(n)
	}
	KVList("features", coloured)
}

func (a *App) requireClient() error {
	if a.c == nil {
		return fmt.Errorf("not connected — %s first", cCyan("`connect <port>`"))
	}
	return nil
}

// Dispatch parses one input line and runs the matching command.
func (a *App) Dispatch(line string) error {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return nil
	}
	name, args := fields[0], fields[1:]
	cmd, ok := commands[name]
	if !ok {
		if alt, ok2 := aliases[name]; ok2 {
			name = alt
			cmd = commands[name]
			ok = true
		}
	}
	if !ok {
		return fmt.Errorf("unknown command: %s (try `help`)", name)
	}
	if cmd.RequiresConn && a.c != nil && cmd.RequiresCap != 0 {
		if a.boardCaps&cmd.RequiresCap == 0 {
			return fmt.Errorf("%s requires capability %s — not compiled into this firmware",
				name, capLabel(cmd.RequiresCap))
		}
		if a.boardEnabledCaps&cmd.RequiresCap == 0 {
			return fmt.Errorf("%s is currently disabled in config — feature compiled in but turned off (capability %s)",
				name, capLabel(cmd.RequiresCap))
		}
	}
	return cmd.Run(a, args)
}

// capLabel formats a capability bitmask as a symbolic name or hex.
func capLabel(bits uint32) string {
	names := core.CapabilityNames(bits)
	if len(names) == 1 {
		return names[0]
	}
	return fmt.Sprintf("0x%08X", bits)
}
