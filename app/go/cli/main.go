package main

// ScaleFX CLI - Entry Point
// Thin wrapper around the shared engine package.
// Build: go build -o scalefx-cli.exe .

import (
	"flag"
	"fmt"
	"os"
	"scalefx/protocol"
)

func main() {
	port := flag.String("port", "", "Serial port (e.g., COM5, /dev/ttyACM0)")
	flag.StringVar(port, "p", "", "Serial port (shorthand)")

	verbose := flag.Bool("verbose", false, "Enable verbose protocol logging")
	flag.BoolVar(verbose, "v", false, "Verbose (shorthand)")

	listPortsFlag := flag.Bool("list", false, "List available serial ports and exit")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "ScaleFX CLI — compiled interactive client for ScaleFX controllers\n\n")
		fmt.Fprintf(os.Stderr, "Usage: %s [options]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Options:\n")
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, "\nExamples:\n")
		fmt.Fprintf(os.Stderr, "  %s -p COM5              Connect to COM5\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s -p COM5 -v           Verbose mode\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s --list               List serial ports\n", os.Args[0])
	}

	flag.Parse()

	if *listPortsFlag {
		ports := protocol.ListPorts()
		if len(ports) == 0 {
			fmt.Println("No serial ports found")
		} else {
			for _, p := range ports {
				fmt.Println(p)
			}
		}
		return
	}

	app := NewApp(*port, *verbose)
	app.Run()
}
