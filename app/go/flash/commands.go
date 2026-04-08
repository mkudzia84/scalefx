package main

// ScaleFX Flash CLI - Subcommand Implementations
// All commands use the firmware library directly — no engine dependency.

import (
	"fmt"
	"strings"

	fw "scalefx/firmware"
)

// ─── build ───

func cmdBuild(controller string, noClean bool) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}

	opts := makeOpts(ctrl.Name)
	opts.NoClean = noClean

	bi, err := fw.Build(opts, ctrl)
	if err != nil {
		printError("%s", err)
		return
	}
	printOK("Built %s v%s (build %d) — %d bytes", ctrl.Name, bi.Version, bi.BuildNumber, bi.FirmwareSize)
}

// ─── flash (build + flash + verify) ───

func cmdFlash(controller, port string, skipVerify, noClean bool) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}

	opts := makeOpts(ctrl.Name)
	opts.Port = port
	opts.SkipVerify = skipVerify
	opts.NoClean = noClean

	if err := fw.Run(opts); err != nil {
		printError("%s", err)
	}
}

// ─── upload (flash without build) ───

func cmdUpload(controller, port string, skipVerify bool) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}

	opts := makeOpts(ctrl.Name)
	opts.NoBuild = true
	opts.Port = port
	opts.SkipVerify = skipVerify

	if err := fw.Run(opts); err != nil {
		printError("%s", err)
	}
}

// ─── verify ───

func cmdVerify(controller, port string) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}

	opts := makeOpts(ctrl.Name)
	opts.Port = port

	if err := fw.VerifyDevice(opts, ctrl); err != nil {
		printError("Verification failed: %s", err)
	}
}

// ─── version ───

func cmdVersion(controller string) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}

	opts := makeOpts(ctrl.Name)
	version, buildNum, err := fw.ExtractVersion(opts, ctrl)
	if err != nil {
		printError("%s", err)
		return
	}
	printInfo("%s: v%s (build %d)", ctrl.Name, version, buildNum)
}

// ─── controllers ───

func cmdControllers() {
	printInfo("Available controller targets:")
	for _, name := range fw.ControllerNames() {
		ctrl := fw.Controllers[name]
		platform := "Pico"
		if ctrl.IsESP32() {
			platform = "ESP32-S3"
		}
		fmt.Printf("  %-14s %s  (%s)\n", name, platform, ctrl.SubDir)
	}
}

// ─── ports ───

func cmdPorts() {
	ports, err := fw.ListScaleFXPorts()
	if err != nil {
		printError("Cannot enumerate ports: %s", err)
		return
	}
	if len(ports) == 0 {
		printWarning("No ScaleFX devices detected")
		return
	}

	printInfo("Detected ScaleFX serial ports (%d):", len(ports))
	for _, p := range ports {
		label := "Pico"
		vid := strings.ToUpper(p.VID)
		pid := strings.ToUpper(p.PID)
		if (vid == "303A" && pid == "1001") || (vid == "1A86" && pid == "55D3") {
			label = "ESP32"
		}
		fmt.Printf("  %-10s  %-6s  VID:%s PID:%s\n", p.Name, label, p.VID, p.PID)
	}

	// Probe each port with IDENTIFY
	fmt.Println()
	printInfo("Probing devices...")
	for _, p := range ports {
		info := fw.ProbeDevice(p.Name)
		if info != nil {
			printOK("  %-10s  %s v%s (build %d)  [%s @ %dMHz]",
				p.Name, info.Name, info.Version, info.Build, info.Platform, info.CPUMHz)
		} else {
			printWarning("  %-10s  no response to IDENTIFY", p.Name)
		}
	}
}

// ─── tools ───

func cmdTools(subcmd string) {
	opts := makeOpts("")

	switch subcmd {
	case "", "status":
		cmdToolsStatus(opts)
	case "download":
		cmdToolsDownload(opts)
	default:
		printError("Unknown tools subcommand: %s (use: status, download)", subcmd)
	}
}

func cmdToolsStatus(opts *fw.Options) {
	printInfo("External tools status:")
	fmt.Println()

	// esptool
	info := fw.ResolveEsptool(opts)
	if info != nil {
		printOK("  esptool    %s (%s)", info.Path, info.Source)
	} else {
		pythonInfo := fw.ResolveEsptoolOrPython(opts)
		if pythonInfo != nil && pythonInfo.Source == "python" {
			printWarning("  esptool    via Python (legacy fallback)")
			printInfo("             Run 'tools download' for standalone binary (no Python needed)")
		} else {
			printError("  esptool    NOT FOUND")
			printInfo("             Run 'tools download' to install")
		}
	}
}

func cmdToolsDownload(opts *fw.Options) {
	// Check if already installed
	if info := fw.ResolveEsptool(opts); info != nil {
		printOK("esptool already installed at %s", info.Path)
		return
	}

	path, err := fw.DownloadEsptool(opts)
	if err != nil {
		printError("Download failed: %s", err)
		return
	}
	printOK("esptool installed: %s", path)
}

// ─── releases ───

func cmdReleases(controller string) {
	if controller != "" {
		if _, ok := fw.Controllers[controller]; !ok {
			printError("Unknown controller: %s (available: %s)", controller, strings.Join(fw.ControllerNames(), ", "))
			return
		}
	}

	printInfo("Fetching releases from GitHub...")
	releases, err := fw.FetchReleases(controller, nil)
	if err != nil {
		printError("Failed to fetch releases: %s", err)
		return
	}
	if len(releases) == 0 {
		if controller != "" {
			printWarning("No releases found for %s", controller)
		} else {
			printWarning("No releases found")
		}
		return
	}

	grouped := make(map[string][]fw.Release)
	var order []string
	for _, r := range releases {
		if _, seen := grouped[r.Controller]; !seen {
			order = append(order, r.Controller)
		}
		grouped[r.Controller] = append(grouped[r.Controller], r)
	}

	for _, ctrl := range order {
		rels := grouped[ctrl]
		fmt.Printf("\n  %s\n", colorize(cyan, strings.ToUpper(ctrl)))
		for _, r := range rels {
			pre := ""
			if r.Prerelease {
				pre = colorize(yellow, " [pre-release]")
			}
			fmt.Printf("    v%-10s  %s  %s%s\n", r.Version, r.Tag, fmtSize(r.AssetSize), pre)
			if summary := noteSummary(r.Body, 80); summary != "" {
				fmt.Printf("      %s\n", colorize(gray, summary))
			}
		}
	}
	fmt.Println()
	printInfo("Total: %d releases", len(releases))
}

// ─── notes ───

func cmdNotes(controller, version string) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}

	printInfo("Fetching releases for %s...", ctrl.Name)
	releases, err := fw.FetchReleases(ctrl.Name, nil)
	if err != nil {
		printError("Failed to fetch releases: %s", err)
		return
	}
	if len(releases) == 0 {
		printError("No releases found for %s", ctrl.Name)
		return
	}

	var rel *fw.Release
	if version != "" {
		tag := ctrl.Name + "-v" + version
		for i, r := range releases {
			if r.Tag == tag || r.Version == version {
				rel = &releases[i]
				break
			}
		}
		if rel == nil {
			printError("Release v%s not found for %s", version, ctrl.Name)
			printInfo("Available versions:")
			for _, r := range releases {
				fmt.Printf("  v%s\n", r.Version)
			}
			return
		}
	} else {
		rel = &releases[0]
	}

	pre := ""
	if rel.Prerelease {
		pre = colorize(yellow, " [pre-release]")
	}

	fmt.Printf("\n  %s v%s%s\n", colorize(cyan, strings.ToUpper(rel.Controller)), rel.Version, pre)
	fmt.Printf("  %s  |  %s  |  %s\n", rel.Tag, rel.Published, fmtSize(rel.AssetSize))

	if rel.Body != "" {
		fmt.Println()
		for _, line := range strings.Split(rel.Body, "\n") {
			fmt.Printf("  %s\n", line)
		}
	} else {
		fmt.Printf("\n  %s\n", colorize(gray, "(no release notes)"))
	}
	fmt.Println()
}

// ─── release-flash ───

func cmdReleaseFlash(controller, version, port string, skipVerify bool) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}

	printInfo("Fetching releases for %s...", ctrl.Name)
	releases, err := fw.FetchReleases(ctrl.Name, nil)
	if err != nil {
		printError("Failed to fetch releases: %s", err)
		return
	}
	if len(releases) == 0 {
		printError("No releases found for %s", ctrl.Name)
		return
	}

	var rel *fw.Release
	if version != "" {
		tag := ctrl.Name + "-v" + version
		for i, r := range releases {
			if r.Tag == tag || r.Version == version {
				rel = &releases[i]
				break
			}
		}
		if rel == nil {
			printError("Release v%s not found for %s", version, ctrl.Name)
			printInfo("Available versions:")
			for _, r := range releases {
				fmt.Printf("  v%s\n", r.Version)
			}
			return
		}
	} else {
		rel = &releases[0]
		printInfo("Using latest release: v%s", rel.Version)
	}

	pre := ""
	if rel.Prerelease {
		pre = " (pre-release)"
	}
	printInfo("Release: %s v%s%s", rel.Controller, rel.Version, pre)

	opts := makeOpts(ctrl.Name)
	opts.Port = port
	opts.SkipVerify = skipVerify

	if err := fw.FlashRelease(*rel, opts); err != nil {
		printError("Flash failed: %s", err)
	}
}

// ─── Helpers ───

func resolveCtrl(name string) (fw.Controller, bool) {
	if name == "" {
		printError("Controller name required. Available: %s", strings.Join(fw.ControllerNames(), ", "))
		return fw.Controller{}, false
	}
	ctrl, ok := fw.Controllers[name]
	if !ok {
		printError("Unknown controller: %s (available: %s)", name, strings.Join(fw.ControllerNames(), ", "))
		return fw.Controller{}, false
	}
	return ctrl, true
}

func makeOpts(controller string) *fw.Options {
	return &fw.Options{
		Controller: controller,
		Timeout:    15,
		OnEvent:    fwEventHandler,
	}
}
