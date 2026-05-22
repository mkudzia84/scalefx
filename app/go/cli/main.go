// Command scalefx-cli — terminal client for the ScaleFX HubFX master and
// every expander reachable through it.  A thin REPL over the shared
// `scalefx/console` command package (the same command set ScaleFX Studio
// drives in its GUI console).
//
// Usage:
//
//	scalefx-cli                # interactive REPL, prompts for port
//	scalefx-cli -p COM5        # open COM5 on launch
//	scalefx-cli -c "topo-ports"   # run one command then exit
package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"scalefx/console"
)

var (
	flagPort    = flag.String("p", "", "Serial port to open at start (e.g. COM5)")
	flagBaud    = flag.Int("b", 0, "Baud rate override (0 = default 6 Mbps)")
	flagVerbose = flag.Bool("v", false, "Verbose wire logging")
	flagCmd     = flag.String("c", "", "Run a single command then exit")
	flagNoColor = flag.Bool("no-color", false, "Disable ANSI color output")
)

func main() {
	flag.Parse()
	if *flagNoColor {
		console.SetColor(false)
	}

	app := console.NewApp(*flagBaud, *flagVerbose)
	defer app.Shutdown()

	// Auto-connect if -p was supplied.
	if *flagPort != "" {
		if err := app.Connect(*flagPort); err != nil {
			fmt.Fprintf(os.Stderr, "connect: %v\n", err)
			os.Exit(1)
		}
	}

	// Single-shot mode (-c "command args").
	if *flagCmd != "" {
		if err := app.Dispatch(*flagCmd); err != nil {
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
		app.Shutdown()
		os.Exit(0)
	}()

	app.Banner()
	reader := bufio.NewReader(os.Stdin)
	for {
		fmt.Print(app.Prompt())
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
		if err := app.Dispatch(line); err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
		}
	}
}
