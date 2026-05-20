package main

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
}

// ─── identify / status / capabilities ────────────────────────────────

func cmdIdentify(a *app, _ []string) error {
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

func cmdStatus(a *app, _ []string) error {
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
	KV("free RAM", humanBytes(uint64(s.FreeRAMBytes)))
	KVf("last activity", "%s ago", humanDurationMs(s.LastActivityMs))
	KVf("keepalives", "%d", s.KeepaliveCount)
	KV("board state", Phase(s.BoardStateName))
	KV("init flags", HexU32(uint32(s.InitFlags)))
	if len(s.ModuleData) > 0 {
		KV("module data", cDim(fmt.Sprintf("%d bytes (raw)", len(s.ModuleData))))
	}
	return nil
}

func cmdCapabilities(a *app, _ []string) error {
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

func cmdInit(a *app, args []string) error {
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

func cmdReboot(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Hub.Reboot(); err != nil {
		return err
	}
	Warn("reboot requested — device will drop the link")
	return nil
}

func cmdBootsel(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	return a.c.Hub.Bootsel()
}

func cmdKeepalive(a *app, _ []string) error {
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

func cmdI2CScan(a *app, _ []string) error {
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
		fmt.Printf("  %s 0x%02X  %s\n", marker,
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
