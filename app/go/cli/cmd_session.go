package main

import (
	"fmt"
	"os"
	"sort"
	"strings"

	"scalefx/client"
)

// ─── Registration ────────────────────────────────────────────────────

func init() {
	register(&command{Name: "help", Usage: "help [cmd]", Help: "list commands or describe one", Category: catSession, Run: cmdHelp})
	register(&command{Name: "ports", Usage: "ports", Help: "list attached serial ports", Category: catSession, Run: cmdPorts})
	register(&command{Name: "connect", Usage: "connect <port>", Help: "open a serial port (or tcp://host:port)", Category: catSession, Run: cmdConnect})
	register(&command{Name: "disconnect", Usage: "disconnect", Help: "close the current port", Category: catSession, RequiresConn: true, Run: cmdDisconnect})
	register(&command{Name: "verbose", Usage: "verbose <on|off>", Help: "toggle wire-level packet logging", Category: catSession, Run: cmdVerbose})
	register(&command{Name: "quit", Usage: "quit", Help: "exit the CLI", Category: catSession, Run: func(_ *app, _ []string) error { os.Exit(0); return nil }})

	aliasFor("ls", "files")
	aliasFor("rm", "delete")
	aliasFor("q", "quit")
	aliasFor("?", "help")
	aliasFor("caps", "capabilities")
	aliasFor("id", "identify")
}

// ─── help ────────────────────────────────────────────────────────────

// cmdHelp lists commands grouped by category, hiding ones the current
// connection state can't honour.  `help <name>` prints one verb's
// detail.
func cmdHelp(a *app, args []string) error {
	if len(args) > 0 {
		c, ok := lookup(args[0])
		if !ok {
			return fmt.Errorf("unknown command: %s", args[0])
		}
		Hdr(c.Name)
		KV("usage", c.Usage)
		KV("help", c.Help)
		KV("category", string(c.Category))
		if c.RequiresConn {
			KV("requires", "connection")
		}
		// Reverse-lookup aliases pointing at this command.
		var aka []string
		for short, target := range aliases {
			if target == c.Name {
				aka = append(aka, short)
			}
		}
		if len(aka) > 0 {
			sort.Strings(aka)
			KV("aliases", joinCSV(aka))
		}
		return nil
	}

	connected := a.c != nil
	groups := availableCommands(connected, a.boardCaps)

	if !connected {
		Note("not connected — `connect <port>` first.  Commands shown are session-only;")
		Note("every other verb appears after a successful connect.")
		fmt.Println()
	}

	for _, cat := range categoryOrder {
		list := groups[cat]
		if len(list) == 0 {
			continue
		}
		Hdr(string(cat))
		for _, c := range list {
			fmt.Printf("  %s  %s\n", cBold(padRight(c.Name, 18)), cDim(c.Help))
		}
		fmt.Println()
	}

	// Hint about hidden commands when not connected or when the board
	// doesn't advertise everything.
	hidden := 0
	for _, c := range commands {
		if c.RequiresConn && !connected {
			hidden++
			continue
		}
		if connected && c.RequiresCap != 0 && (a.boardCaps&c.RequiresCap) == 0 {
			hidden++
		}
	}
	if hidden > 0 {
		if connected {
			Note("%d more verb%s hidden — board doesn't advertise the matching capability.",
				hidden, plural(hidden))
		} else {
			Note("%d more verb%s available after `connect`.", hidden, plural(hidden))
		}
	}
	return nil
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}

// ─── ports ───────────────────────────────────────────────────────────

func cmdPorts(_ *app, _ []string) error {
	pl := client.ListSerialPortsDetailed()
	if len(pl) == 0 {
		Note("(no serial ports found)")
		return nil
	}
	Hdr("serial ports")
	for _, p := range pl {
		fmt.Printf("  %s  %s\n", cBold(padRight(p.Name, 12)), cDim(p.Description))
	}
	return nil
}

// ─── connect / disconnect ────────────────────────────────────────────

func cmdConnect(a *app, args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: connect <port>")
	}
	return a.connect(args[0])
}

func cmdDisconnect(a *app, args []string) error {
	if a.c != nil {
		a.c.Close()
		a.c = nil
		a.boardKind = ""
		a.boardName = ""
		a.boardCaps = 0
		Ok("disconnected")
	}
	return nil
}

// ─── verbose ─────────────────────────────────────────────────────────

func cmdVerbose(a *app, args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: verbose <on|off>")
	}
	v := args[0] == "on" || args[0] == "true" || args[0] == "1"
	a.verbose = v
	if a.c != nil {
		a.c.SetVerbose(v)
	}
	if v {
		Ok("wire logging: %s", cYellow("ON"))
	} else {
		Ok("wire logging: %s", cDim("off"))
	}
	return nil
}

// Compile-time keep-alive — strings package is used inside this file
// via `joinCSV` (declared in registry.go).  Without a direct reference
// here go vet would complain on stripped builds.
var _ = strings.TrimSpace
