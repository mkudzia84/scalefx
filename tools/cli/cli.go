package main

// ScaleFX CLI - Interactive Command Loop
// Slim coordinator: dispatch, help, listener, and command group aggregation.
// Handler implementations are in handler_*.go files.

import (
	"bufio"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"sync/atomic"
	"time"
)

// ─── Types ───

// cmdEntry defines a single CLI command.
type cmdEntry struct {
	handler     func(args []string)
	usage       string
	description string
	requireInit bool
}

// cmdGroup defines a group of related commands (one per controller type).
type cmdGroup struct {
	Name       string
	Controller string // empty = universal
	Color      string
	Commands   map[string]cmdEntry
}

// CLI is the interactive command-line interface.
type CLI struct {
	conn           *Connection
	port           string
	verbose        bool
	controllerType string
	initialized    bool
	info           *InitReadyInfo

	// Command groups (populated on first call to getGroups)
	groups   []*cmdGroup
	flatCmds map[string]flatEntry // cached on first dispatch

	// Listener control
	listenerRunning atomic.Bool
	stopListener    chan struct{}

	// Keepalive
	keepaliveInterval time.Duration
}

// NewCLI creates a new CLI instance.
func NewCLI(port string, verbose bool) *CLI {
	return &CLI{
		port:              port,
		verbose:           verbose,
		keepaliveInterval: 5 * time.Second,
	}
}

// ─── Command Registration ───

// getGroups returns all command groups, building them once.
func (c *CLI) getGroups() []*cmdGroup {
	if c.groups == nil {
		c.groups = []*cmdGroup{
			c.coreCommands(),
			c.gunfxCommands(),
			c.gearcontrolCommands(),
			c.lightfxCommands(),
			c.hubfxCommands(),
		}
	}
	return c.groups
}

// flatCommands returns all commands across all groups, keyed by command name.
// Also stores the controller type for each command for dispatch checks.
type flatEntry struct {
	cmdEntry
	controller string
}

func (c *CLI) flatCommands() map[string]flatEntry {
	if c.flatCmds != nil {
		return c.flatCmds
	}
	cmds := make(map[string]flatEntry)
	for _, g := range c.getGroups() {
		for name, entry := range g.Commands {
			cmds[name] = flatEntry{entry, g.Controller}
		}
	}
	// Built-in aliases
	cmds["help"] = flatEntry{cmdEntry{c.cmdHelp, "help [command]", "Show help", false}, ""}
	cmds["?"] = flatEntry{cmdEntry{c.cmdHelp, "?", "Show help", false}, ""}
	c.flatCmds = cmds
	return cmds
}

// ─── Main Loop ───

// Run starts the interactive CLI.
func (c *CLI) Run() {
	EnableVirtualTerminal()
	PrintBanner()

	// Auto-connect if port specified
	if c.port != "" {
		c.cmdConnect(nil)
	}

	// Handle Ctrl+C gracefully
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)
	go func() {
		<-sigCh
		fmt.Println("\nInterrupted")
		c.cleanup()
		os.Exit(0)
	}()

	scanner := bufio.NewScanner(os.Stdin)
	for {
		prompt := GetPrompt(c.controllerType, c.initialized)
		fmt.Print(prompt)

		if !scanner.Scan() {
			break
		}
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}

		if line == "quit" || line == "exit" || line == "q" {
			break
		}

		c.dispatch(line)
	}

	c.cleanup()
}

func (c *CLI) cleanup() {
	if c.conn != nil {
		c.stopListenerLoop()
		c.conn.Close()
	}
}

// ─── Command Dispatch ───

func (c *CLI) dispatch(line string) {
	parts := strings.Fields(line)
	if len(parts) == 0 {
		return
	}
	cmd := strings.ToLower(parts[0])
	args := parts[1:]

	cmds := c.flatCommands()
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
			cmd = matches[0]
			ok = true
		} else if len(matches) > 1 {
			PrintError("Ambiguous command '%s'. Matches: %s", cmd, strings.Join(matches, ", "))
			return
		} else {
			PrintError("Unknown command: %s (type 'help' for commands)", cmd)
			return
		}
	}

	// Check connection
	if entry.requireInit || (entry.controller != "" && c.conn == nil) {
		if c.conn == nil {
			PrintError("Not connected. Use 'connect <port>' first.")
			return
		}
	}

	// Check init
	if entry.requireInit && !c.initialized {
		PrintWarning("Controller not initialized. Use 'init' first.")
	}

	// Check controller type mismatch
	if entry.controller != "" && c.controllerType != "" && entry.controller != c.controllerType {
		PrintWarning("Command '%s' is for %s but connected to %s",
			cmd, controllerLabels[entry.controller], controllerLabels[c.controllerType])
	}

	entry.handler(args)
}

// ─── Listener Goroutine (keepalive + async packets) ───

func (c *CLI) startListenerLoop() {
	if c.conn == nil || c.listenerRunning.Load() {
		return
	}

	// Register async callback so the reader goroutine delivers
	// unsolicited packets (log messages, gear status, etc.) to us.
	c.conn.SetCallback(c.handleAsyncPacket)

	c.stopListener = make(chan struct{})
	c.listenerRunning.Store(true)

	go func() {
		lastKeepalive := time.Now()
		for {
			select {
			case <-c.stopListener:
				c.listenerRunning.Store(false)
				return
			default:
			}

			// Send keepalive periodically
			if time.Since(lastKeepalive) >= c.keepaliveInterval {
				_ = c.conn.Send(CmdKeepalive())
				lastKeepalive = time.Now()
			}

			time.Sleep(100 * time.Millisecond)
		}
	}()
}

func (c *CLI) stopListenerLoop() {
	if c.listenerRunning.Load() {
		close(c.stopListener)
		// Wait briefly for goroutine to stop
		time.Sleep(150 * time.Millisecond)
	}
}

func (c *CLI) handleAsyncPacket(resp *Response) {
	switch resp.PacketType {
	case CoreLOG_MESSAGE:
		ParseLogMessage(resp.Payload)
	case GcGEAR_CALIB_STATUS:
		ParseGearCalibStatus(resp.Payload)
	case GcGEAR_SEQ_STATUS:
		ParseGearSeqStatus(resp.Payload)
	case GcGEAR_DOOR_STATUS:
		ParseGearDoorStatus(resp.Payload)
	case LfxLANDING_LIGHT_STATUS:
		ParseLandingLightStatus(resp.Payload)
	case CoreACK:
		if c.verbose {
			PrintInfo("  [async ACK tag=%d]", resp.Tag)
		}
	case CoreNACK:
		errCode := byte(0)
		if len(resp.Payload) > 0 {
			errCode = resp.Payload[0]
		}
		PrintWarning("  [async NACK tag=%d error=%s]", resp.Tag, ErrorName(errCode))
	default:
		if c.verbose {
			fmt.Printf("  [async 0x%02X tag=%d len=%d]\n", resp.PacketType, resp.Tag, len(resp.Payload))
		}
	}
}

// ─── Helper: Send and expect ACK ───

func (c *CLI) sendACK(pkt []byte, successMsg string) {
	resp, err := c.conn.SendExpectACK(pkt)
	if err != nil {
		PrintError("%v", err)
		return
	}
	PrintACKResult(resp, successMsg)
}

// ─── Help System ───

func (c *CLI) cmdHelp(args []string) {
	cmds := c.flatCommands()

	// Single command help
	if len(args) > 0 {
		cmd := strings.ToLower(args[0])
		if entry, ok := cmds[cmd]; ok {
			fmt.Printf("  %s — %s\n", entry.usage, entry.description)
			return
		}
		PrintError("Unknown command: %s", cmd)
		return
	}

	// Show connection status at top
	if c.info != nil {
		PrintConnectionStatus(c.controllerType, c.initialized, c.info)
	}

	// Group help — filter by connected controller
	for _, g := range c.getGroups() {
		// Skip controller-specific groups if connected to a different controller
		if g.Controller != "" && c.controllerType != "" && c.controllerType != g.Controller {
			continue
		}
		PrintGroupHelp(g)
	}

	// "help" and "quit" at the bottom
	fmt.Printf("\n%s── Other %s\n", colorGray, strings.Repeat("─", 42)+colorReset)
	fmt.Printf("  %s%-40s%s  %s%s%s\n", colorGray, "help [command]", colorReset, colorGray, "Show help", colorReset)
	fmt.Printf("  %s%-40s%s  %s%s%s\n", colorGray, "quit", colorReset, colorGray, "Exit CLI", colorReset)

	// Hint about hidden commands when connected
	if c.controllerType != "" {
		hiddenCount := 0
		for _, g := range c.getGroups() {
			if g.Controller != "" && g.Controller != c.controllerType {
				hiddenCount += len(g.Commands)
			}
		}
		if hiddenCount > 0 {
			fmt.Printf("\n  %s(%d commands for other controllers hidden)%s\n", colorGray, hiddenCount, colorReset)
		}
	}

	fmt.Println()
}
