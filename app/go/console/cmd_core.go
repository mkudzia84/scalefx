package console

import (
	"fmt"
	"strings"

	"scalefx/client"
	"scalefx/protocol/core"
)

func init() {
	register(&command{Name: "identify", Usage: "identify", Help: "print hub identity", Category: catCore, RequiresConn: true, Run: cmdIdentify})
	register(&command{Name: "status", Usage: "status", Help: "print STATUS payload", Category: catCore, RequiresConn: true, Run: cmdStatus})
	register(&command{Name: "capabilities", Usage: "capabilities", Help: "list hub capability bits", Category: catCore, RequiresConn: true, Run: cmdCapabilities})
	register(&command{Name: "init", Usage: "init [mode] [flags]", Help: "send INIT (default: SLAVE, flags=0)", Category: catCore, RequiresConn: true, Run: cmdInit})
	register(&command{Name: "reboot", Usage: "reboot", Help: "reboot the hub", Category: catCore, RequiresConn: true, Run: cmdReboot})
	register(&command{Name: "bootsel", Usage: "bootsel", Help: "enter bootloader (Pico only)", Category: catCore, RequiresConn: true, Run: cmdBootsel})
	register(&command{Name: "keepalive", Usage: "keepalive", Help: "send one KEEPALIVE packet", Category: catCore, RequiresConn: true, Run: cmdKeepalive})
	register(&command{Name: "i2c-scan", Usage: "i2c-scan", Help: "scan the I²C bus", Category: catCore, RequiresConn: true, Run: cmdI2CScan})
	register(&command{Name: "rc-routing", Usage: "rc-routing [on|off]", Help: "global RC→effect routing gate (no arg = show)", Category: catCore, RequiresConn: true, Run: cmdRcRouting})
}

// ─── identify / status / capabilities ────────────────────────────────

func cmdIdentify(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	id, err := a.c.Hub.Identify()
	if err != nil {
		return err
	}
	Hdr("identity")
	KV("name", cBold(id.DeviceName))
	KV("guid", cMagenta(id.GUID))
	KV("kind", cCyan(boardKindLabel(id)))
	KVf("firmware", "%s build %s", cBold("v"+id.FirmwareVersion), cDim(fmt.Sprintf("%d", id.BuildNumber)))
	KVf("platform", "%s @ %s MHz", id.Platform, cBold(fmt.Sprintf("%d", id.CPUFreqMHz)))
	KV("free RAM", humanBytes(uint64(id.FreeRAMBytes)))
	KV("caps mask", HexU32(id.Capabilities))
	caps := id.CapabilityNames()
	coloured := make([]string, len(caps))
	for i, n := range caps {
		coloured[i] = cGreen(n)
	}
	KVList("features", coloured)
	return nil
}

func boardKindLabel(id client.Identity) string {
	k := id.Kind()
	if k == client.BoardUnknown {
		return "unknown"
	}
	return string(k)
}

func cmdStatus(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Hub.Status()
	if err != nil {
		return err
	}
	Hdr("status")
	KVf("counter", "%d", s.Counter)
	KV("uptime", humanDurationMs(s.UptimeMs))
	KV("free RAM (total)", humanBytes(uint64(s.FreeRAMBytes)))
	if s.HasMemExtension {
		KV("free DRAM", humanBytes(uint64(s.FreeDramBytes)))
		KV("free PSRAM", humanBytes(uint64(s.FreePsramBytes)))
	}
	KVf("last activity", "%s ago", humanDurationMs(s.LastActivityMs))
	KVf("keepalives", "%d", s.KeepaliveCount)
	// Render the contextualised state name ("host-driven" / "hub-driven"
	// / "standalone" / …) using the connection's cached capabilities.
	// The raw enum name ("SLAVE") stays available via s.BoardStateName
	// for log analysis when needed.
	KV("board state", Phase(s.Display(a.boardCaps)))
	KV("init flags", HexU32(uint32(s.InitFlags)))
	if len(s.ModuleData) > 0 {
		KV("module data", cDim(fmt.Sprintf("%d bytes (raw)", len(s.ModuleData))))
	}
	return nil
}

func cmdCapabilities(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	id, err := a.c.Hub.Identify()
	if err != nil {
		return err
	}
	Hdr("capabilities")
	KV("mask", HexU32(id.Capabilities))
	names := id.CapabilityNames()
	coloured := make([]string, len(names))
	for i, n := range names {
		coloured[i] = cGreen(n)
	}
	KVList("flags", coloured)
	return nil
}

// ─── INIT / reboot / bootsel / keepalive ─────────────────────────────

func cmdInit(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	mode := core.InitModeSlave
	flags := core.InitFlagNone
	if len(args) >= 1 {
		switch args[0] {
		case "slave":
			mode = core.InitModeSlave
		case "direct":
			mode = core.InitModeDirect
		default:
			return fmt.Errorf("init mode must be slave|direct")
		}
	}
	if len(args) >= 2 {
		v, err := parseU8(args[1])
		if err != nil {
			return err
		}
		flags = v
	}
	if err := a.c.Hub.InitMode(mode, flags); err != nil {
		return err
	}
	Ok("init: mode=%s flags=0x%02X", cCyan(initModeName(mode)), flags)
	return nil
}

func initModeName(m byte) string {
	switch m {
	case core.InitModeSlave:
		return "slave"
	case core.InitModeDirect:
		return "direct"
	}
	return fmt.Sprintf("0x%02X", m)
}

func cmdReboot(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Hub.Reboot(); err != nil {
		return err
	}
	Warn("reboot requested — device will drop the link")
	return nil
}

func cmdBootsel(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	return a.c.Hub.Bootsel()
}

// cmdRcRouting toggles or shows the master's global RC→effect routing
// gate.  off = effects ignore RC and hold their last state (drive them
// from Studio); live channel monitors keep working either way.
func cmdRcRouting(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) == 0 {
		en, err := a.c.Input.GetRouting()
		if err != nil {
			return err
		}
		Hdr("rc routing")
		KV("state", Phase(rcRoutingLabel(en)))
		return nil
	}
	var en bool
	switch strings.ToLower(args[0]) {
	case "on", "enable", "enabled", "1", "true":
		en = true
	case "off", "disable", "disabled", "0", "false":
		en = false
	default:
		return fmt.Errorf("rc-routing arg must be on|off")
	}
	if err := a.c.Input.SetRouting(en); err != nil {
		return err
	}
	Ok("rc routing → %s", cCyan(rcRoutingLabel(en)))
	return nil
}

func rcRoutingLabel(en bool) string {
	if en {
		return "on (RC drives effects)"
	}
	return "off (manual — effects ignore RC)"
}

func cmdKeepalive(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Hub.Keepalive(); err != nil {
		return err
	}
	Ok("keepalive sent")
	return nil
}

// ─── i2c-scan ────────────────────────────────────────────────────────

func cmdI2CScan(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	devs, err := a.c.Hub.I2CScan()
	if err != nil {
		return err
	}
	if len(devs) == 0 {
		Note("(no I²C devices reported)")
		return nil
	}
	Hdr("i2c scan")
	for _, d := range devs {
		marker := cDim("·")
		if d.Found {
			marker = cGreen("●")
		}
		fmt.Fprintf(out, "  %s 0x%02X  %s\n", marker,
			d.Address, cDim(fmt.Sprintf("id=0x%02X", d.ID)))
	}
	return nil
}

// humanDurationMs renders a millisecond count as "12.3 s" / "5m 23s" /
// "2h 04m 09s".
func humanDurationMs(ms uint32) string {
	s := uint64(ms) / 1000
	switch {
	case s < 60:
		return fmt.Sprintf("%d.%03ds", ms/1000, ms%1000)
	case s < 3600:
		return fmt.Sprintf("%dm %02ds", s/60, s%60)
	default:
		return fmt.Sprintf("%dh %02dm %02ds", s/3600, (s%3600)/60, s%60)
	}
}

// Compile-time keep-alive for strings/client so vet stays happy on
// stripped builds.
var (
	_ = strings.ToLower
	_ = client.BoardUnknown
)
