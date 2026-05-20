package main

import (
	"fmt"

	"scalefx/protocol/core"
	"scalefx/protocol/landing"
)

func init() {
	register(&command{Name: "landing-list", Usage: "landing-list", Help: "list configured landing lights (owner + phase)", Category: catLanding, RequiresConn: true, RequiresCap: core.CapLandingLights, Run: cmdLandingList})
	register(&command{Name: "landing-status", Usage: "landing-status", Help: "per-light lifecycle phases", Category: catLanding, RequiresConn: true, RequiresCap: core.CapLandingLights, Run: cmdLandingStatus})
	register(&command{Name: "landing-on", Usage: "landing-on <id>", Help: "deploy + power on a landing light", Category: catLanding, RequiresConn: true, RequiresCap: core.CapLandingLights, Run: cmdLandingOn})
	register(&command{Name: "landing-off", Usage: "landing-off <id>", Help: "power off + retract a landing light", Category: catLanding, RequiresConn: true, RequiresCap: core.CapLandingLights, Run: cmdLandingOff})
}

func cmdLandingList(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	lst, err := a.c.LandingLights.List()
	if err != nil {
		return err
	}
	if len(lst) == 0 {
		Note("(no landing lights configured)")
		return nil
	}
	Hdr(fmt.Sprintf("Landing lights (%d)", len(lst)))
	for _, l := range lst {
		owner := cDim("(none)")
		if l.Owner != 0 {
			owner = cCyan(effectIdName(l.Owner))
		}
		fmt.Printf("  %s  %s  phase=%s  owner=%s\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", l.ID))),
			padRight(l.Name, 20),
			Phase(landing.PhaseName(l.Phase)),
			owner)
	}
	return nil
}

func cmdLandingStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	st, err := a.c.LandingLights.Status()
	if err != nil {
		return err
	}
	if len(st) == 0 {
		Note("(no landing lights configured)")
		return nil
	}
	Hdr("Landing lights")
	for _, e := range st {
		fmt.Printf("  %s  %s\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", e.ID))),
			Phase(landing.PhaseName(e.Phase)))
	}
	return nil
}

func cmdLandingOn(a *app, args []string) error  { return cmdLandingSet(a, args, landing.StateOn) }
func cmdLandingOff(a *app, args []string) error { return cmdLandingSet(a, args, landing.StateOff) }

func cmdLandingSet(a *app, args []string, state byte) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: landing-%s <id>", landing.StateName(state))
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.LandingLights.SetState(id, state); err != nil {
		return err
	}
	Ok("landing[%d] → %s", id, Phase(landing.StateName(state)))
	return nil
}

// effectIdName matches `effects/effect_id.h` — keep in sync if new
// EffectIds get added on the firmware side.
func effectIdName(id byte) string {
	switch id {
	case 0:
		return "none"
	case 1:
		return "LightFx"
	case 2:
		return "GunFx"
	case 3:
		return "GearCtrl"
	case 4:
		return "LandingLight"
	case 5:
		return "EngineFx"
	case 6:
		return "Alert"
	default:
		return fmt.Sprintf("0x%02X?", id)
	}
}
