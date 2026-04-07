package engine

// ScaleFX Engine - Firmware Commands
// Build, flash, and verify firmware for ScaleFX controllers.
// Uses the reusable firmware library (scalefx/firmware).

import (
	"fmt"
	"scalefx/firmware"
	"strings"
)

func (e *Engine) firmwareCommands() *CmdGroup {
	return &CmdGroup{
		Name:       "Firmware",
		Controller: "",
		Color:      ColorMagenta,
		Commands: map[string]CmdEntry{
			"fw.build": {e.cmdFwBuild, "fw.build <controller> [--no-clean]", "Build firmware", false},
			"fw.flash": {e.cmdFwFlash, "fw.flash <controller> [--port PORT] [--skip-verify]", "Build and flash firmware", false},
			"fw.upload": {e.cmdFwUpload, "fw.upload <controller> [--port PORT] [--skip-verify]", "Flash without rebuilding", false},
			"fw.verify": {e.cmdFwVerify, "fw.verify <controller> [--port PORT]", "Verify device firmware", false},
			"fw.version": {e.cmdFwVersion, "fw.version <controller>", "Show firmware version from source", false},
			"fw.controllers": {e.cmdFwControllers, "fw.controllers", "List known controller targets", false},
		},
	}
}

// ─── Firmware Command Handlers ───

// cmdFwControllers lists all known controller targets.
func (e *Engine) cmdFwControllers(args []string) {
	e.Out.Info("Available controller targets:")
	for _, name := range firmware.ControllerNames() {
		ctrl := firmware.Controllers[name]
		platform := "Pico"
		if ctrl.IsESP32() {
			platform = "ESP32-S3"
		}
		e.Out.Printf("  %-14s %s  (%s)\n", name, platform, ctrl.SubDir)
	}
}

// cmdFwVersion shows the firmware version from source.
func (e *Engine) cmdFwVersion(args []string) {
	ctrl, ok := e.resolveController(args)
	if !ok {
		return
	}

	opts := e.fwOptions(ctrl.Name)
	version, buildNum, err := firmware.ExtractVersion(opts, ctrl)
	if err != nil {
		e.Out.Error("%s", err)
		return
	}

	e.Out.Info("%s: v%s (build %d)", ctrl.Name, version, buildNum)
}

// cmdFwBuild builds firmware without flashing.
func (e *Engine) cmdFwBuild(args []string) {
	ctrl, ok := e.resolveController(args)
	if !ok {
		return
	}

	noClean := hasFlag(args, "--no-clean")

	opts := e.fwOptions(ctrl.Name)
	opts.NoClean = noClean

	bi, err := firmware.Build(opts, ctrl)
	if err != nil {
		e.Out.Error("%s", err)
		return
	}

	e.Out.OK("Built %s v%s (build %d) — %d bytes", ctrl.Name, bi.Version, bi.BuildNumber, bi.FirmwareSize)
}

// cmdFwFlash builds and flashes firmware.
func (e *Engine) cmdFwFlash(args []string) {
	ctrl, ok := e.resolveController(args)
	if !ok {
		return
	}

	port := flagValue(args, "--port")
	skipVerify := hasFlag(args, "--skip-verify")
	noClean := hasFlag(args, "--no-clean")

	// If we're connected to this device, disconnect first
	e.disconnectIfNeeded(port)

	opts := e.fwOptions(ctrl.Name)
	opts.Port = port
	opts.SkipVerify = skipVerify
	opts.NoClean = noClean

	if err := firmware.Run(opts); err != nil {
		e.Out.Error("%s", err)
	}
}

// cmdFwUpload flashes without rebuilding (--no-build).
func (e *Engine) cmdFwUpload(args []string) {
	ctrl, ok := e.resolveController(args)
	if !ok {
		return
	}

	port := flagValue(args, "--port")
	skipVerify := hasFlag(args, "--skip-verify")

	e.disconnectIfNeeded(port)

	opts := e.fwOptions(ctrl.Name)
	opts.NoBuild = true
	opts.Port = port
	opts.SkipVerify = skipVerify

	if err := firmware.Run(opts); err != nil {
		e.Out.Error("%s", err)
	}
}

// cmdFwVerify verifies the device firmware without flashing.
func (e *Engine) cmdFwVerify(args []string) {
	ctrl, ok := e.resolveController(args)
	if !ok {
		return
	}

	port := flagValue(args, "--port")
	if port == "" && e.Conn != nil {
		port = e.Conn.PortName()
	}

	opts := e.fwOptions(ctrl.Name)
	opts.Port = port

	if err := firmware.VerifyDevice(opts, ctrl); err != nil {
		e.Out.Error("Verification failed: %s", err)
	}
}

// ─── Helpers ───

// fwOptions creates firmware Options wired to the engine's output.
func (e *Engine) fwOptions(controller string) *firmware.Options {
	return &firmware.Options{
		Controller: controller,
		Timeout:    15,
		OnEvent:    e.fwEventHandler,
	}
}

// fwEventHandler routes firmware events to the engine output.
func (e *Engine) fwEventHandler(evt firmware.Event) {
	switch evt.Type {
	case firmware.EventInfo:
		e.Out.Info("%s", evt.Message)
	case firmware.EventOK:
		e.Out.OK("%s", evt.Message)
	case firmware.EventWarning:
		e.Out.Warning("%s", evt.Message)
	case firmware.EventError:
		e.Out.Error("%s", evt.Message)
	case firmware.EventStep:
		e.Out.Info("[%d/%d] %s", evt.Step, evt.Total, evt.Message)
	case firmware.EventProgress:
		e.Out.Printf("  Progress: %d%%\n", evt.Progress)
	}
}

// resolveController parses the controller name from args.
func (e *Engine) resolveController(args []string) (firmware.Controller, bool) {
	// Find the first non-flag argument
	name := ""
	for _, a := range args {
		if !strings.HasPrefix(a, "--") {
			name = a
			break
		}
	}

	if name == "" {
		e.Out.Error("Controller name required. Available: %s", strings.Join(firmware.ControllerNames(), ", "))
		return firmware.Controller{}, false
	}

	ctrl, ok := firmware.Controllers[name]
	if !ok {
		e.Out.Error("Unknown controller: %s (available: %s)", name, strings.Join(firmware.ControllerNames(), ", "))
		return firmware.Controller{}, false
	}

	return ctrl, true
}

// disconnectIfNeeded disconnects if we're connected to the port being flashed.
func (e *Engine) disconnectIfNeeded(flashPort string) {
	if e.Conn == nil {
		return
	}

	// Disconnect if port matches or no port specified (auto-detect may use this port)
	if flashPort == "" || strings.EqualFold(e.Conn.PortName(), flashPort) {
		e.Out.Warning("Disconnecting from %s for flash operation...", e.Conn.PortName())
		e.Conn.Close()
		e.Conn = nil
		e.API = nil
		e.ControllerType = ""
		e.Initialized = false
		e.Info = nil
	}
}

// hasFlag checks if a flag is present in args.
func hasFlag(args []string, flag string) bool {
	for _, a := range args {
		if a == flag {
			return true
		}
	}
	return false
}

// flagValue returns the value following a flag, or empty string.
func flagValue(args []string, flag string) string {
	for i, a := range args {
		if a == flag && i+1 < len(args) {
			return args[i+1]
		}
	}
	return ""
}

// fwControllerArg is a helper exported to allow the GUI to use controller name resolution.
func FwControllerNames() []string {
	return firmware.ControllerNames()
}

// FwControllers returns the controller map for GUI use.
func FwControllers() map[string]firmware.Controller {
	return firmware.Controllers
}

// FwControllerInfo returns a formatted string describing a controller.
func FwControllerInfo(name string) string {
	ctrl, ok := firmware.Controllers[name]
	if !ok {
		return "unknown"
	}
	platform := "Pico (UF2)"
	if ctrl.IsESP32() {
		platform = "ESP32-S3 (UART)"
	}
	return fmt.Sprintf("%s — %s", ctrl.SubDir, platform)
}
