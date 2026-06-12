package main

// ScaleFX Flash CLI
// Standalone firmware build & flash tool.
//
// Usage (argument mode):
//   scalefx-flash build <controller> [--no-clean] [--no-bump]
//   scalefx-flash flash <controller> [--port PORT] [--skip-verify] [--no-clean]
//   scalefx-flash upload <controller> [--port PORT] [--skip-verify]
//   scalefx-flash verify <controller> [--port PORT]
//   scalefx-flash version <controller>
//   scalefx-flash controllers
//   scalefx-flash ports
//   scalefx-flash releases [controller]
//   scalefx-flash notes <controller> [version]
//   scalefx-flash release-flash <controller> [version] [--port PORT] [--skip-verify]
//
// Usage (interactive mode):
//   scalefx-flash              (launches REPL)
//   scalefx-flash -i           (explicit interactive)
//
// Build:
//   cd app/go && go build -o scalefx-flash.exe ./flash/

import (
	"fmt"
	"os"
	"strings"
)

func main() {
	args := os.Args[1:]

	// No args or explicit -i → interactive mode
	if len(args) == 0 || (len(args) == 1 && (args[0] == "-i" || args[0] == "--interactive")) {
		runInteractive()
		return
	}

	// Help
	if args[0] == "-h" || args[0] == "--help" || args[0] == "help" {
		printUsage()
		return
	}

	// Subcommand dispatch
	cmd := args[0]
	sub := args[1:]

	switch cmd {
	case "build":
		controller, flags := extractPositionalAndFlags(sub)
		if controller == "" {
			printError("Usage: scalefx-flash build <controller> [--no-clean] [--no-bump]")
			return
		}
		cmdBuild(controller, hasArg(flags, "--no-clean"), hasArg(flags, "--no-bump"))

	case "flash":
		controller, flags := extractPositionalAndFlags(sub)
		if controller == "" {
			printError("Usage: scalefx-flash flash <controller> [--port PORT] [--skip-verify] [--no-clean]")
			return
		}
		port := argValue(flags, "--port")
		cmdFlash(controller, port, hasArg(flags, "--skip-verify"), hasArg(flags, "--no-clean"))

	case "upload":
		controller, flags := extractPositionalAndFlags(sub)
		if controller == "" {
			printError("Usage: scalefx-flash upload <controller> [--port PORT] [--skip-verify]")
			return
		}
		port := argValue(flags, "--port")
		cmdUpload(controller, port, hasArg(flags, "--skip-verify"))

	case "verify":
		controller, flags := extractPositionalAndFlags(sub)
		if controller == "" {
			printError("Usage: scalefx-flash verify <controller> [--port PORT]")
			return
		}
		port := argValue(flags, "--port")
		cmdVerify(controller, port)

	case "programs":
		// Deploy the bundled lightfx programs to a HubFX WITHOUT reflashing.
		_, flags := extractPositionalAndFlags(sub)
		deployLightFxPrograms(argValue(flags, "--port"))

	case "coredump":
		// Pull + decode the ESP32 crash coredump from flash (HubFX).
		controller, flags := extractPositionalAndFlags(sub)
		if controller == "" {
			controller = "hubfx"
		}
		cmdCoredump(controller, argValue(flags, "--port"), argValue(flags, "--save"),
			hasArg(flags, "--raw"))

	case "version":
		controller, _ := extractPositionalAndFlags(sub)
		if controller == "" {
			printError("Usage: scalefx-flash version <controller>")
			return
		}
		cmdVersion(controller)

	case "controllers":
		cmdControllers()

	case "ports":
		cmdPorts()

	case "tools":
		subcmd := ""
		if len(sub) > 0 {
			subcmd = sub[0]
		}
		cmdTools(subcmd)

	case "releases":
		controller := ""
		for _, a := range sub {
			if !strings.HasPrefix(a, "--") {
				controller = a
				break
			}
		}
		cmdReleases(controller)

	case "notes":
		positionals, _ := extractPositionals(sub)
		if len(positionals) < 1 {
			printError("Usage: scalefx-flash notes <controller> [version]")
			return
		}
		version := ""
		if len(positionals) >= 2 {
			version = positionals[1]
		}
		cmdNotes(positionals[0], version)

	case "release-flash":
		positionals, flags := extractPositionals(sub)
		if len(positionals) < 1 {
			printError("Usage: scalefx-flash release-flash <controller> [version] [--port PORT] [--skip-verify]")
			return
		}
		version := ""
		if len(positionals) >= 2 {
			version = positionals[1]
		}
		port := argValue(flags, "--port")
		cmdReleaseFlash(positionals[0], version, port, hasArg(flags, "--skip-verify"))

	default:
		printError("Unknown command: %s", cmd)
		fmt.Println()
		printUsage()
		os.Exit(1)
	}
}

func printUsage() {
	fmt.Printf("%s — firmware build & flash tool\n\n", colorize(bold+magenta, "ScaleFX Flash"))
	fmt.Println("Usage:")
	fmt.Println("  scalefx-flash <command> [args] [flags]")
	fmt.Println("  scalefx-flash                           (interactive mode)")
	fmt.Println("  scalefx-flash -i                        (interactive mode)")
	fmt.Println()
	fmt.Println("Commands:")
	fmt.Printf("  %-16s %s\n", "build", "Build firmware for a controller")
	fmt.Printf("  %-16s %s\n", "flash", "Build + flash + verify (full pipeline)")
	fmt.Printf("  %-16s %s\n", "upload", "Flash without rebuilding")
	fmt.Printf("  %-16s %s\n", "verify", "Verify device firmware version")
	fmt.Printf("  %-16s %s\n", "programs", "Deploy bundled lightfx programs to a HubFX (no reflash)")
	fmt.Printf("  %-16s %s\n", "coredump", "Pull + decode the ESP32 crash backtrace from flash (HubFX)")
	fmt.Printf("  %-16s %s\n", "version", "Show firmware version from source")
	fmt.Printf("  %-16s %s\n", "controllers", "List available controller targets")
	fmt.Printf("  %-16s %s\n", "ports", "List detected ScaleFX serial ports")
	fmt.Printf("  %-16s %s\n", "tools", "Manage external tools (esptool)")
	fmt.Printf("  %-16s %s\n", "releases", "List available GitHub releases")
	fmt.Printf("  %-16s %s\n", "notes", "Show release notes for a version")
	fmt.Printf("  %-16s %s\n", "release-flash", "Download and flash from GitHub release")
	fmt.Println()
	fmt.Println("Flags:")
	fmt.Printf("  %-16s %s\n", "--port PORT", "Serial port (default: auto-detect)")
	fmt.Printf("  %-16s %s\n", "--skip-verify", "Skip post-flash device verification")
	fmt.Printf("  %-16s %s\n", "--no-clean", "Incremental build (skip clean)")
	fmt.Printf("  %-16s %s\n", "--no-bump", "Verification build: keep BUILD_NUMBER unchanged (test gate / CI)")
	fmt.Println()
	fmt.Println("Examples:")
	fmt.Println("  scalefx-flash build gunfx")
	fmt.Println("  scalefx-flash flash gearcontrol --port COM5")
	fmt.Println("  scalefx-flash upload hubfx --skip-verify")
	fmt.Println("  scalefx-flash release-flash gunfx 0.7.0")
	fmt.Println("  scalefx-flash releases gunfx")
	fmt.Println("  scalefx-flash ports")
}
