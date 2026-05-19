package main

// ScaleFX CLI - Thin Wrapper
// Delegates all command handling to the shared engine package.
// This file provides only the terminal-specific readline loop,
// Ctrl+C handling, and interactive port selection via stdin.

import (
	"bufio"
	"fmt"
	"os"
	"os/signal"
	"runtime"
	"scalefx/engine"
	"scalefx/engine/handlers"
	"scalefx/engine/handlers/firmware"
	"strconv"
	"strings"
)

// App wraps the shared engine with terminal-specific I/O.
type App struct {
	eng *engine.Engine
}

// NewApp creates a new CLI application.
func NewApp(port string, verbose bool) *App {
	eng := engine.NewEngine(engine.TerminalOutput{}, port, verbose)
	handlers.RegisterDefaults(eng)
	firmware.Register(eng)

	// Wire callbacks for terminal interaction
	eng.PromptSelectPort = promptSelectPort
	eng.OnDisconnect = func() {} // nothing extra needed in CLI

	return &App{eng: eng}
}

// Run starts the interactive CLI loop.
func (a *App) Run() {
	enableVirtualTerminal()
	a.eng.PrintBanner()

	// Auto-connect if port specified
	if a.eng.Port != "" {
		a.eng.Dispatch("connect")
	}

	// Handle Ctrl+C gracefully
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)
	go func() {
		<-sigCh
		fmt.Println("\nInterrupted")
		a.eng.Cleanup()
		os.Exit(0)
	}()

	scanner := bufio.NewScanner(os.Stdin)
	for {
		prompt := a.eng.GetPrompt()
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

		a.eng.Dispatch(line)
	}

	a.eng.Cleanup()
}

// ─── Terminal-Specific Helpers ───

// promptSelectPort presents an interactive port selection menu via stdin.
func promptSelectPort(ports []string) string {
	fmt.Println("Available ports:")
	for i, p := range ports {
		fmt.Printf("  %d) %s\n", i+1, p)
	}
	fmt.Print("Select port number (or press Enter to cancel): ")

	scanner := bufio.NewScanner(os.Stdin)
	if !scanner.Scan() {
		return ""
	}
	input := strings.TrimSpace(scanner.Text())
	if input == "" {
		return ""
	}
	idx, err := strconv.Atoi(input)
	if err != nil || idx < 1 || idx > len(ports) {
		return ""
	}
	return ports[idx-1]
}

// enableVirtualTerminal enables ANSI escape processing on Windows console.
func enableVirtualTerminal() {
	if runtime.GOOS != "windows" {
		return
	}
	// Windows 10+ supports ANSI natively in most terminals.
	// Windows Terminal and VS Code terminal support ANSI natively.
}
