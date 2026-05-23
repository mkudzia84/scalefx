package console

import (
	"fmt"
	"strconv"
	"strings"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/protocol/gunfx"
)

func init() {
	register(&command{Name: "gun-fire", Usage: "gun-fire <id>", Help: "fire exactly one shot", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunFire})
	register(&command{Name: "gun-start", Usage: "gun-start <id> [rpm]", Help: "start auto-fire at <rpm> rounds/min (0 = default)", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunStart})
	register(&command{Name: "gun-stop", Usage: "gun-stop <id>", Help: "stop auto-fire", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunStop})
	register(&command{Name: "gun-smoke", Usage: "gun-smoke <id> <on|off>", Help: "arm/disarm the smoke heater", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunSmoke})
	register(&command{Name: "gun-status", Usage: "gun-status", Help: "per-gun firing + smoke state", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunStatus})
	register(&command{Name: "gun-manual", Usage: "gun-manual <id> <key=val> ...", Help: "puppet-mode override (yaw_us, pitch_us, rof, fire, smoke, fan_burst)", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunManual})
	register(&command{Name: "gun-manual-release", Usage: "gun-manual-release <id>", Help: "exit manual override; RC takes over on next tick", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunManualRelease})
	register(&command{Name: "gun-verbose", Usage: "gun-verbose <id> <on|off>", Help: "subscribe/unsubscribe ~10 Hz GUN_VERBOSE_STATUS broadcasts", Category: catGun, RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunVerbose})
}

func cmdGunFire(a *App, args []string) error {
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

func cmdGunStart(a *App, args []string) error {
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

func cmdGunStop(a *App, args []string) error {
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

func cmdGunSmoke(a *App, args []string) error {
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

// ─── Manual override (puppet mode) ──────────────────────────────────
//
// `gun-manual <id> <key=val> ...` lets the operator drive any subset
// of a gun's subsystems directly from the CLI — same wire surface
// Studio's "simulate" panel will use in Phase 4d.  Keys:
//
//   yaw_us=1500           — set yaw to absolute µs
//   pitch_us=1500         — set pitch to absolute µs
//   rof=2                 — arm ROF item index (or 255 for none)
//   fire=on|off           — hold/release trigger
//   smoke=on|off          — arm/disarm heater
//   fan_burst=on          — one-shot fan puff
//
// Only keys that appear in the command set the corresponding flag bit;
// the firmware leaves the other subsystems at their prior manual
// values (or RC-driven for fields never manually set since the last
// `gun-manual-release`).  Auto-released after 5 s without another
// gun-manual call so a CLI crash never strands the gun in puppet mode.
func cmdGunManual(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: gun-manual <id> <key=val> ...")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	kv, err := parseKeyVals(args[1:])
	if err != nil {
		return err
	}

	var st client.GunManualState
	if s, ok := kv["yaw_us"]; ok {
		v, e := strconv.ParseUint(s, 0, 16)
		if e != nil {
			return fmt.Errorf("yaw_us: %w", e)
		}
		st.Flags |= gunfx.ManualFlagYaw
		st.YawUs = uint16(v)
	}
	if s, ok := kv["pitch_us"]; ok {
		v, e := strconv.ParseUint(s, 0, 16)
		if e != nil {
			return fmt.Errorf("pitch_us: %w", e)
		}
		st.Flags |= gunfx.ManualFlagPitch
		st.PitchUs = uint16(v)
	}
	if s, ok := kv["rof"]; ok {
		v, e := strconv.ParseUint(s, 0, 8)
		if e != nil {
			return fmt.Errorf("rof: %w", e)
		}
		st.Flags |= gunfx.ManualFlagRof
		st.RofIndex = uint8(v)
	}
	if s, ok := kv["fire"]; ok {
		b, e := parseOnOff(s)
		if e != nil {
			return fmt.Errorf("fire: %w", e)
		}
		st.Flags |= gunfx.ManualFlagFire
		if b {
			st.FireHold = 1
		}
	}
	if s, ok := kv["smoke"]; ok {
		b, e := parseOnOff(s)
		if e != nil {
			return fmt.Errorf("smoke: %w", e)
		}
		st.Flags |= gunfx.ManualFlagSmoke
		if b {
			st.SmokeArm = 1
		}
	}
	if s, ok := kv["fan_burst"]; ok {
		b, e := parseOnOff(s)
		if e != nil {
			return fmt.Errorf("fan_burst: %w", e)
		}
		st.Flags |= gunfx.ManualFlagFanBurst
		if b {
			st.SmokeFanBurst = 1
		}
	}
	if st.Flags == 0 {
		return fmt.Errorf("no manual fields set — pass at least one key=val")
	}
	if err := a.c.Gun.ManualSet(id, st); err != nil {
		return err
	}
	Ok("gun[%d] manual override applied (flags=0x%02X)", id, st.Flags)
	return nil
}

func cmdGunManualRelease(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gun-manual-release <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gun.ManualRelease(id); err != nil {
		return err
	}
	Ok("gun[%d] manual %s — RC takes over", id, Phase("released"))
	return nil
}

func cmdGunVerbose(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 2 {
		return fmt.Errorf("usage: gun-verbose <id> <on|off>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	on, err := parseOnOff(args[1])
	if err != nil {
		return err
	}
	enable := byte(0)
	if on {
		enable = 1
	}
	if err := a.c.Gun.VerboseStatusSubscribe(id, enable); err != nil {
		return err
	}
	if on {
		Ok("gun[%d] verbose status %s @ ~10 Hz", id, Phase("subscribed"))
		Note("  subscribe to the gun:verbose event stream to see them")
	} else {
		Ok("gun[%d] verbose status %s", id, Phase("unsubscribed"))
	}
	return nil
}

// parseOnOff accepts a small set of yes/no tokens — shared with smoke
// and verbose toggles.
func parseOnOff(s string) (bool, error) {
	switch strings.ToLower(s) {
	case "on", "1", "yes", "arm", "true":
		return true, nil
	case "off", "0", "no", "disarm", "false":
		return false, nil
	}
	return false, fmt.Errorf("expected on|off, got %q", s)
}

func cmdGunStatus(a *App, _ []string) error {
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
		fmt.Fprintf(out, "  %s  state=%s  smoke=%s\n",
			cBold(cMagenta(fmt.Sprintf("[%d]", g.ID))),
			fire, smoke)
	}
	return nil
}
