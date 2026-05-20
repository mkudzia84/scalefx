package main

import (
	"fmt"
	"strconv"
	"strings"

	"scalefx/protocol/core"
)

func init() {
	register(&command{Name: "gun-fire", Usage: "gun-fire <id>", Help: "fire exactly one shot", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunFire})
	register(&command{Name: "gun-start", Usage: "gun-start <id> [rpm]", Help: "start auto-fire at <rpm> rounds/min (0 = default)", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunStart})
	register(&command{Name: "gun-stop", Usage: "gun-stop <id>", Help: "stop auto-fire", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunStop})
	register(&command{Name: "gun-smoke", Usage: "gun-smoke <id> <on|off>", Help: "arm/disarm the smoke heater", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunSmoke})
	register(&command{Name: "gun-status", Usage: "gun-status", Help: "per-gun firing + smoke state", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunStatus})
}

func cmdGunFire(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gun-fire <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gun.FireOnce(id); err != nil {
		return err
	}
	Ok("gun[%d] %s", id, cRed("fired"))
	return nil
}

func cmdGunStart(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 1 {
		return fmt.Errorf("usage: gun-start <id> [rpm]")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	rpm := uint16(0)
	if len(args) >= 2 {
		v, e := strconv.ParseUint(args[1], 0, 16)
		if e != nil {
			return fmt.Errorf("rpm: %w", e)
		}
		rpm = uint16(v)
	}
	if err := a.c.Gun.StartFiring(id, rpm); err != nil {
		return err
	}
	if rpm == 0 {
		Ok("gun[%d] auto-fire %s (default cadence)", id, Phase("firing"))
	} else {
		Ok("gun[%d] auto-fire %s @ %d rpm", id, Phase("firing"), rpm)
	}
	return nil
}

func cmdGunStop(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gun-stop <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gun.StopFiring(id); err != nil {
		return err
	}
	Ok("gun[%d] auto-fire %s", id, Phase("off"))
	return nil
}

func cmdGunSmoke(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 2 {
		return fmt.Errorf("usage: gun-smoke <id> <on|off>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	var armed byte
	switch strings.ToLower(args[1]) {
	case "on", "1", "arm":
		armed = 1
	case "off", "0", "disarm":
		armed = 0
	default:
		return fmt.Errorf("expected on|off, got %q", args[1])
	}
	if err := a.c.Gun.SmokeArm(id, armed); err != nil {
		return err
	}
	if armed != 0 {
		Ok("gun[%d] smoke heater %s", id, Phase("armed"))
	} else {
		Ok("gun[%d] smoke heater %s", id, Phase("off"))
	}
	return nil
}

func cmdGunStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	st, err := a.c.Gun.Status()
	if err != nil {
		return err
	}
	if len(st) == 0 {
		Note("(no guns configured)")
		return nil
	}
	Hdr("Guns")
	for _, g := range st {
		fire := Phase("idle")
		if g.Firing {
			fire = Phase("firing")
		}
		smoke := Phase("off")
		if g.SmokeArmed {
			smoke = Phase("armed")
		}
		fmt.Printf("  %s  state=%s  smoke=%s\n",
			cBold(cMagenta(fmt.Sprintf("[%d]", g.ID))),
			fire, smoke)
	}
	return nil
}
