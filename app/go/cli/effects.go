package main

import (
	"fmt"
	"strconv"
	"strings"

	"scalefx/client"
	"scalefx/protocol/alerts"
	"scalefx/protocol/enginefx"
	"scalefx/protocol/gear"
	"scalefx/protocol/landing"
)

// ─── LightFX ─────────────────────────────────────────────────────────

func cmdLightPrograms(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	progs, err := a.c.LightFx.Programs()
	if err != nil {
		return err
	}
	if len(progs) == 0 {
		fmt.Println("(no LightFX programs registered)")
		return nil
	}
	fmt.Printf("LightFX programs (%d):\n", len(progs))
	for _, p := range progs {
		fmt.Printf("  [%2d] %s\n", p.Index, p.Name)
	}
	return nil
}

func cmdLightStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.LightFx.Status()
	if err != nil {
		return err
	}
	if s.HasActiveProgram() {
		// Best-effort: enrich with the program name if available.
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
			fmt.Printf("  Active program  : [%d] %s\n", s.ActiveIdx, name)
		} else {
			fmt.Printf("  Active program  : [%d]\n", s.ActiveIdx)
		}
	} else {
		fmt.Println("  Active program  : (none)")
	}
	fmt.Printf("  Master brightness: %d%%\n", s.MasterBrightnessPct)
	return nil
}

func cmdLightSelect(a *app, args []string) error {
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
	fmt.Printf("  selected program [%d]\n", idx)
	return nil
}

func cmdLightReset(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.LightFx.ResetProgram(); err != nil {
		return err
	}
	fmt.Println("  active program cleared; every claimed LED off.")
	return nil
}

func cmdLightBrightness(a *app, args []string) error {
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
	fmt.Printf("  master brightness → %d%%\n", pct)
	return nil
}

// resolveLightProgram lets the user pass either a numeric index or a
// program name; for the name path, we resolve through ProgramListReq.
func resolveLightProgram(a *app, s string) (byte, error) {
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

// ─── Landing Lights ──────────────────────────────────────────────────

func cmdLandingList(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	lst, err := a.c.LandingLights.List()
	if err != nil {
		return err
	}
	if len(lst) == 0 {
		fmt.Println("(no landing lights configured)")
		return nil
	}
	fmt.Printf("Landing lights (%d):\n", len(lst))
	for _, l := range lst {
		owner := "(none)"
		if l.Owner != 0 {
			owner = effectIdName(l.Owner)
		}
		fmt.Printf("  [%2d] %-20s  phase=%-12s  owner=%s\n",
			l.ID, l.Name, landing.PhaseName(l.Phase), owner)
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
		fmt.Println("(no landing lights configured)")
		return nil
	}
	for _, e := range st {
		fmt.Printf("  [%2d] %s\n", e.ID, landing.PhaseName(e.Phase))
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
	fmt.Printf("  landing[%d] → %s\n", id, landing.StateName(state))
	return nil
}

// ─── Gear ────────────────────────────────────────────────────────────

func cmdGearList(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	lst, err := a.c.Gear.List()
	if err != nil {
		return err
	}
	if len(lst) == 0 {
		fmt.Println("(no gear units configured)")
		return nil
	}
	fmt.Printf("Gear units (%d):\n", len(lst))
	for _, g := range lst {
		fmt.Printf("  [%2d] %s\n", g.ID, g.Name)
	}
	return nil
}

func cmdGearStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	st, err := a.c.Gear.Status()
	if err != nil {
		return err
	}
	if len(st) == 0 {
		fmt.Println("(no gear units configured)")
		return nil
	}
	for _, e := range st {
		fmt.Printf("  [%2d] %s\n", e.ID, gear.PhaseName(e.Phase))
	}
	return nil
}

func cmdGearDeploy(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-deploy <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gear.Deploy(id); err != nil {
		return err
	}
	fmt.Printf("  gear[%d] deploying...\n", id)
	return nil
}

func cmdGearRetract(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-retract <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gear.Retract(id); err != nil {
		return err
	}
	fmt.Printf("  gear[%d] retracting...\n", id)
	return nil
}

func cmdGearStop(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-stop <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gear.Stop(id); err != nil {
		return err
	}
	fmt.Printf("  gear[%d] stopped (any error state cleared).\n", id)
	return nil
}

func cmdGearAll(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-all <stop|deploy|retract>")
	}
	var action byte
	switch strings.ToLower(args[0]) {
	case "stop":
		action = client.GearAllStop
	case "deploy":
		action = client.GearAllDeploy
	case "retract":
		action = client.GearAllRetract
	default:
		return fmt.Errorf("action must be stop|deploy|retract")
	}
	if err := a.c.Gear.All(action); err != nil {
		return err
	}
	fmt.Printf("  all gear → %s\n", gear.AllActionName(action))
	return nil
}

// ─── EngineFX ────────────────────────────────────────────────────────

func cmdEngineStart(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Engine.Start(); err != nil {
		return err
	}
	fmt.Println("  engine: startup sequence kicked off.")
	return nil
}

func cmdEngineStop(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Engine.Stop(); err != nil {
		return err
	}
	fmt.Println("  engine: shutdown sequence kicked off.")
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
	fmt.Printf("  State          : %s\n", enginefx.StateName(s.State))
	fmt.Printf("  Active         : %v\n", s.Active)
	fmt.Printf("  RC toggle held : %v\n", s.ToggleEngaged)
	return nil
}

// ─── GunFX ───────────────────────────────────────────────────────────

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
	fmt.Printf("  gun[%d] fired.\n", id)
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
		fmt.Printf("  gun[%d] auto-fire ON (default cadence).\n", id)
	} else {
		fmt.Printf("  gun[%d] auto-fire ON @ %d rpm.\n", id, rpm)
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
	fmt.Printf("  gun[%d] auto-fire OFF.\n", id)
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
	state := "off"
	if armed != 0 {
		state = "ON"
	}
	fmt.Printf("  gun[%d] smoke heater %s.\n", id, state)
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
		fmt.Println("(no guns configured)")
		return nil
	}
	for _, g := range st {
		smoke := "off"
		if g.SmokeArmed {
			smoke = "ARMED"
		}
		fire := "idle"
		if g.Firing {
			fire = "FIRING"
		}
		fmt.Printf("  gun[%d]  state=%-7s  smoke=%s\n", g.ID, fire, smoke)
	}
	return nil
}

// ─── Alerts ──────────────────────────────────────────────────────────

func cmdAlert(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 1 {
		return fmt.Errorf("usage: alert <info|warning|error|critical> [outputMask]")
	}
	sev, ok := alerts.SeverityFromName(strings.ToLower(args[0]))
	if !ok {
		v, e := parseU8(args[0])
		if e != nil {
			return fmt.Errorf("unknown severity: %q", args[0])
		}
		sev = v
	}
	mask := byte(0) // 0 == use severity preset
	if len(args) >= 2 {
		v, e := parseU8(args[1])
		if e != nil {
			return e
		}
		mask = v
	}
	if err := a.c.Alerts.Beep(sev, mask); err != nil {
		return err
	}
	fmt.Printf("  alert: %s\n", alerts.SeverityName(sev))
	return nil
}

func cmdAlertStop(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Alerts.Stop(); err != nil {
		return err
	}
	fmt.Println("  alert channel silenced.")
	return nil
}

func cmdAlertStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Alerts.Status()
	if err != nil {
		return err
	}
	playing := "idle"
	if s.Playing {
		playing = "PLAYING"
	}
	last := "(never)"
	if s.LastSeverity != 0xFF {
		last = alerts.SeverityName(s.LastSeverity)
	}
	fmt.Printf("  Channel      : %s\n", playing)
	fmt.Printf("  Last severity: %s\n", last)
	return nil
}

// ─── Helpers ─────────────────────────────────────────────────────────

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

