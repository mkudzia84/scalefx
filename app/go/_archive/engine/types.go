package engine

// ScaleFX Engine - Types
// Shared type definitions for command registration and dispatch.

import "scalefx/protocol/core"

// CmdEntry defines a single CLI command. Name is the bare invocation form
// (e.g. `servo`, `seq.start`) — the prefix-less key that FlatCommands /
// PrintGroupHelp use; Usage is the full help-line argspec (`servo <ch> <pos>`).
type CmdEntry struct {
	Name        string
	Handler     func(args []string)
	Usage       string
	Description string
	RequireInit bool
}

// CmdGroup defines a group of related commands (one per controller type).
//
// Prefix forces every command in the group to be invoked as `<prefix>:<name>`
// (e.g. `light:servo`, `gear:reset`, `gun:trigger`, `hub:slaves`). This makes
// board-targeting unambiguous on hub connections where multiple slave types
// expose overlapping names (`servo`, `reset`, `enable`, `battery`, …) and is
// the canonical form everywhere — direct connections too, so muscle memory
// transfers when the same script later runs through a hub. Universal groups
// (Core, Firmware, Storage/Config) leave Prefix empty.
//
// Commands is an ordered slice so help output preserves the registration order
// when groups want it; PrintGroupHelp sorts by Name for stable display either
// way.
type CmdGroup struct {
	Name       string
	Controller string // empty = universal
	Prefix     string // empty = bare names; non-empty = "<prefix>:<cmd>"
	Color      Color
	Commands   []CmdEntry
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
	// Capabilities is the bitmask the firmware advertises in IDENTIFY/INIT_READY
	// (core.Cap* bits). 0 means "legacy firmware" — UI should fall back to
	// probing rather than assuming nothing is supported.
	Capabilities uint32
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
