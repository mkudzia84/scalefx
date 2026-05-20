package main

import (
	"scalefx/protocol/core"
	"scalefx/protocol/enginefx"
)

func init() {
	register(&command{Name: "engine-start", Usage: "engine-start", Help: "kick off engine startup sequence", Category: catEngine, RequiresConn: true, RequiresCap: core.CapEngine, Run: cmdEngineStart})
	register(&command{Name: "engine-stop", Usage: "engine-stop", Help: "kick off engine shutdown sequence", Category: catEngine, RequiresConn: true, RequiresCap: core.CapEngine, Run: cmdEngineStop})
	register(&command{Name: "engine-status", Usage: "engine-status", Help: "current engine state + RC toggle", Category: catEngine, RequiresConn: true, RequiresCap: core.CapEngine, Run: cmdEngineStatus})
}

func cmdEngineStart(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Engine.Start(); err != nil {
		return err
	}
	Ok("engine: %s", Phase("starting"))
	return nil
}

func cmdEngineStop(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Engine.Stop(); err != nil {
		return err
	}
	Ok("engine: %s", Phase("stopping"))
	return nil
}

func cmdEngineStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Engine.Status()
	if err != nil {
		return err
	}
	Hdr("Engine")
	KV("state", Phase(enginefx.StateName(s.State)))
	KV("active", Bool(s.Active))
	KV("RC toggle", Bool(s.ToggleEngaged))
	return nil
}
