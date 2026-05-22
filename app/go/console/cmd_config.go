package console

import (
	"fmt"

	"scalefx/protocol/core"
)

func init() {
	register(&command{Name: "config-reload", Usage: "config-reload [path]", Help: "re-read /hubfx.yaml (and friends); fan apply to every service", Category: catConfig, RequiresConn: true, RequiresCap: core.CapConfig, Run: cmdConfigReload})
	register(&command{Name: "config-status", Usage: "config-status", Help: "aggregate config-store state (loaded, file size, validate ok)", Category: catConfig, RequiresConn: true, RequiresCap: core.CapConfig, Run: cmdConfigStatus})
	register(&command{Name: "config-save", Usage: "config-save <path>", Help: "save the in-memory config to file (atomic temp+rename)", Category: catConfig, RequiresConn: true, RequiresCap: core.CapConfig, Run: cmdConfigSave})
	aliasFor("reload", "config-reload")
}

func cmdConfigReload(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) >= 1 {
		if err := a.c.Config.ReloadPath(args[0]); err != nil {
			return err
		}
		Ok("reloaded %s", cMagenta(args[0]))
		return nil
	}
	if err := a.c.Config.Reload(); err != nil {
		return err
	}
	Ok("config reloaded — every store re-read + re-applied")
	return nil
}

func cmdConfigStatus(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	st, err := a.c.Config.Status()
	if err != nil {
		return err
	}
	Hdr("config status")
	if st.Loaded {
		KV("loaded", cGreen("yes"))
	} else {
		KV("loaded", cRed("no — file missing or parse failed"))
	}
	KV("file size", humanBytes(uint64(st.FileSize)))
	if st.ValidOk {
		KV("validate", cGreen("ok"))
	} else {
		KV("validate", cRed("FAILED"))
	}
	return nil
}

func cmdConfigSave(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 1 {
		return fmt.Errorf("usage: config-save <path>")
	}
	if err := a.c.Config.SavePath(args[0]); err != nil {
		return err
	}
	Ok("saved %s", cMagenta(args[0]))
	return nil
}
