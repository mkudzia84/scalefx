package console

import (
	"fmt"
	"strings"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/protocol/gear"
)

func init() {
	register(&command{Name: "gear-list", Usage: "gear-list", Help: "list configured gear units", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearList})
	register(&command{Name: "gear-status", Usage: "gear-status", Help: "per-unit gear lifecycle phases", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearStatus})
	register(&command{Name: "gear-deploy", Usage: "gear-deploy <id>", Help: "lower a gear unit", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearDeploy})
	register(&command{Name: "gear-retract", Usage: "gear-retract <id>", Help: "raise a gear unit", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearRetract})
	register(&command{Name: "gear-stop", Usage: "gear-stop <id>", Help: "hold a strut in place (brake + freeze)", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearStop})
	register(&command{Name: "gear-all", Usage: "gear-all <stop|deploy|retract>", Help: "apply action to every configured gear", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearAll})
	register(&command{Name: "gear-reset", Usage: "gear-reset <id>", Help: "clear a gear's error state (ERROR → unknown)", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearReset})
	register(&command{Name: "gear-estop", Usage: "gear-estop <id|all>", Help: "emergency hold — brake + freeze in place", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearEStop})
	register(&command{Name: "gear-step", Usage: "gear-step <id> <up|down>", Help: "advance a strut ONE leg toward up/down, then park", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearStep})
	register(&command{Name: "gear-info", Usage: "gear-info [id]", Help: "verbose per-strut state (cycle position + faults)", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearInfo})
}

// gearPhaseColored wraps an already-laid-out cell so ANSI codes don't break
// column alignment: green = settled, yellow = moving/held, red = error,
// dim = unknown/unconfigured.
func gearPhaseColored(cell string, p byte) string {
	switch p {
	case gear.PhaseUp, gear.PhaseDown:
		return cGreen(cell)
	case gear.PhaseMovingUp, gear.PhaseMovingDown, gear.PhaseHeld:
		return cYellow(cell)
	case gear.PhaseError:
		return cRed(cell)
	default:
		return cDim(cell)
	}
}

// gearNames maps id → configured name (best-effort; empty on a List error).
func gearNames(a *App) map[uint8]string {
	out := map[uint8]string{}
	if lst, err := a.c.Gear.List(); err == nil {
		for _, g := range lst {
			out[g.ID] = g.Name
		}
	}
	return out
}

// gearStageText is the human stage inside a transit (door vs strut action),
// or a settled descriptor.
func gearStageText(e gear.GearStatus) string {
	if e.SubPhase != gear.SubPhaseIdle {
		return gear.SubPhaseName(e.SubPhase)
	}
	switch e.Phase {
	case gear.PhaseUp, gear.PhaseDown:
		return "settled"
	case gear.PhaseHeld:
		return "frozen"
	case gear.PhaseError:
		return "faulted"
	default:
		return "—"
	}
}

// gearNote is the trailing detail column: fault reason / resume hint.
func gearNote(e gear.GearStatus) string {
	switch e.Phase {
	case gear.PhaseError:
		if tag := gear.ErrorReasonTag(e.ErrReason); tag != "" {
			return cRed("fault: " + tag)
		}
		return cRed("fault")
	case gear.PhaseHeld:
		return cDim("resume gear up / gear down")
	case gear.PhaseUnknown, gear.PhaseUnconfigured:
		return cDim("position unknown — command to home")
	default:
		return ""
	}
}

func cmdGearList(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	lst, err := a.c.Gear.List()
	if err != nil {
		return err
	}
	if len(lst) == 0 {
		Note("(no gear units configured)")
		return nil
	}
	Hdr(fmt.Sprintf("Gear units (%d)", len(lst)))
	for _, g := range lst {
		fmt.Fprintf(out, "  %s  %s\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", g.ID))), g.Name)
	}
	return nil
}

func cmdGearStatus(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	st, err := a.c.Gear.Status()
	if err != nil {
		return err
	}
	if len(st) == 0 {
		Note("(no gear units configured)")
		return nil
	}
	names := gearNames(a)
	Hdr(fmt.Sprintf("Gear status (%d)", len(st)))
	fmt.Fprintf(out, "       %s %s %s %s\n",
		cDim(fmt.Sprintf("%-15s", "name")),
		cDim(fmt.Sprintf("%-11s", "state")),
		cDim(fmt.Sprintf("%-14s", "stage")),
		cDim("detail"))
	for _, e := range st {
		name := names[e.ID]
		if name == "" {
			name = "—"
		}
		if len(name) > 15 {
			name = name[:15]
		}
		fmt.Fprintf(out, "  %s %s %s %s %s\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", e.ID))),
			fmt.Sprintf("%-15s", name),
			gearPhaseColored(fmt.Sprintf("%-11s", gear.PhaseName(e.Phase)), e.Phase),
			fmt.Sprintf("%-14s", gearStageText(e)),
			gearNote(e))
	}
	return nil
}

func cmdGearInfo(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	st, err := a.c.Gear.Status()
	if err != nil {
		return err
	}
	if len(st) == 0 {
		Note("(no gear units configured)")
		return nil
	}
	filter := -1
	if len(args) == 1 {
		id, perr := parseU8(args[0])
		if perr != nil {
			return perr
		}
		filter = int(id)
	}
	names := gearNames(a)
	Hdr("Gear info")
	shown := 0
	for _, e := range st {
		if filter >= 0 && int(e.ID) != filter {
			continue
		}
		shown++
		name := names[e.ID]
		if name == "" {
			name = "(unnamed)"
		}
		fmt.Fprintf(out, "\n  %s %s\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", e.ID))), cBold(name))
		fmt.Fprintf(out, "       state : %s\n",
			gearPhaseColored(gear.PhaseName(e.Phase), e.Phase))
		fmt.Fprintf(out, "       stage : %s\n", gearStageText(e))
		fmt.Fprintf(out, "       cycle : %s\n", gearCycleBar(e.Phase, e.SubPhase))
		if e.Phase == gear.PhaseError {
			tag := gear.ErrorReasonTag(e.ErrReason)
			if tag == "" {
				tag = "unspecified"
			}
			fmt.Fprintf(out, "       fault : %s (0x%02X)\n", cRed(tag), e.ErrReason)
		}
	}
	if shown == 0 {
		Note("no gear with id %d", filter)
	}
	return nil
}

// gearCycleBar renders the symmetric transit as a one-liner with the current
// macro-stage highlighted: doors → strut → doors, bracketed by the endpoints.
func gearCycleBar(phase, sub byte) string {
	switch phase {
	case gear.PhaseUp:
		return cGreen("◀ gear up") + cDim("  ···  doors · strut · doors  ···  gear down")
	case gear.PhaseDown:
		return cDim("gear up  ···  doors · strut · doors  ···  ") + cGreen("gear down ▶")
	case gear.PhaseHeld:
		return cYellow("⏸ held in place (position uncertain)")
	case gear.PhaseError:
		return cRed("■ faulted")
	case gear.PhaseUnknown, gear.PhaseUnconfigured:
		return cDim("? position unknown")
	}
	dir := "lowering"
	if phase == gear.PhaseMovingUp {
		dir = "raising"
	}
	stage := func(label string, active bool) string {
		if active {
			return cYellow("[" + label + "]")
		}
		return cDim(label)
	}
	doorsOpen := sub == gear.SubPhaseDoorsOpening
	strut := sub == gear.SubPhaseStrutMoving || sub == gear.SubPhaseStrutDone || sub == gear.SubPhaseDoorsOpen
	doorsClose := sub == gear.SubPhaseDoorsClosing || sub == gear.SubPhaseDoorsClosed
	return fmt.Sprintf("%s  %s · %s · %s",
		cCyan(dir),
		stage("doors", doorsOpen),
		stage("strut", strut),
		stage("doors", doorsClose))
}

func cmdGearStep(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 2 {
		return fmt.Errorf("usage: gear-step <id> <up|down>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	var target byte
	switch strings.ToLower(args[1]) {
	case "up":
		target = client.GearStepUp
	case "down":
		target = client.GearStepDown
	default:
		return fmt.Errorf("target must be up|down")
	}
	if err := a.c.Gear.Step(id, target); err != nil {
		return err
	}
	Ok("gear[%d] step → %s", id, Phase(args[1]))
	return nil
}

func cmdGearDeploy(a *App, args []string) error {
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
	Ok("gear[%d] %s", id, Phase("deploying"))
	return nil
}

func cmdGearRetract(a *App, args []string) error {
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
	Ok("gear[%d] %s", id, Phase("retracting"))
	return nil
}

func cmdGearStop(a *App, args []string) error {
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
	Ok("gear[%d] held", id)
	return nil
}

func cmdGearEStop(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-estop <id|all>")
	}
	if strings.ToLower(args[0]) == "all" {
		if err := a.c.Gear.EStopAll(); err != nil {
			return err
		}
		Ok("all gear → %s", Phase("held"))
		return nil
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gear.EStop(id); err != nil {
		return err
	}
	Ok("gear[%d] %s", id, Phase("held"))
	return nil
}

func cmdGearReset(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-reset <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gear.Reset(id); err != nil {
		return err
	}
	Ok("gear[%d] error cleared → unknown", id)
	return nil
}

func cmdGearAll(a *App, args []string) error {
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
	Ok("all gear → %s", Phase(gear.AllActionName(action)))
	return nil
}
