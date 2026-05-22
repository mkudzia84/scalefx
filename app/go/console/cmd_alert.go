package console

import (
	"fmt"
	"strings"

	"scalefx/protocol/alerts"
	"scalefx/protocol/core"
)

func init() {
	register(&command{Name: "alert", Usage: "alert <info|warning|error|critical> [outputMask]", Help: "play the preset alert sound", Category: catAlert, RequiresConn: true, RequiresCap: core.CapAlerts, Run: cmdAlert})
	register(&command{Name: "alert-stop", Usage: "alert-stop", Help: "silence the alert channel", Category: catAlert, RequiresConn: true, RequiresCap: core.CapAlerts, Run: cmdAlertStop})
	register(&command{Name: "alert-status", Usage: "alert-status", Help: "show alert channel state + last severity", Category: catAlert, RequiresConn: true, RequiresCap: core.CapAlerts, Run: cmdAlertStatus})
}

func cmdAlert(a *App, args []string) error {
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
	mask := byte(0)
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
	Ok("alert: %s", severityLabel(sev))
	return nil
}

func cmdAlertStop(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Alerts.Stop(); err != nil {
		return err
	}
	Ok("alert channel silenced")
	return nil
}

func cmdAlertStatus(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Alerts.Status()
	if err != nil {
		return err
	}
	Hdr("Alerts")
	if s.Playing {
		KV("channel", cGreen("PLAYING"))
	} else {
		KV("channel", cDim("idle"))
	}
	last := cDim("(never)")
	if s.LastSeverity != 0xFF {
		last = severityLabel(s.LastSeverity)
	}
	KV("last", last)
	return nil
}

// severityLabel colours a severity name to match its weight.
func severityLabel(sev byte) string {
	name := alerts.SeverityName(sev)
	switch sev {
	case alerts.SeverityInfo:
		return cCyan(name)
	case alerts.SeverityWarning:
		return cYellow(name)
	case alerts.SeverityError:
		return cRed(name)
	case alerts.SeverityCritical:
		return cBold(cRed(name))
	}
	return name
}
