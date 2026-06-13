package console

import (
	"fmt"
	"math"
	"strconv"

	"scalefx/protocol/core"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

func init() {
	register(&command{
		Name:         "input-verbose",
		Usage:        "input-verbose <on|off>",
		Help:         "stream decoded RC channels (PPM/SBUS/Jeti) — pair with 'subscribe' to see [RC] lines",
		Category:     catCore,
		RequiresConn: true,
		RequiresCap:  core.CapTopology,
		Run:          cmdInputVerbose,
	})
	register(&command{
		Name:         "telemetry",
		Usage:        "telemetry",
		Help:         "snapshot the master's telemetry collection (hub-local + polled input devices) + publish rate",
		Category:     catCore,
		RequiresConn: true,
		RequiresCap:  core.CapTopology,
		Run:          cmdTelemetry,
	})
	register(&command{
		Name:         "links",
		Usage:        "links [link-loss-ms]",
		Help:         "input connection-loss status (per-source state + brownouts); optional arg sets the global link-loss interval",
		Category:     catCore,
		RequiresConn: true,
		RequiresCap:  core.CapTopology,
		Run:          cmdLinks,
	})
}

// cmdLinks shows (or configures) the generic input connection-loss tracking:
// every input source's link state + brownout count + current silence.  With a
// numeric arg, sets the global link-loss interval (ms; 0 disables signalling).
func cmdLinks(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) == 1 {
		ms, err := strconv.Atoi(args[0])
		if err != nil || ms < 0 || ms > 65535 {
			return fmt.Errorf("link-loss-ms must be 0..65535")
		}
		if err := a.c.Input.SetLinkLossMs(uint16(ms)); err != nil {
			return err
		}
		Ok("link-loss interval set to %d ms%s", ms, map[bool]string{true: " (signalling OFF)"}[ms == 0])
	}
	st, err := a.c.Input.GetConnection()
	if err != nil {
		return err
	}
	Ok("input links — loss interval %d ms, %d tracked source(s)", st.LinkLossMs, len(st.Links))
	for _, l := range st.Links {
		src := "hub"
		if l.GUID != "" {
			src = l.GUID
		}
		state := Phase("up")
		switch l.State {
		case 1:
			state = cYellow("LOST")
		case 2:
			state = cRed("DOWN")
		}
		fmt.Fprintf(out, "  %s port=%d  %s  brownouts=%d  silence=%dms\n",
			src, l.PortIdx, state, l.Brownouts, l.SilenceMs)
	}
	if len(st.Links) == 0 {
		Note("  (no input sources seen yet — attach an input + enable its broadcast)")
	}
	return nil
}

// cmdTelemetry prints the live telemetry collection — one block per device
// (the hub itself + any actively-polled input device such as an ESC on IN_2),
// each with its sensors' decimal-scaled values, plus the publish-rate header.
func cmdTelemetry(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	snap, err := a.c.Input.GetTelemetry()
	if err != nil {
		return err
	}
	Ok("telemetry collection — publish %.1f Hz (%d ms/reply), %d active sensor(s)",
		float64(snap.RespHzX10)/10.0, snap.PubIntervalMs, snap.ActiveSensors)
	if len(snap.Devices) == 0 {
		Note("  (empty — attach a Jeti EX input, or wait for an ESC on IN_2)")
		return nil
	}
	for _, d := range snap.Devices {
		tag := "downstream"
		if d.Local {
			tag = "hub-local"
		}
		state := Phase("active")
		if !d.Active {
			state = "stale"
		}
		fmt.Fprintf(out, "  %s  [%s, %s]  usn=%04X lsn=%04X\n",
			cBold(d.Name), tag, state, d.USN, d.LSN)
		for _, s := range d.Sensors {
			val := scaleDecimals(s.Value, s.Decimals)
			act := ""
			if !s.Active {
				act = "  (stale)"
			}
			fmt.Fprintf(out, "      %-20s %12s %-5s%s\n", s.Label, val, s.Unit, act)
		}
	}
	return nil
}

// scaleDecimals renders a raw scaled value with its implied decimal point
// (Jeti EX sends value × 10^decimals).
func scaleDecimals(v int32, decimals byte) string {
	if decimals == 0 {
		return fmt.Sprintf("%d", v)
	}
	return fmt.Sprintf("%.*f", int(decimals), float64(v)/math.Pow10(int(decimals)))
}

// cmdInputVerbose enables (or stops) the firmware's per-port input-frame
// broadcast on every hub-local input port that has an input role attached,
// dispatching the right enable packet by role kind (PPM / SBUS / Jeti EX use
// distinct broadcast channels).  The decoded frames print via 'subscribe'
// (the [RC] handler).  This is the CLI twin of Studio's live channel bars.
func cmdInputVerbose(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: input-verbose <on|off>")
	}
	on, err := parseOnOff(args[0])
	if err != nil {
		return err
	}
	hz := byte(0)
	if on {
		hz = 10
	}

	boards, err := a.c.Topology.RoleList("")
	if err != nil {
		return err
	}
	n := 0
	for _, r := range a.c.Topology.FlattenRoles(boards) {
		if r.PortKind != ports.KindInput {
			continue
		}
		var e error
		switch r.RoleKind {
		case roles.KindSbusInput:
			e = a.c.Input.SetSbusBroadcastHz(r.PortIdx, hz)
		case roles.KindJetiExInput:
			e = a.c.Input.SetJetiBroadcastHz(r.PortIdx, hz)
		case roles.KindRcPwmInput:
			e = a.c.Input.SetBroadcastHz(r.PortIdx, hz)
		default:
			continue
		}
		if e != nil {
			Warn("input[%d] (role %s): %v", r.PortIdx, roles.KindName(r.RoleKind), e)
			continue
		}
		n++
	}

	if n == 0 {
		Note("no input ports with an attached PPM/SBUS/Jeti role — attach one with role-attach")
		return nil
	}
	// Gate the 'subscribe' [RC] printer: only stream per-frame channel lines
	// once the operator explicitly asks for them (otherwise Studio's connect-
	// time broadcast floods the console).
	a.inputVerbose = on
	if on {
		Ok("input verbose %s on %d port(s) @ ~10 Hz", Phase("enabled"), n)
		Note("  run 'subscribe' to see decoded channels ([RC] lines)")
	} else {
		Ok("input verbose %s on %d port(s)", Phase("disabled"), n)
	}
	return nil
}
