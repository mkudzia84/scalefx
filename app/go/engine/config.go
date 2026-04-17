package engine

// ScaleFX Engine - Shared Config Commands
// Reusable config command implementations (config.reload, config.status, config.save, config.validate).
// Eliminates duplication across GearControl, LightFX, and HubFX handlers.

import (
	"os"
	"path/filepath"
	"scalefx/protocol"
)

// ─── Shared Config Command Handlers ───

// ConfigReload sends CONFIG_RELOAD to the hub for the connected controller.
func (e *Engine) ConfigReload(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	e.Ack(e.API.HubFx.ConfigReload(path), "Config reload")
}

// ConfigStatus queries CONFIG_STATUS from the hub and displays the result.
func (e *Engine) ConfigStatus(_ []string) {
	e.Query(e.API.HubFx.ConfigStatus(), e.ParseConfigStatus)
}

// ConfigSave sends CONFIG_SAVE to the hub for the connected controller.
func (e *Engine) ConfigSave(args []string) {
	path := ""
	if len(args) > 0 {
		path = args[0]
	}
	e.Ack(e.API.HubFx.ConfigSave(path), "Config save")
}

// ConfigValidate validates a local YAML config file against the controller's schema.
func (e *Engine) ConfigValidate(args []string) {
	if !e.RequireArgs(args, 1, "config.validate <file>") {
		return
	}
	filePath := args[0]

	data, err := os.ReadFile(filepath.Clean(filePath))
	if err != nil {
		e.Out.Error("Cannot read file: %s", err)
		return
	}

	// Determine which schema to use based on connected controller type
	schema := SchemaForController(e.ControllerType)
	if schema == nil {
		e.Out.Error("No config schema defined for controller type '%s'", e.ControllerType)
		return
	}

	errors := schema.Validate(string(data))
	if len(errors) == 0 {
		e.Out.OK("Config valid (%d bytes, schema: %s)", len(data), schema.Name)
	} else {
		e.Out.Error("Config validation failed (%d error(s)):", len(errors))
		for _, ve := range errors {
			e.Out.Printf("  %s  %s\n", e.Out.C(ColorRed, "•"), ve)
		}
	}
}

// ─── Shared Config Response Parser ───

// ParseConfigStatus parses and displays a CONFIG_STATUS_RESP payload.
// Payload: [loaded:u8][size:u16LE][valid:u8]
func (e *Engine) ParseConfigStatus(payload []byte) {
	if len(payload) < 4 {
		e.Out.Println("  (config status too short)")
		return
	}
	loaded := payload[0] != 0
	size := protocol.ReadU16LE(payload, 1)
	valid := payload[3] != 0

	e.Out.Printf("\n  %s\n", e.Out.C(ColorCyan, "Config Status"))
	statusStr := e.Out.C(ColorRed, "not loaded")
	if loaded {
		statusStr = e.Out.C(ColorGreen, "loaded")
	}
	e.Out.Printf("    Status:     %s\n", statusStr)
	e.Out.Printf("    Size:       %d bytes\n", size)
	if loaded {
		validStr := e.Out.C(ColorYellow, "invalid")
		if valid {
			validStr = e.Out.C(ColorGreen, "valid")
		}
		e.Out.Printf("    Validation: %s\n", validStr)
	}
	e.Out.Println("")
}

// ConfigCommands returns the standard config CmdEntry map for embedding in a handler's command group.
func (e *Engine) ConfigCommands() map[string]CmdEntry {
	return map[string]CmdEntry{
		"config.reload":   {e.ConfigReload, "config.reload [path]", "Reload config from storage", true},
		"config.status":   {e.ConfigStatus, "config.status", "Config load status", true},
		"config.save":     {e.ConfigSave, "config.save [path]", "Save config to storage", true},
		"config.validate": {e.ConfigValidate, "config.validate <file>", "Validate local config file", true},
	}
}
