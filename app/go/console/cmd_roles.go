package console

// Role-layer live-tune CLI surface (Phase 2.9.x / Phase 3 of GunFX
// rollout, instructions/22).  Lets operators read + push servo motion
// profiles and DC-motor / heater element scaling without re-attaching
// the role (which would lose target / position state).
//
//   servo-profile-get <portIdx>
//   servo-profile-set <portIdx> <key=val> ...
//   motor-element-get <portIdx>
//   motor-element-set <portIdx> <key=val> ...
//   heater-element-get <portIdx>
//   heater-element-set <portIdx> <key=val> ...
//
// Set commands use a key=val syntax so partial updates are ergonomic at
// the terminal (`servo-profile-set 0 max_speed=400` only touches that
// one field; everything else is pulled from a fresh GET first).

import (
	"fmt"
	"strconv"
	"strings"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/protocol/roles"
)

func init() {
	register(&command{Name: "servo-profiles", Usage: "servo-profiles", Help: "dump the LIVE motion profile of every attached hub servo (one row each) — the fast way to spot a stale/clamped calibration", Category: catTopology, RequiresConn: true, Run: cmdServoProfilesAll})
	register(&command{Name: "servo-profile-get", Usage: "servo-profile-get <portIdx>", Help: "read the motion profile of a servo on the hub (Phase 2.9.x)", Category: catTopology, RequiresConn: true, Run: cmdServoProfileGet})
	register(&command{Name: "servo-profile-set", Usage: "servo-profile-set <portIdx> <key=val> ...", Help: "push a servo motion profile (min/max/center/max_speed/max_accel/max_jerk/reversed)", Category: catTopology, RequiresConn: true, Run: cmdServoProfileSet})
	register(&command{Name: "motor-element-get", Usage: "motor-element-get <portIdx>", Help: "read DC motor element scaling config", Category: catTopology, RequiresConn: true, Run: cmdMotorElementGet})
	register(&command{Name: "motor-element-set", Usage: "motor-element-set <portIdx> <key=val> ...", Help: "push DC motor element_mv / scaling (passthrough|linear|quadratic)", Category: catTopology, RequiresConn: true, Run: cmdMotorElementSet})
	register(&command{Name: "heater-element-get", Usage: "heater-element-get <portIdx>", Help: "read heater element scaling + drive percent + hysteresis", Category: catTopology, RequiresConn: true, Run: cmdHeaterElementGet})
	register(&command{Name: "heater-element-set", Usage: "heater-element-set <portIdx> <key=val> ...", Help: "push heater element_mv / scaling / drive_pct / hyst_cx10", Category: catTopology, RequiresConn: true, Run: cmdHeaterElementSet})
	_ = core.CapTopology // keep the import live; commands are role-layer, gating handled at attach time
}

// ─── parsing helpers ────────────────────────────────────────────────

// parseKeyVals splits `k=v` args into a map.  Returns an error if any
// arg doesn't contain `=`.
func parseKeyVals(args []string) (map[string]string, error) {
	out := map[string]string{}
	for _, a := range args {
		eq := strings.IndexByte(a, '=')
		if eq <= 0 {
			return nil, fmt.Errorf("expected key=val, got %q", a)
		}
		out[strings.ToLower(a[:eq])] = a[eq+1:]
	}
	return out, nil
}

func u16From(m map[string]string, key string, def uint16) (uint16, error) {
	s, ok := m[key]
	if !ok {
		return def, nil
	}
	v, err := strconv.ParseUint(s, 0, 16)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", key, err)
	}
	return uint16(v), nil
}

func u8From(m map[string]string, key string, def uint8) (uint8, error) {
	s, ok := m[key]
	if !ok {
		return def, nil
	}
	v, err := strconv.ParseUint(s, 0, 8)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", key, err)
	}
	return uint8(v), nil
}

func i16From(m map[string]string, key string, def int16) (int16, error) {
	s, ok := m[key]
	if !ok {
		return def, nil
	}
	v, err := strconv.ParseInt(s, 0, 16)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", key, err)
	}
	return int16(v), nil
}

func boolFrom(m map[string]string, key string, def bool) (bool, error) {
	s, ok := m[key]
	if !ok {
		return def, nil
	}
	switch strings.ToLower(s) {
	case "1", "true", "yes", "on":
		return true, nil
	case "0", "false", "no", "off":
		return false, nil
	}
	return false, fmt.Errorf("%s: expected bool, got %q", key, s)
}

func parseScaling(s string) (client.ElementScaling, error) {
	switch strings.ToLower(s) {
	case "passthrough", "pass", "raw", "":
		return client.ScalingPassthrough, nil
	case "linear":
		return client.ScalingLinear, nil
	case "quadratic", "quad", "power":
		return client.ScalingQuadratic, nil
	}
	return 0, fmt.Errorf("scaling: expected passthrough|linear|quadratic, got %q", s)
}

func scalingName(s client.ElementScaling) string {
	switch s {
	case client.ScalingPassthrough:
		return "passthrough"
	case client.ScalingLinear:
		return "linear"
	case client.ScalingQuadratic:
		return "quadratic"
	}
	return fmt.Sprintf("0x%02x", byte(s))
}

// ─── Servo profile ──────────────────────────────────────────────────

func cmdServoProfileGet(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: servo-profile-get <portIdx>")
	}
	idx, err := parseU8(args[0])
	if err != nil {
		return err
	}
	p, err := a.c.Roles.ServoGetProfile(idx)
	if err != nil {
		return err
	}
	Hdr(fmt.Sprintf("servo[%d] motion profile", idx))
	fmt.Fprintf(out, "  %s %s … %s µs\n", cDim("range:      "), cCyan(fmt.Sprintf("%d", p.MinUs)), cCyan(fmt.Sprintf("%d", p.MaxUs)))
	fmt.Fprintf(out, "  %s %s µs\n", cDim("center:     "), cCyan(fmt.Sprintf("%d", p.CenterUs)))
	fmt.Fprintf(out, "  %s %s\n", cDim("reversed:   "), Bool(p.Reversed))
	fmt.Fprintf(out, "  %s %d µs/s\n", cDim("max speed:  "), p.MaxSpeedUsPerSec)
	fmt.Fprintf(out, "  %s %d µs/s²\n", cDim("max accel:  "), p.MaxAccelUsPerSec2)
	fmt.Fprintf(out, "  %s %d µs/s³  %s\n", cDim("max jerk:   "), p.MaxJerkUsPerSec3,
		cDim(map[bool]string{true: "(S-curve)", false: "(trapezoidal)"}[p.MaxJerkUsPerSec3 > 0]))
	return nil
}

// cmdServoProfilesAll dumps the live motion profile of EVERY attached hub
// servo, one compact row each.  This is the fast way to spot a stale /
// clamped calibration (e.g. a profile whose max is narrower than the
// operator set) — one command instead of probing each port by index.
func cmdServoProfilesAll(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	boards, err := a.c.Topology.RoleList("") // "" = hub-local
	if err != nil {
		return err
	}
	Hdr("hub servo motion profiles (live)")
	n := 0
	for _, rb := range boards {
		for _, r := range rb.Roles {
			if r.RoleKind != roles.KindServoActuator {
				continue
			}
			n++
			p, perr := a.c.Roles.ServoGetProfile(r.PortIdx)
			if perr != nil {
				fmt.Fprintf(out, "  %s  %s\n",
					cBold(fmt.Sprintf("servo[%d]", r.PortIdx)), cRed("read failed: "+perr.Error()))
				continue
			}
			rev := ""
			if p.Reversed {
				rev = "  " + cYellow("REV")
			}
			fmt.Fprintf(out, "  %s  %s…%s µs  center %s  speed %d  accel %d%s\n",
				cBold(fmt.Sprintf("servo[%d]", r.PortIdx)),
				cCyan(fmt.Sprintf("%d", p.MinUs)), cCyan(fmt.Sprintf("%d", p.MaxUs)),
				cCyan(fmt.Sprintf("%d", p.CenterUs)),
				p.MaxSpeedUsPerSec, p.MaxAccelUsPerSec2, rev)
		}
	}
	if n == 0 {
		Note("  (no servo-actuator roles attached on the hub)")
	}
	return nil
}

func cmdServoProfileSet(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: servo-profile-set <portIdx> <key=val> ...")
	}
	idx, err := parseU8(args[0])
	if err != nil {
		return err
	}
	kv, err := parseKeyVals(args[1:])
	if err != nil {
		return err
	}
	// Read-modify-write so partial updates only touch the named fields.
	p, err := a.c.Roles.ServoGetProfile(idx)
	if err != nil {
		return fmt.Errorf("read current profile: %w", err)
	}
	if p.MinUs, err = u16From(kv, "min_us", p.MinUs); err != nil {
		return err
	}
	if p.MaxUs, err = u16From(kv, "max_us", p.MaxUs); err != nil {
		return err
	}
	if p.CenterUs, err = u16From(kv, "center_us", p.CenterUs); err != nil {
		return err
	}
	if p.Reversed, err = boolFrom(kv, "reversed", p.Reversed); err != nil {
		return err
	}
	if p.MaxSpeedUsPerSec, err = u16From(kv, "max_speed", p.MaxSpeedUsPerSec); err != nil {
		return err
	}
	if p.MaxAccelUsPerSec2, err = u16From(kv, "max_accel", p.MaxAccelUsPerSec2); err != nil {
		return err
	}
	if p.MaxJerkUsPerSec3, err = u16From(kv, "max_jerk", p.MaxJerkUsPerSec3); err != nil {
		return err
	}
	if err := a.c.Roles.ServoSetProfile(idx, p); err != nil {
		return err
	}
	Ok("servo[%d] profile updated", idx)
	return cmdServoProfileGet(a, args[:1])
}

// ─── Motor element ──────────────────────────────────────────────────

func cmdMotorElementGet(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: motor-element-get <portIdx>")
	}
	idx, err := parseU8(args[0])
	if err != nil {
		return err
	}
	e, err := a.c.Roles.MotorGetElement(idx)
	if err != nil {
		return err
	}
	Hdr(fmt.Sprintf("motor[%d] element", idx))
	fmt.Fprintf(out, "  %s %s mV  %s\n", cDim("element:  "), cCyan(fmt.Sprintf("%d", e.ElementMv)), cDim("(scaling="+scalingName(e.Scaling)+")"))
	fmt.Fprintf(out, "  %s %s mV  %s\n", cDim("port rail:"), cCyan(fmt.Sprintf("%d", e.PortRailMv)), cDim("(read-only, from port descriptor)"))
	if e.ElementMv > 0 && e.PortRailMv > 0 && e.ElementMv < e.PortRailMv {
		ratio := float64(e.ElementMv) / float64(e.PortRailMv)
		fmt.Fprintf(out, "  %s %s %s\n", cDim("duty cap: "), cYellow(fmt.Sprintf("%.0f%%", ratio*100)),
			cDim(fmt.Sprintf("(at 100%% intent, scaling=%s)", scalingName(e.Scaling))))
	}
	return nil
}

func cmdMotorElementSet(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: motor-element-set <portIdx> <element_mv=...> <scaling=...>")
	}
	idx, err := parseU8(args[0])
	if err != nil {
		return err
	}
	kv, err := parseKeyVals(args[1:])
	if err != nil {
		return err
	}
	e, err := a.c.Roles.MotorGetElement(idx)
	if err != nil {
		return fmt.Errorf("read current element: %w", err)
	}
	if e.ElementMv, err = u16From(kv, "element_mv", e.ElementMv); err != nil {
		return err
	}
	if s, ok := kv["scaling"]; ok {
		if e.Scaling, err = parseScaling(s); err != nil {
			return err
		}
	}
	if err := a.c.Roles.MotorSetElement(idx, e); err != nil {
		return err
	}
	Ok("motor[%d] element updated", idx)
	return cmdMotorElementGet(a, args[:1])
}

// ─── Heater element ─────────────────────────────────────────────────

func cmdHeaterElementGet(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: heater-element-get <portIdx>")
	}
	idx, err := parseU8(args[0])
	if err != nil {
		return err
	}
	e, err := a.c.Roles.HeaterGetElement(idx)
	if err != nil {
		return err
	}
	Hdr(fmt.Sprintf("heater[%d] element", idx))
	fmt.Fprintf(out, "  element:   %d mV  (scaling=%s)\n", e.ElementMv, scalingName(e.Scaling))
	fmt.Fprintf(out, "  drive pct: %d %% %s\n", e.DrivePct, cDim("(of element rated voltage)"))
	fmt.Fprintf(out, "  hyst:      %d cx10  (%.1f °C)\n", e.HystCx10, float64(e.HystCx10)/10.0)
	fmt.Fprintf(out, "  port rail: %d mV  %s\n", e.PortRailMv, cDim("(read-only)"))
	return nil
}

func cmdHeaterElementSet(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: heater-element-set <portIdx> <key=val> ...")
	}
	idx, err := parseU8(args[0])
	if err != nil {
		return err
	}
	kv, err := parseKeyVals(args[1:])
	if err != nil {
		return err
	}
	e, err := a.c.Roles.HeaterGetElement(idx)
	if err != nil {
		return fmt.Errorf("read current element: %w", err)
	}
	if e.ElementMv, err = u16From(kv, "element_mv", e.ElementMv); err != nil {
		return err
	}
	if s, ok := kv["scaling"]; ok {
		if e.Scaling, err = parseScaling(s); err != nil {
			return err
		}
	}
	if e.DrivePct, err = u8From(kv, "drive_pct", e.DrivePct); err != nil {
		return err
	}
	if e.HystCx10, err = i16From(kv, "hyst_cx10", e.HystCx10); err != nil {
		return err
	}
	if err := a.c.Roles.HeaterSetElement(idx, e); err != nil {
		return err
	}
	Ok("heater[%d] element updated", idx)
	return cmdHeaterElementGet(a, args[:1])
}
