// Command scalefx-cli — terminal client for the ScaleFX HubFX master and
// every expander reachable through it.  Speaks the generic-expander wire
// protocol: hub-local commands (audio / storage / files) plus
// GUID-addressed topology commands that talk to remote boards through
// the hub's TopologyServicePolicy.
//
// Usage:
//
//	scalefx-cli                # interactive REPL, prompts for port
//	scalefx-cli -p COM5        # open COM5 on launch
//	scalefx-cli -p tcp://localhost:5050   # virtual-board harness
package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"scalefx/client"
)

var (
	flagPort    = flag.String("p", "", "Serial port to open at start (e.g. COM5 or tcp://host:port)")
	flagBaud    = flag.Int("b", 0, "Baud rate override (0 = default 6 Mbps)")
	flagVerbose = flag.Bool("v", false, "Verbose wire logging")
	flagCmd     = flag.String("c", "", "Run a single command then exit")
)

func main() {
	flag.Parse()

	app := newApp()
	defer app.shutdown()

	// Auto-connect if -p was supplied.
	if *flagPort != "" {
		if err := app.connect(*flagPort); err != nil {
			fmt.Fprintf(os.Stderr, "connect: %v\n", err)
			os.Exit(1)
		}
	}

	// Single-shot mode (-c "command args").
	if *flagCmd != "" {
		if err := app.dispatch(*flagCmd); err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(2)
		}
		return
	}

	// Interactive REPL.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sig
		fmt.Println("\ngoodbye.")
		app.shutdown()
		os.Exit(0)
	}()

	app.banner()
	reader := bufio.NewReader(os.Stdin)
	for {
		fmt.Print(app.prompt())
		line, err := reader.ReadString('\n')
		if err != nil {
			fmt.Println()
			return
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if line == "quit" || line == "exit" {
			return
		}
		if err := app.dispatch(line); err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
		}
	}
}

// ─── App ────────────────────────────────────────────────────────────

type app struct {
	c       *client.Client
	verbose bool
}

func newApp() *app { return &app{verbose: *flagVerbose} }

func (a *app) shutdown() {
	if a.c != nil {
		a.c.Close()
		a.c = nil
	}
}

func (a *app) prompt() string {
	if a.c == nil {
		return "scalefx> "
	}
	return fmt.Sprintf("scalefx (%s)> ", a.c.PortName())
}

func (a *app) banner() {
	fmt.Println("ScaleFX CLI — generic-expander build")
	fmt.Println("type `help` for commands, `quit` to exit")
}

func (a *app) connect(portName string) error {
	if a.c != nil {
		a.c.Close()
		a.c = nil
	}
	opts := client.Options{
		Baud:    *flagBaud,
		Verbose: a.verbose,
	}
	c, err := client.OpenWith(portName, opts)
	if err != nil {
		return err
	}
	a.c = c
	fmt.Printf("connected: %s\n", portName)

	// Auto-detect: pull IDENTIFY and surface what kind of board this is
	// + what subsystems it ships with.  The topology snapshot fields
	// (numServos, audioChannels, ...) come from the Rule 11 tail added
	// in firmware build ≥ 26 — older firmware leaves them zero, in
	// which case we just fall back to the capability bits.
	if id, err := c.Hub.Identify(); err == nil {
		if strings.Contains(strings.ToLower(id.Platform), "esp32") {
			c.Storage.SetPeerMaxPayload(client.Esp32MaxPayload)
		} else {
			c.Storage.SetPeerMaxPayload(client.PicoMaxPayload)
		}
		printIdentityBanner(id)
	}
	return nil
}

// printIdentityBanner shows the board type, firmware, and feature
// catalog in a compact connect-time summary.  The features come from
// the IDENTIFY capabilities bitmask — see protocol/core for the catalog.
func printIdentityBanner(id client.Identity) {
	kind := "unknown"
	if k := id.Kind(); k != client.BoardUnknown {
		kind = string(k)
	}
	fmt.Printf("  board    : %s — %s v%s build %d (%s)\n",
		kind, id.DeviceName, id.FirmwareVersion, id.BuildNumber, id.Platform)

	if caps := id.CapabilityNames(); len(caps) > 0 {
		fmt.Printf("  features : %s\n", strings.Join(caps, " · "))
	} else {
		fmt.Println("  features : (firmware pre-dates IDENTIFY capabilities field)")
	}
}

func (a *app) requireClient() error {
	if a.c == nil {
		return fmt.Errorf("not connected — `connect <port>` first")
	}
	return nil
}

// dispatch parses one input line and runs the matching command.
func (a *app) dispatch(line string) error {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return nil
	}
	name, args := fields[0], fields[1:]
	cmd, ok := commands[name]
	if !ok {
		// Try alias lookup.
		if alt, ok2 := aliases[name]; ok2 {
			name = alt
			cmd = commands[name]
			ok = true
		}
	}
	if !ok {
		return fmt.Errorf("unknown command: %s (try `help`)", name)
	}
	return cmd.Run(a, args)
}
