package main

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
	register(&command{Name: "gear-stop", Usage: "gear-stop <id>", Help: "halt motion (motor brake)", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearStop})
	register(&command{Name: "gear-all", Usage: "gear-all <stop|deploy|retract>", Help: "apply action to every configured gear", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearAll})
	register(&command{Name: "gear-reset", Usage: "gear-reset <id>", Help: "clear a gear's error state (ERROR → retracted)", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearReset})
	register(&command{Name: "gear-calibrate", Usage: "gear-calibrate <id>", Help: "run stall-endpoint calibration sweep", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearCalibrate})
	register(&command{Name: "gear-calib-cancel", Usage: "gear-calib-cancel <id>", Help: "abort an in-progress calibration", Category: catGear, RequiresConn: true, RequiresCap: core.CapGearCtrl, Run: cmdGearCalibCancel})
}

func cmdGearList(a *app, _ []string) error {
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
		fmt.Printf("  %s  %s\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", g.ID))), g.Name)
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
		Note("(no gear units configured)")
		return nil
	}
	Hdr("Gear")
	for _, e := range st {
		fmt.Printf("  %s  %-12s (%s)\n",
			cBold(cMagenta(fmt.Sprintf("[%2d]", e.ID))),
			Phase(gear.PhaseName(e.Phase)),
			gear.PhaseSummary(e.Phase))
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
	Ok("gear[%d] %s", id, Phase("deploying"))
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
	Ok("gear[%d] %s", id, Phase("retracting"))
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
	Ok("gear[%d] stopped", id)
	return nil
}

func cmdGearReset(a *app, args []string) error {
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
	Ok("gear[%d] error cleared → retracted", id)
	return nil
}

func cmdGearCalibrate(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-calibrate <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gear.Calibrate(id); err != nil {
		return err
	}
	Ok("gear[%d] %s (retract→deploy→home; watch gear-status)", id, Phase("calibrating"))
	return nil
}

func cmdGearCalibCancel(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: gear-calib-cancel <id>")
	}
	id, err := parseU8(args[0])
	if err != nil {
		return err
	}
	if err := a.c.Gear.CalibrateCancel(id); err != nil {
		return err
	}
	Ok("gear[%d] calibration cancelled", id)
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
	Ok("all gear → %s", Phase(gear.AllActionName(action)))
	return nil
}
