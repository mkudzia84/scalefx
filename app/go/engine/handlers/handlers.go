package handlers

// RegisterDefaults registers all built-in command groups (core + controllers).
// Called by both CLI and GUI before first Dispatch.

import (
	"scalefx/engine"
	"scalefx/engine/handlers/core"
	"scalefx/engine/handlers/gearcontrol"
	"scalefx/engine/handlers/gunfx"
	"scalefx/engine/handlers/hubfx"
	"scalefx/engine/handlers/lightfx"
)

func RegisterDefaults(eng *engine.Engine) {
	core.Register(eng)
	gunfx.Register(eng)
	gearcontrol.Register(eng)
	lightfx.Register(eng)
	hubfx.Register(eng)
}
