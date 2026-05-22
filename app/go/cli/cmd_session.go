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
	// Active section uses the runtime-enabled mask (compiled-in AND
	// turned on by config).  Disabled section uses the compiled-but-off
	// delta so the user can see what they could turn on.
	groups := availableCommands(connected, a.boardEnabledCaps)
	disabled := disabledCommands(connected, a.boardCaps, a.boardEnabledCaps)

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

	// Disabled section — compiled in but config-disabled.  Show them
	// so the user knows the verb exists and what to flip in the YAML.
	disabledCount := 0
	for _, list := range disabled {
		disabledCount += len(list)
	}
	if disabledCount > 0 {
		Hdr("Currently disabled (compiled in; turn on in config to use)")
		for _, cat := range categoryOrder {
			for _, c := range disabled[cat] {
				fmt.Printf("  %s  %s  %s\n",
					cDim(padRight(c.Name, 18)),
					cDim(c.Help),
					cDim("[disabled]"))
			}
		}
		fmt.Println()
	}

	// Hint about not-compiled-in verbs.
	notCompiled := 0
	for _, c := range commands {
		if c.RequiresConn && !connected {
			notCompiled++
			continue
		}
		if connected && c.RequiresCap != 0 && (a.boardCaps&c.RequiresCap) == 0 {
			notCompiled++
		}
	}
	if notCompiled > 0 {
		if connected {
			Note("%d more verb%s hidden — board firmware doesn't include the matching capability.",
				notCompiled, plural(notCompiled))
		} else {
			Note("%d more verb%s available after `connect`.", notCompiled, plural(notCompiled))
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
