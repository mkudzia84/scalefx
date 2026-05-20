package main

import (
	"encoding/hex"
	"fmt"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/protocol/enginefx"
	expp "scalefx/protocol/expanders"
	"scalefx/protocol/gear"
	"scalefx/protocol/gunfx"
	"scalefx/protocol/landing"
	"scalefx/protocol/storage"
	"scalefx/protocol/topology"
)

func init() {
	register(&command{
		Name:         "subscribe",
		Usage:        "subscribe",
		Help:         "stream every async packet to stdout (Ctrl+C to stop)",
		Category:     catEvents,
		RequiresConn: true,
		Run:          cmdSubscribe,
	})
}

func cmdSubscribe(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	a.c.Events.OnLog(func(m client.LogMessage) {
		fmt.Printf("%s @%dms  %s\n",
			levelTag(m.Level), m.Millis, m.Message)
	})
	a.c.Events.OnExpanderConnected(func(e expp.ConnectedEvent) {
		fmt.Printf("%s %s addr=%d  %s\n",
			cGreen("[EXP+]"), cCyan(e.KindName), e.USBAddr,
			cDim(fmt.Sprintf("VID=%04X PID=%04X", e.VID, e.PID)))
	})
	a.c.Events.OnExpanderIdentified(func(e expp.ExpanderEntry) {
		fmt.Printf("%s %s addr=%d  guid=%s  %s %s\n",
			cGreen("[EXP=]"), cCyan(e.KindName), e.USBAddr,
			cMagenta(e.GUID), e.DeviceName, cDim("v"+e.FirmwareVersion))
	})
	a.c.Events.OnExpanderDisconnected(func(e expp.DisconnectedEvent) {
		fmt.Printf("%s %s addr=%d  guid=%s\n",
			cRed("[EXP-]"), cCyan(e.KindName), e.USBAddr, cMagenta(e.GUID))
	})
	a.c.Events.OnExpanderCollision(func(e expp.CollisionEvent) {
		fmt.Printf("%s guid=%s  addrA=%d  addrB=%d\n",
			cRed("[!! COLLISION]"), cMagenta(e.GUID), e.USBAddrA, e.USBAddrB)
	})
	a.c.Events.OnRoleEvent(func(ev topology.RoleEvent) {
		fmt.Printf("%s guid=%s  inner=%s  %s\n",
			cMagenta("[ROLE]"), cMagenta(ev.GUID),
			cCyan(ev.InnerType.String()),
			cDim(hex.EncodeToString(ev.InnerPayload)))
	})
	a.c.Events.OnUploadProgress(func(p storage.UploadProgress) {
		fmt.Printf("%s seg=%d bytes=%d ring=%d%%\n",
			cYellow("[UPLOAD]"), p.SegmentIdx, p.BytesReceived, p.RingFillPct)
	})
	a.c.Events.OnLandingLightPhase(func(ev landing.PhaseChange) {
		fmt.Printf("%s landing[%d] → %s\n",
			cBlue("[LL]"), ev.ID, Phase(landing.PhaseName(ev.Phase)))
	})
	a.c.Events.OnGearPhase(func(ev gear.PhaseChange) {
		fmt.Printf("%s gear[%d] → %s\n",
			cBlue("[GEAR]"), ev.ID, Phase(gear.PhaseName(ev.Phase)))
	})
	a.c.Events.OnEngineState(func(ev enginefx.StateChange) {
		fmt.Printf("%s engine → %s\n",
			cBlue("[ENG]"), Phase(enginefx.StateName(ev.State)))
	})
	a.c.Events.OnGunShot(func(ev gunfx.Shot) {
		fmt.Printf("%s gun[%d] %s\n",
			cBlue("[GUN]"), ev.ID, cRed("shot fired"))
	})
	Info("subscribing — events stream until disconnect")
	return nil
}

func cBlue(s string) string { return wrap(ansiBlue, s) }

// levelTag colours the diag-log severity prefix.
func levelTag(level byte) string {
	name := core.DiagLevelName(level)
	switch name {
	case "ERROR":
		return cRed("[LOG E]")
	case "WARN":
		return cYellow("[LOG W]")
	case "INFO":
		return cCyan("[LOG I]")
	case "DEBUG":
		return cDim("[LOG D]")
	}
	return cDim("[LOG ?]")
}
