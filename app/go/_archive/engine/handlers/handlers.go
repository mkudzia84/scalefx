package handlers

// ScaleFX Engine - Built-in Handler Registration
// RegisterDefaults wires all board handlers onto a shared engine and returns
// a Registry so external consumers (Studio, CLI, tests) can install typed
// listeners exposed by each board's Handler.

import (
	"scalefx/engine"
	"scalefx/engine/handlers/core"
	"scalefx/engine/handlers/gearcontrol"
	"scalefx/engine/handlers/gunfx"
	"scalefx/engine/handlers/hubfx"
	"scalefx/engine/handlers/lightfx"
)

// Registry exposes each board's handler so external code can attach typed
// listeners (e.g. Studio sets GearControl.OnStatusBroadcast to emit a Wails
// event). CLI can ignore this return value.
type Registry struct {
	Core        *core.Handler
	GunFX       *gunfx.Handler
	GearControl *gearcontrol.Handler
	LightFX     *lightfx.Handler
	HubFX       *hubfx.Handler
}

// RegisterDefaults registers all built-in command groups (core + controllers).
// Called by both CLI and GUI before first Dispatch. Returns a Registry so the
// GUI can wire per-board event listeners; CLI can ignore the return value.
func RegisterDefaults(eng *engine.Engine) *Registry {
	return &Registry{
		Core:        core.Register(eng),
		GunFX:       gunfx.Register(eng),
		GearControl: gearcontrol.Register(eng),
		LightFX:     lightfx.Register(eng),
		HubFX:       hubfx.Register(eng),
	}
}
