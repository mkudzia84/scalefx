package firmware

// ScaleFX Engine - Firmware Command Handler
// Build, flash, and verify firmware for ScaleFX controllers.
// Uses the reusable firmware library (scalefx/firmware).

import (
	"fmt"
	"scalefx/engine"
	fw "scalefx/firmware"
	"scalefx/protocol"
	"strings"
	"time"
)

// Handler groups all firmware commands.
type Handler struct {
	E *engine.Engine

	// Saved connection state for reconnect after flash
	savedPort string
	savedBaud int
}

// Register adds the firmware command group to the engine.
func Register(eng *engine.Engine) {
	h := &Handler{E: eng}
	eng.AddGroup(h.commands())
}

func (h *Handler) commands() *engine.CmdGroup {
	return &engine.CmdGroup{
		Name:       "Firmware",
		Controller: "",
		Color:      engine.ColorMagenta,
		Commands: map[string]engine.CmdEntry{
			"fw.build":         {h.cmdBuild, "fw.build <controller> [--no-clean]", "Build firmware", false},
			"fw.flash":         {h.cmdFlash, "fw.flash <controller> [--port PORT] [--skip-verify] [--no-clean]", "Build and flash firmware", false},
			"fw.upload":        {h.cmdUpload, "fw.upload <controller> [--port PORT] [--skip-verify]", "Flash firmware without rebuilding", false},
			"fw.verify":        {h.cmdVerify, "fw.verify <controller> [--port PORT]", "Verify device firmware", false},
			"fw.version":       {h.cmdVersion, "fw.version <controller>", "Show firmware version from source", false},
			"fw.controllers":   {h.cmdControllers, "fw.controllers", "List known controller targets", false},
			"fw.ports":         {h.cmdPorts, "fw.ports", "List detected ScaleFX serial ports", false},
			"fw.releases":      {h.cmdReleases, "fw.releases [controller]", "List available GitHub releases", false},
			"fw.notes":         {h.cmdNotes, "fw.notes <controller> [version]", "Show release notes for a version", false},
			"fw.release-flash": {h.cmdReleaseFlash, "fw.release-flash <controller> [version] [--port PORT] [--skip-verify]", "Download and flash from GitHub release", false},
		},
	}
}

// ─── Firmware Command Handlers ───

func (h *Handler) cmdControllers(args []string) {
	h.E.Out.Info("Available controller targets:")
	for _, name := range fw.ControllerNames() {
		ctrl := fw.Controllers[name]
		platform := "Pico"
		if ctrl.IsESP32() {
			platform = "ESP32-S3"
		}
		h.E.Out.Printf("  %-14s %s  (%s)\n", name, platform, ctrl.SubDir)
	}
}

func (h *Handler) cmdVersion(args []string) {
	ctrl, ok := h.resolveController(args)
	if !ok {
		return
	}

	opts := h.fwOptions(ctrl.Name)
	version, buildNum, err := fw.ExtractVersion(opts, ctrl)
	if err != nil {
		h.E.Out.Error("%s", err)
		return
	}

	h.E.Out.Info("%s: v%s (build %d)", ctrl.Name, version, buildNum)
}

func (h *Handler) cmdBuild(args []string) {
	ctrl, ok := h.resolveController(args)
	if !ok {
		return
	}

	noClean := hasFlag(args, "--no-clean")

	opts := h.fwOptions(ctrl.Name)
	opts.NoClean = noClean

	bi, err := fw.Build(opts, ctrl)
	if err != nil {
		h.E.Out.Error("%s", err)
		return
	}

	h.E.Out.OK("Built %s v%s (build %d) — %d bytes", ctrl.Name, bi.Version, bi.BuildNumber, bi.FirmwareSize)
}

func (h *Handler) cmdFlash(args []string) {
	ctrl, ok := h.resolveController(args)
	if !ok {
		return
	}

	port := flagValue(args, "--port")
	skipVerify := hasFlag(args, "--skip-verify")
	noClean := hasFlag(args, "--no-clean")

	h.disconnectIfNeeded(port)

	opts := h.fwOptions(ctrl.Name)
	opts.Port = port
	opts.SkipVerify = skipVerify
	opts.NoClean = noClean

	if err := fw.Run(opts); err != nil {
		h.E.Out.Error("%s", err)
	} else {
		h.reconnectAfterFlash()
	}
}

func (h *Handler) cmdUpload(args []string) {
	ctrl, ok := h.resolveController(args)
	if !ok {
		return
	}

	port := flagValue(args, "--port")
	skipVerify := hasFlag(args, "--skip-verify")

	h.disconnectIfNeeded(port)

	opts := h.fwOptions(ctrl.Name)
	opts.NoBuild = true
	opts.Port = port
	opts.SkipVerify = skipVerify

	if err := fw.Run(opts); err != nil {
		h.E.Out.Error("%s", err)
	} else {
		h.reconnectAfterFlash()
	}
}

func (h *Handler) cmdVerify(args []string) {
	ctrl, ok := h.resolveController(args)
	if !ok {
		return
	}

	port := flagValue(args, "--port")
	if port == "" && h.E.Conn != nil {
		port = h.E.Conn.PortName()
	}

	opts := h.fwOptions(ctrl.Name)
	opts.Port = port

	if err := fw.VerifyDevice(opts, ctrl); err != nil {
		h.E.Out.Error("Verification failed: %s", err)
	}
}

func (h *Handler) cmdPorts(args []string) {
	ports, err := fw.ListScaleFXPorts()
	if err != nil {
		h.E.Out.Error("Cannot enumerate ports: %s", err)
		return
	}
	if len(ports) == 0 {
		h.E.Out.Warning("No ScaleFX devices detected")
		return
	}
	h.E.Out.Info("Detected ScaleFX serial ports:")
	for _, p := range ports {
		label := "Pico"
		if strings.ToUpper(p.VID) == "303A" {
			label = "ESP32"
		}
		h.E.Out.Printf("  %-10s  %-6s  VID:%s PID:%s\n", p.Name, label, p.VID, p.PID)
	}
}

func (h *Handler) cmdReleases(args []string) {
	controller := ""
	for _, a := range args {
		if !strings.HasPrefix(a, "--") {
			controller = a
			break
		}
	}

	// Auto-filter to the connected controller when no explicit argument given
	if controller == "" && h.E.ControllerType != "" {
		if _, ok := fw.Controllers[h.E.ControllerType]; ok {
			controller = h.E.ControllerType
			h.E.Out.Info("Filtering to connected board: %s", controller)
		}
	}

	if controller != "" {
		if _, ok := fw.Controllers[controller]; !ok {
			h.E.Out.Error("Unknown controller: %s (available: %s)", controller, strings.Join(fw.ControllerNames(), ", "))
			return
		}
	}

	h.E.Out.Info("Fetching releases from GitHub...")
	releases, err := fw.FetchReleases(controller, nil)
	if err != nil {
		h.E.Out.Error("Failed to fetch releases: %s", err)
		return
	}
	if len(releases) == 0 {
		if controller != "" {
			h.E.Out.Warning("No releases found for %s", controller)
		} else {
			h.E.Out.Warning("No releases found")
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
		h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, strings.ToUpper(ctrl)))
		for _, r := range rels {
			pre := ""
			if r.Prerelease {
				pre = h.E.Out.C(engine.ColorYellow, " [pre-release]")
			}
			h.E.Out.Printf("    v%-10s  %s  %s%s\n", r.Version, r.Tag, fmtReleaseSize(r.AssetSize), pre)
			if summary := noteSummary(r.Body, 80); summary != "" {
				h.E.Out.Printf("      %s\n", h.E.Out.C(engine.ColorGray, summary))
			}
		}
	}
	h.E.Out.Printf("\n")
	h.E.Out.Info("Total: %d releases", len(releases))
}

func (h *Handler) cmdNotes(args []string) {
	ctrl, ok := h.resolveController(args)
	if !ok {
		return
	}

	version := ""
	positionals := positionalArgs(args)
	if len(positionals) >= 2 {
		version = positionals[1]
	}

	h.E.Out.Info("Fetching releases for %s...", ctrl.Name)
	releases, err := fw.FetchReleases(ctrl.Name, nil)
	if err != nil {
		h.E.Out.Error("Failed to fetch releases: %s", err)
		return
	}
	if len(releases) == 0 {
		h.E.Out.Error("No releases found for %s", ctrl.Name)
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
			h.E.Out.Error("Release v%s not found for %s", version, ctrl.Name)
			h.E.Out.Info("Available versions:")
			for _, r := range releases {
				h.E.Out.Printf("  v%s\n", r.Version)
			}
			return
		}
	} else {
		rel = &releases[0]
	}

	pre := ""
	if rel.Prerelease {
		pre = h.E.Out.C(engine.ColorYellow, " [pre-release]")
	}

	h.E.Out.Printf("\n  %s v%s%s\n", h.E.Out.C(engine.ColorCyan, strings.ToUpper(rel.Controller)), rel.Version, pre)
	h.E.Out.Printf("  %s  |  %s  |  %s\n", rel.Tag, rel.Published, fmtReleaseSize(rel.AssetSize))

	if rel.Body != "" {
		h.E.Out.Printf("\n")
		// Print release notes line-by-line with indent
		for _, line := range strings.Split(rel.Body, "\n") {
			h.E.Out.Printf("  %s\n", line)
		}
	} else {
		h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorGray, "(no release notes)"))
	}
	h.E.Out.Printf("\n")
}

func (h *Handler) cmdReleaseFlash(args []string) {
	ctrl, ok := h.resolveController(args)
	if !ok {
		return
	}

	port := flagValue(args, "--port")
	skipVerify := hasFlag(args, "--skip-verify")
	version := ""

	positionals := positionalArgs(args)
	if len(positionals) >= 2 {
		version = positionals[1]
	}

	h.disconnectIfNeeded(port)

	h.E.Out.Info("Fetching releases for %s...", ctrl.Name)
	releases, err := fw.FetchReleases(ctrl.Name, nil)
	if err != nil {
		h.E.Out.Error("Failed to fetch releases: %s", err)
		return
	}
	if len(releases) == 0 {
		h.E.Out.Error("No releases found for %s", ctrl.Name)
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
			h.E.Out.Error("Release v%s not found for %s", version, ctrl.Name)
			h.E.Out.Info("Available versions:")
			for _, r := range releases {
				h.E.Out.Printf("  v%s\n", r.Version)
			}
			return
		}
	} else {
		rel = &releases[0]
		h.E.Out.Info("Using latest release: v%s", rel.Version)
	}

	pre := ""
	if rel.Prerelease {
		pre = " (pre-release)"
	}
	h.E.Out.Info("Release: %s v%s%s", rel.Controller, rel.Version, pre)

	opts := h.fwOptions(ctrl.Name)
	opts.Port = port
	opts.SkipVerify = skipVerify

	if err := fw.FlashRelease(*rel, opts); err != nil {
		h.E.Out.Error("Flash failed: %s", err)
	} else {
		h.reconnectAfterFlash()
	}
}

// ─── Helpers ───

func (h *Handler) fwOptions(controller string) *fw.Options {
	return &fw.Options{
		Controller: controller,
		Timeout:    15,
		OnEvent:    h.fwEventHandler,
	}
}

func (h *Handler) fwEventHandler(evt fw.Event) {
	switch evt.Type {
	case fw.EventInfo:
		h.E.Out.Info("%s", evt.Message)
	case fw.EventOK:
		h.E.Out.OK("%s", evt.Message)
	case fw.EventWarning:
		h.E.Out.Warning("%s", evt.Message)
	case fw.EventError:
		h.E.Out.Error("%s", evt.Message)
	case fw.EventStep:
		h.E.Out.Info("[%d/%d] %s", evt.Step, evt.Total, evt.Message)
	case fw.EventProgress:
		h.E.Out.Printf("  Progress: %d%%\n", evt.Progress)
	}
}

func (h *Handler) resolveController(args []string) (fw.Controller, bool) {
	name := ""
	for _, a := range args {
		if !strings.HasPrefix(a, "--") {
			name = a
			break
		}
	}

	// Fall back to connected board type when no argument given
	if name == "" && h.E.ControllerType != "" {
		if _, ok := fw.Controllers[h.E.ControllerType]; ok {
			name = h.E.ControllerType
		}
	}

	if name == "" {
		h.E.Out.Error("Controller name required. Available: %s", strings.Join(fw.ControllerNames(), ", "))
		return fw.Controller{}, false
	}

	ctrl, ok := fw.Controllers[name]
	if !ok {
		h.E.Out.Error("Unknown controller: %s (available: %s)", name, strings.Join(fw.ControllerNames(), ", "))
		return fw.Controller{}, false
	}

	return ctrl, true
}

func (h *Handler) disconnectIfNeeded(flashPort string) {
	if h.E.Conn == nil {
		h.savedPort = ""
		h.savedBaud = 0
		return
	}

	if flashPort == "" || strings.EqualFold(h.E.Conn.PortName(), flashPort) {
		h.savedPort = h.E.Conn.PortName()
		h.savedBaud = h.E.Conn.Baud()
		h.E.Out.Warning("Disconnecting from %s for flash operation...", h.savedPort)
		h.E.StopListenerLoop()
		h.E.Conn.Close()
		h.E.Conn = nil
		h.E.API = nil
		h.E.ControllerType = ""
		h.E.Initialized = false
		h.E.Info = nil
	} else {
		h.savedPort = ""
		h.savedBaud = 0
	}
}

// reconnectAfterFlash waits for the serial port to reappear after a flash
// operation and re-establishes the connection.
func (h *Handler) reconnectAfterFlash() {
	if h.savedPort == "" {
		return
	}

	port := h.savedPort
	baud := h.savedBaud
	h.savedPort = ""
	h.savedBaud = 0

	h.E.Out.Info("Waiting for %s to reappear...", port)

	// Wait up to 10 seconds for the port to come back
	deadline := time.Now().Add(10 * time.Second)
	found := false
	for time.Now().Before(deadline) {
		time.Sleep(500 * time.Millisecond)
		for _, p := range protocol.ListPorts() {
			if strings.EqualFold(p, port) {
				found = true
				break
			}
		}
		if found {
			break
		}
	}

	if !found {
		h.E.Out.Warning("Port %s did not reappear — use 'connect' to reconnect manually", port)
		return
	}

	// Small extra delay for device to stabilize
	time.Sleep(500 * time.Millisecond)

	h.E.Out.Info("Reconnecting to %s...", port)
	h.E.Dispatch(fmt.Sprintf("connect %s %d", port, baud))
}

// ─── Package-level helpers ───

func hasFlag(args []string, flag string) bool {
	for _, a := range args {
		if a == flag {
			return true
		}
	}
	return false
}

func flagValue(args []string, flag string) string {
	for i, a := range args {
		if a == flag && i+1 < len(args) {
			return args[i+1]
		}
	}
	return ""
}

func positionalArgs(args []string) []string {
	valueFlags := map[string]bool{"--port": true, "--timeout": true}
	var result []string
	skip := false
	for _, a := range args {
		if skip {
			skip = false
			continue
		}
		if strings.HasPrefix(a, "--") {
			if valueFlags[a] {
				skip = true
			}
			continue
		}
		result = append(result, a)
	}
	return result
}

func fmtReleaseSize(bytes int64) string {
	if bytes >= 1048576 {
		return fmt.Sprintf("%.1f MB", float64(bytes)/1048576)
	}
	if bytes >= 1024 {
		return fmt.Sprintf("%.1f KB", float64(bytes)/1024)
	}
	return fmt.Sprintf("%d B", bytes)
}

// FwControllerNames returns sorted controller names for external use (e.g., GUI).
func FwControllerNames() []string {
	return fw.ControllerNames()
}

// FwControllers returns the controller map for external use (e.g., GUI).
func FwControllers() map[string]fw.Controller {
	return fw.Controllers
}

// FwControllerInfo returns a formatted string describing a controller.
func FwControllerInfo(name string) string {
	ctrl, ok := fw.Controllers[name]
	if !ok {
		return "unknown"
	}
	platform := "Pico (UF2)"
	if ctrl.IsESP32() {
		platform = "ESP32-S3 (UART)"
	}
	return fmt.Sprintf("%s — %s", ctrl.SubDir, platform)
}

// noteSummary returns a truncated single-line summary of markdown release notes.
// Skips headings and metadata lines, returning the first content line trimmed to maxLen.
func noteSummary(body string, maxLen int) string {
	if body == "" {
		return ""
	}
	for _, line := range strings.Split(body, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		// Skip markdown headings, table separators, and metadata lines
		if strings.HasPrefix(line, "#") || strings.HasPrefix(line, "|") || strings.HasPrefix(line, "---") {
			continue
		}
		// Skip lines that look like key-value metadata (e.g. "**Platform:** ...")
		if strings.HasPrefix(line, "**") && strings.Contains(line, ":**") {
			continue
		}
		if len(line) > maxLen {
			return line[:maxLen-3] + "..."
		}
		return line
	}
	return ""
}
