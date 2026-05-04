// Virtual Board — generic CLI driver.
//
// One binary that emulates any of the ScaleFX boards (LightFX,
// GearControl, GunFX, HubFX). The board type is selected at startup
// via -board; per-board behaviour lives in boards/<kind>/.
//
// Each running instance advertises itself via the discovery package so
// scalefx-cli + Studio can list it alongside real serial ports.
//
// Run examples:
//
//   tests/virtual_board.exe                          # LightFX on :9000
//   tests/virtual_board.exe -board gearcontrol       # GearControl on :9000
//   tests/virtual_board.exe -board hubfx -port :9100 # HubFX on :9100
//   tests/virtual_board.exe -board gunfx  -name GunFX-Bench

package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"scalefx/tests/virtual_board/boards/gearcontrol"
	"scalefx/tests/virtual_board/boards/gunfx"
	"scalefx/tests/virtual_board/boards/hubfx"
	"scalefx/tests/virtual_board/boards/lightfx"
	"scalefx/virtualdiscovery"
	"scalefx/tests/virtual_board/server"
)

const defaultPort = ":9000"

func main() {
	var (
		boardKind = flag.String("board", "lightfx",
			"board type to emulate: lightfx | gearcontrol | gunfx | hubfx")
		port = flag.String("port", defaultPort,
			"TCP listen address (\"host:port\" or \":port\"). The advertised port name is `tcp://host:port`")
		name = flag.String("name", "",
			"override the device name advertised in INIT_READY (default \"<Board>-Virtual\")")
		verbose = flag.Bool("verbose", false, "log every packet sent/received")
		noDemo = flag.Bool("no-demo", false,
			"skip the default demo program (LightFX only — boots blank instead of running the demo)")
		// Legacy alias kept so older invocations of the v0 binary keep
		// working. -addr maps to -port.
		legacyAddr = flag.String("addr", "", "alias for -port (deprecated)")
	)
	flag.Parse()

	if *legacyAddr != "" && *port == defaultPort {
		*port = *legacyAddr
	}

	board, err := newBoard(*boardKind, *name)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}

	// Real firmware loads /lightfx.yaml from flash on boot; the virtual
	// board has no flash, so without this the GUI sees a blank board
	// and nobody can tell whether the program runtime is even working.
	// `-no-demo` skips it for tests / clean-slate use.
	if lfx, ok := board.(*lightfx.Board); ok && !*noDemo {
		lfx.LoadDemo()
		log.Printf("virtual_board: lightfx demo program loaded — pass -no-demo to skip")
	}

	srv := server.New(board, server.Options{Addr: *port, Verbose: *verbose})

	advertised := advertiseAddress(*port)
	stop, err := virtualdiscovery.Advertise(virtualdiscovery.Entry{
		Pid:       os.Getpid(),
		Address:   advertised,
		BoardKind: board.BoardKind(),
		Name:      board.Name(),
	})
	if err != nil {
		log.Printf("virtual_board: discovery advertise failed: %v", err)
	}
	defer stop()

	log.Printf("virtual_board: discovery file -> %s/<pid>.json", virtualdiscovery.Dir())
	log.Printf("virtual_board: connect with `%s`", advertised)

	// Stop discovery cleanly on Ctrl+C.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sig
		stop()
		os.Exit(0)
	}()

	if err := srv.Run(); err != nil {
		log.Fatalf("virtual_board: %v", err)
	}
}

func newBoard(kind, name string) (server.Board, error) {
	switch strings.ToLower(kind) {
	case "lightfx":
		return lightfx.New(name), nil
	case "gearcontrol", "gear":
		return gearcontrol.New(name), nil
	case "gunfx", "gun":
		return gunfx.New(name), nil
	case "hubfx", "hub":
		return hubfx.New(name), nil
	default:
		return nil, fmt.Errorf("unknown board %q (try lightfx | gearcontrol | gunfx | hubfx)", kind)
	}
}

// advertiseAddress turns the listen address into a `tcp://...` value
// that scalefx-cli and Studio can pass to protocol.NewConnection. If
// the host part is empty (":9000") we substitute "localhost".
func advertiseAddress(addr string) string {
	if strings.HasPrefix(addr, ":") {
		return "tcp://localhost" + addr
	}
	if strings.HasPrefix(strings.ToLower(addr), "tcp://") {
		return addr
	}
	return "tcp://" + addr
}
