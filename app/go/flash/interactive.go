package main

// ScaleFX Flash CLI - Interactive Mode
// REPL-style loop when no subcommand is given.

import (
	"bufio"
	"fmt"
	"os"
	"os/signal"
	"runtime"
	"strings"
)

// runInteractive enters a REPL loop with the same commands as argument mode.
func runInteractive() {
	enableVT()
	printBanner()
	printInteractiveHelp()

	// Handle Ctrl+C
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)
	go func() {
		<-sigCh
		fmt.Println("\nInterrupted")
		os.Exit(0)
	}()

	scanner := bufio.NewScanner(os.Stdin)
	for {
		fmt.Print(colorize(magenta, "flash") + "> ")

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

		dispatchInteractive(line)
	}
}

func dispatchInteractive(line string) {
	parts := strings.Fields(line)
	if len(parts) == 0 {
		return
	}
	cmd := parts[0]
	args := parts[1:]

	switch cmd {
	case "build":
		controller, flags := extractPositionalAndFlags(args)
		cmdBuild(controller, hasArg(flags, "--no-clean"), hasArg(flags, "--no-bump"))

	case "flash":
		controller, flags := extractPositionalAndFlags(args)
		port := argValue(flags, "--port")
		cmdFlash(controller, port, hasArg(flags, "--skip-verify"), hasArg(flags, "--no-clean"))

	case "upload":
		controller, flags := extractPositionalAndFlags(args)
		port := argValue(flags, "--port")
		cmdUpload(controller, port, hasArg(flags, "--skip-verify"))

	case "verify":
		controller, flags := extractPositionalAndFlags(args)
		port := argValue(flags, "--port")
		cmdVerify(controller, port)

	case "version":
		controller, _ := extractPositionalAndFlags(args)
		cmdVersion(controller)

	case "controllers":
		cmdControllers()

	case "ports":
		cmdPorts()

	case "tools":
		subcmd := ""
		if len(args) > 0 {
			subcmd = args[0]
		}
		cmdTools(subcmd)

	case "releases":
		controller, _ := extractPositionalAndFlags(args)
		cmdReleases(controller)

	case "notes":
		positionals, _ := extractPositionals(args)
		controller := ""
		version := ""
		if len(positionals) >= 1 {
			controller = positionals[0]
		}
		if len(positionals) >= 2 {
			version = positionals[1]
		}
		cmdNotes(controller, version)

	case "release-flash":
		positionals, flags := extractPositionals(args)
		controller := ""
		version := ""
		if len(positionals) >= 1 {
			controller = positionals[0]
		}
		if len(positionals) >= 2 {
			version = positionals[1]
		}
		port := argValue(flags, "--port")
		cmdReleaseFlash(controller, version, port, hasArg(flags, "--skip-verify"))

	case "help", "?":
		printInteractiveHelp()

	default:
		printError("Unknown command: %s (type 'help' for available commands)", cmd)
	}
}

func printInteractiveHelp() {
	fmt.Println("  Commands:")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "build <controller> [--no-clean] [--no-bump]"), "Build firmware")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "flash <controller> [flags]"), "Build + flash + verify")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "upload <controller> [flags]"), "Flash without rebuilding")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "verify <controller> [--port PORT]"), "Verify device firmware")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "version <controller>"), "Show firmware version from source")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "controllers"), "List controller targets")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "ports"), "List detected serial ports")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "tools [status|download]"), "Manage external tools")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "releases [controller]"), "List GitHub releases")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "notes <controller> [version]"), "Show release notes")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "release-flash <controller> [ver] [flags]"), "Flash from GitHub release")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "help"), "Show this help")
	fmt.Printf("    %-42s %s\n", colorize(cyan, "quit"), "Exit")
	fmt.Println()
	fmt.Printf("  Flags: %s, %s, %s, %s\n",
		colorize(gray, "--port PORT"),
		colorize(gray, "--skip-verify"),
		colorize(gray, "--no-clean"),
		colorize(gray, "--no-bump"))
	fmt.Println()
}

// ─── Helpers ───

// extractPositionalAndFlags returns the first positional arg and all remaining args.
func extractPositionalAndFlags(args []string) (string, []string) {
	for i, a := range args {
		if !strings.HasPrefix(a, "--") {
			return a, append(args[:i:i], args[i+1:]...)
		}
	}
	return "", args
}

// extractPositionals returns all positional args and all flag args separately.
func extractPositionals(args []string) (positionals, flags []string) {
	valueFlags := map[string]bool{"--port": true}
	skip := false
	for _, a := range args {
		if skip {
			flags = append(flags, a)
			skip = false
			continue
		}
		if strings.HasPrefix(a, "--") {
			flags = append(flags, a)
			if valueFlags[a] {
				skip = true
			}
			continue
		}
		positionals = append(positionals, a)
	}
	return
}

func hasArg(args []string, flag string) bool {
	for _, a := range args {
		if a == flag {
			return true
		}
	}
	return false
}

func argValue(args []string, flag string) string {
	for i, a := range args {
		if a == flag && i+1 < len(args) {
			return args[i+1]
		}
	}
	return ""
}

func enableVT() {
	if runtime.GOOS != "windows" {
		return
	}
	// Windows 10+ / VS Code terminal support ANSI natively
}
