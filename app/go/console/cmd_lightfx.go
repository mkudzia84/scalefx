package console

import (
	"fmt"
	"strconv"
	"strings"

	"scalefx/protocol/core"
)

func init() {
	register(&command{Name: "light-programs", Usage: "light-programs", Help: "list LightFX programs registered on the hub", Category: catLightFx, RequiresConn: true, RequiresCap: core.CapLightFx, Run: cmdLightPrograms})
	register(&command{Name: "light-status", Usage: "light-status", Help: "active LightFX program + master brightness", Category: catLightFx, RequiresConn: true, RequiresCap: core.CapLightFx, Run: cmdLightStatus})
	register(&command{Name: "light-select", Usage: "light-select <idx|name>", Help: "switch to a LightFX program (idx or name)", Category: catLightFx, RequiresConn: true, RequiresCap: core.CapLightFx, Run: cmdLightSelect})
	register(&command{Name: "light-reset", Usage: "light-reset", Help: "drop the active LightFX program; LEDs off", Category: catLightFx, RequiresConn: true, RequiresCap: core.CapLightFx, Run: cmdLightReset})
	register(&command{Name: "light-brightness", Usage: "light-brightness <0-100>", Help: "set LightFX master brightness percent", Category: catLightFx, RequiresConn: true, RequiresCap: core.CapLightFx, Run: cmdLightBrightness})
}

func cmdLightPrograms(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	progs, err := a.c.LightFx.Programs()
	if err != nil {
		return err
	}
	if len(progs) == 0 {
		Note("(no LightFX programs registered)")
		return nil
	}
	Hdr(fmt.Sprintf("LightFX programs (%d)", len(progs)))
	for _, p := range progs {
		fmt.Fprintf(out, "  %s  %s\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", p.Index))),
			p.Name)
	}
	return nil
}

func cmdLightStatus(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.LightFx.Status()
	if err != nil {
		return err
	}
	Hdr("LightFX")
	if s.HasActiveProgram() {
		name := ""
		if progs, err := a.c.LightFx.Programs(); err == nil {
			for _, pg := range progs {
				if pg.Index == s.ActiveIdx {
					name = pg.Name
					break
				}
			}
		}
		if name != "" {
			KV("program", fmt.Sprintf("%s %s",
				cMagenta(fmt.Sprintf("[%d]", s.ActiveIdx)),
				cBold(name)))
		} else {
			KV("program", cMagenta(fmt.Sprintf("[%d]", s.ActiveIdx)))
		}
	} else {
		KV("program", cDim("(none)"))
	}
	KVf("brightness", "%d%%", s.MasterBrightnessPct)
	return nil
}

func cmdLightSelect(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: light-select <programIdx | name>")
	}
	idx, err := resolveLightProgram(a, args[0])
	if err != nil {
		return err
	}
	if err := a.c.LightFx.SelectProgram(idx); err != nil {
		return err
	}
	Ok("program %s selected", cMagenta(fmt.Sprintf("[%d]", idx)))
	return nil
}

func cmdLightReset(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.LightFx.ResetProgram(); err != nil {
		return err
	}
	Ok("active program cleared; every claimed LED off")
	return nil
}

func cmdLightBrightness(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: light-brightness <pct 0..100>")
	}
	pct, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.LightFx.SetMasterBrightness(pct); err != nil {
		return err
	}
	Ok("master brightness → %d%%", pct)
	return nil
}

// resolveLightProgram lets the user pass either a numeric index or a
// program name; for the name path, we resolve through ProgramListReq.
func resolveLightProgram(a *App, s string) (byte, error) {
	if v, err := strconv.ParseUint(strings.TrimPrefix(s, "0x"), 0, 8); err == nil {
		return byte(v), nil
	}
	progs, err := a.c.LightFx.Programs()
	if err != nil {
		return 0, fmt.Errorf("look up program by name: %w", err)
	}
	for _, p := range progs {
		if strings.EqualFold(p.Name, s) {
			return p.Index, nil
		}
	}
	return 0, fmt.Errorf("unknown LightFX program: %q", s)
}
