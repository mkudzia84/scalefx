package console

// Central command registry.  Each `cmd_*.go` file owns one wire-domain
// (storage, audio, topology, gear, …) and self-registers its commands
// in `init()` — there is no master switch statement.  Same idea as
// `engine/handlers/<mod>/handler.go::Register(eng)` in the archived
// GUI engine, just leaner.

import (
	"fmt"
	"sort"
	"strings"
)

// category groups commands in the help screen.  The order below is also
// the print order (most-used → least-used, connection-agnostic → connected).
type category string

const (
	catSession   category = "Session"        // help, ports, connect, disconnect, verbose, quit
	catCore      category = "Core"           // identify, status, capabilities, init, reboot, …
	catStorage   category = "Storage"        // sd-*, flash-*, files, tree, mkdir, upload, …
	catAudio     category = "Audio"          // play, volume, queue, fade, audio-status, …
	catTopology  category = "Topology"       // expanders, topo-*, role-*
	catLightFx   category = "LightFX"        // light-*
	catLanding   category = "Landing lights" // landing-*
	catGear      category = "Gear"           // gear-*
	catEngine    category = "Engine"         // engine-*
	catGun       category = "Gun"            // gun-*
	catAlert     category = "Alerts"         // alert*
	catConfig    category = "Config"         // config-reload, config-status, config-save
	catEvents    category = "Events"         // subscribe
)

var categoryOrder = []category{
	catSession, catCore, catStorage, catAudio, catTopology,
	catLightFx, catLanding, catGear, catEngine, catGun, catAlert,
	catConfig, catEvents,
}

// command is one registered CLI verb.
type command struct {
	Name         string
	Usage        string
	Help         string
	Run          func(a *App, args []string) error
	Category     category
	RequiresConn bool
	// RequiresCap is a bitmask of CoreCapability flags; the command is
	// shown in `help` only when the connected board advertises AT LEAST
	// ONE of the bits.  0 means "no capability gating".  Filtering is a
	// UX layer — the wire dispatch always succeeds or NACKs cleanly.
	RequiresCap uint32
}

// Global registries — populated from each cmd_*.go `init()`.
var (
	commands = map[string]*command{}
	aliases  = map[string]string{}
)

// register adds a command to the central table.  Panics on duplicate
// name so the package init order is loud rather than silently shadowing.
func register(c *command) {
	if _, dup := commands[c.Name]; dup {
		panic(fmt.Sprintf("cli: duplicate command %q", c.Name))
	}
	commands[c.Name] = c
}

// aliasFor declares a short form (`ls` → `files`).  Panics if the
// target doesn't exist yet (sequence in init()s matters), so misspelled
// aliases surface at boot rather than at first use.
func aliasFor(short, target string) {
	if _, dup := aliases[short]; dup {
		panic(fmt.Sprintf("cli: duplicate alias %q", short))
	}
	aliases[short] = target
}

// lookup resolves a name through the alias table.  Returns (cmd, true)
// on hit; (nil, false) otherwise.
func lookup(name string) (*command, bool) {
	if c, ok := commands[name]; ok {
		return c, true
	}
	if target, ok := aliases[name]; ok {
		return commands[target], true
	}
	return nil, false
}

// availableCommands returns the commands the user can currently invoke,
// grouped by category.  When `connected` is false, only commands with
// RequiresConn==false (Session) survive.  When connected, commands are
// gated by `RequiresCap` against `enabledCaps` — the runtime-enabled
// subset of the firmware's compiled-in capability mask.  Pass 0 for
// both `compiledCaps` and `enabledCaps` for pre-IDENTIFY firmware
// (legacy boards); every command then passes the filter.
//
// Commands whose backing feature is COMPILED-IN BUT RUNTIME-DISABLED
// don't appear in this map — they're surfaced separately by
// `disabledCommands` so the CLI can show "currently disabled" verbs
// in a dim/separate section.
func availableCommands(connected bool, enabledCaps uint32) map[category][]*command {
	out := map[category][]*command{}
	for _, c := range commands {
		if c.RequiresConn && !connected {
			continue
		}
		if connected && c.RequiresCap != 0 && (enabledCaps&c.RequiresCap) == 0 {
			continue
		}
		out[c.Category] = append(out[c.Category], c)
	}
	for _, slice := range out {
		sort.Slice(slice, func(i, j int) bool { return slice[i].Name < slice[j].Name })
	}
	return out
}

// disabledCommands returns commands whose `RequiresCap` is set in
// `compiledCaps` (firmware was built with the feature) but NOT in
// `enabledCaps` (config has it turned off) — so the user knows the
// command would work if they re-enabled the feature.
func disabledCommands(connected bool, compiledCaps, enabledCaps uint32) map[category][]*command {
	out := map[category][]*command{}
	if !connected {
		return out
	}
	disabledMask := compiledCaps & ^enabledCaps
	if disabledMask == 0 {
		return out
	}
	for _, c := range commands {
		if !c.RequiresConn || c.RequiresCap == 0 {
			continue
		}
		if (disabledMask & c.RequiresCap) == 0 {
			continue
		}
		out[c.Category] = append(out[c.Category], c)
	}
	for _, slice := range out {
		sort.Slice(slice, func(i, j int) bool { return slice[i].Name < slice[j].Name })
	}
	return out
}

// joinCSV is a small helper used by `cmd_session.go` to render alias
// lists in the help footer.
func joinCSV(parts []string) string { return strings.Join(parts, ", ") }
