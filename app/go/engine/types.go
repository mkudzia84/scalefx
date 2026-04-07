package engine

// ScaleFX Engine - Types
// Shared type definitions for command registration and dispatch.

import "scalefx/protocol/core"

// CmdEntry defines a single CLI command.
type CmdEntry struct {
	Handler     func(args []string)
	Usage       string
	Description string
	RequireInit bool
}

// CmdGroup defines a group of related commands (one per controller type).
type CmdGroup struct {
	Name       string
	Controller string // empty = universal
	Color      Color
	Commands   map[string]CmdEntry
}

// flatEntry is used internally for dispatch — a CmdEntry plus its controller filter.
type flatEntry struct {
	CmdEntry
	controller string
}

// InitReadyInfo holds parsed INIT_READY/IDENTIFY data.
type InitReadyInfo struct {
	Name           string
	Version        string
	Platform       string
	CPUMHz         uint32
	FreeRAM        uint32
	Build          uint32
	ControllerType string
}

// ControllerColors maps controller types to logical colors.
var ControllerColors = map[string]Color{
	core.CtrlGearControl: ColorGreen,
	core.CtrlGunFX:       ColorRed,
	core.CtrlHubFX:       ColorCyan,
	core.CtrlLightFX:     ColorBlue,
	core.CtrlNoOp:        ColorMagenta,
}

// ControllerLabels maps controller types to display names.
var ControllerLabels = map[string]string{
	core.CtrlGearControl: "GearControl",
	core.CtrlGunFX:       "GunFX",
	core.CtrlHubFX:       "HubFX",
	core.CtrlLightFX:     "LightFX",
	core.CtrlNoOp:        "NoOp",
}
