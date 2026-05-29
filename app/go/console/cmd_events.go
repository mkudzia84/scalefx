package console

import (
	"encoding/hex"
	"fmt"
	"strings"

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

func cmdSubscribe(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	a.c.Events.OnLog(func(m client.LogMessage) {
		fmt.Fprintf(out, "%s @%dms  %s\n",
			levelTag(m.Level), m.Millis, m.Message)
	})
	a.c.Events.OnExpanderConnected(func(e expp.ConnectedEvent) {
		fmt.Fprintf(out, "%s %s addr=%d  %s\n",
			cGreen("[EXP+]"), cCyan(e.KindName), e.USBAddr,
			cDim(fmt.Sprintf("VID=%04X PID=%04X", e.VID, e.PID)))
	})
	a.c.Events.OnExpanderIdentified(func(e expp.ExpanderEntry) {
		fmt.Fprintf(out, "%s %s addr=%d  guid=%s  %s %s\n",
			cGreen("[EXP=]"), cCyan(e.KindName), e.USBAddr,
			cMagenta(e.GUID), e.DeviceName, cDim("v"+e.FirmwareVersion))
	})
	a.c.Events.OnExpanderDisconnected(func(e expp.DisconnectedEvent) {
		fmt.Fprintf(out, "%s %s addr=%d  guid=%s\n",
			cRed("[EXP-]"), cCyan(e.KindName), e.USBAddr, cMagenta(e.GUID))
	})
	a.c.Events.OnExpanderCollision(func(e expp.CollisionEvent) {
		fmt.Fprintf(out, "%s guid=%s  addrA=%d  addrB=%d\n",
			cRed("[!! COLLISION]"), cMagenta(e.GUID), e.USBAddrA, e.USBAddrB)
	})
	a.c.Events.OnRoleEvent(func(ev topology.RoleEvent) {
		fmt.Fprintf(out, "%s guid=%s  inner=%s  %s\n",
			cMagenta("[ROLE]"), cMagenta(ev.GUID),
			cCyan(ev.InnerType.String()),
			cDim(hex.EncodeToString(ev.InnerPayload)))
	})
	a.c.Events.OnUploadProgress(func(p storage.UploadProgress) {
		fmt.Fprintf(out, "%s seg=%d bytes=%d ring=%d%%\n",
			cYellow("[UPLOAD]"), p.SegmentIdx, p.BytesReceived, p.RingFillPct)
	})
	a.c.Events.OnLandingLightPhase(func(ev landing.PhaseChange) {
		fmt.Fprintf(out, "%s landing[%d] → %s\n",
			cBlue("[LL]"), ev.ID, Phase(landing.PhaseName(ev.Phase)))
	})
	a.c.Events.OnGearPhase(func(ev gear.PhaseChange) {
		fmt.Fprintf(out, "%s gear[%d] → %s\n",
			cBlue("[GEAR]"), ev.ID, Phase(gear.PhaseName(ev.Phase)))
	})
	a.c.Events.OnEngineState(func(ev enginefx.StateChange) {
		fmt.Fprintf(out, "%s engine → %s\n",
			cBlue("[ENG]"), Phase(enginefx.StateName(ev.State)))
	})
	a.c.Events.OnGunVerboseStatus(func(ev gunfx.VerboseStatus) {
		fmt.Fprintf(out, "%s gun[%d] firing=%v smoke=%v fan=%v heaterPct=%d rof=%d trig=%dus\n",
			cBlue("[GUNV]"), ev.ID, ev.Firing, ev.SmokeArmed, ev.SmokeFanRunning,
			ev.HeaterDutyPct, ev.RofIndex, ev.TriggerUs)
	})
	a.c.Events.OnInputValue(func(iv client.InputValue) {
		var sb strings.Builder
		for i, ch := range iv.Channels {
			if i > 0 {
				sb.WriteByte(' ')
			}
			if ch.Valid {
				fmt.Fprintf(&sb, "%4d", ch.Us)
			} else {
				sb.WriteString("----")
			}
		}
		g := iv.GUID
		if g == "" {
			g = "hub"
		}
		fmt.Fprintf(out, "%s %s port=%d proto=%s  %s  (%dch)\n",
			cBlue("[RC]"), cMagenta(g), iv.PortIdx, cCyan(iv.Protocol),
			sb.String(), len(iv.Channels))
	})
	// gun-shot events are very high rate during sustained fire — skip the
	// per-shot console line; subscribers that want them register their own
	// observer on a.c.Events.OnGunShot.
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
