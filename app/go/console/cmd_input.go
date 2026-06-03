package console

import (
	"fmt"

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
