package console

import (
	"encoding/hex"
	"fmt"
	"strings"

	"scalefx/client"
	"scalefx/protocol/core"
	expp "scalefx/protocol/expanders"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
)

func init() {
	register(&command{Name: "expanders", Usage: "expanders", Help: "list connected expanders", Category: catTopology, RequiresConn: true, RequiresCap: core.CapTopology | core.CapExpanderBus, Run: cmdExpanders})
	register(&command{Name: "system-info", Usage: "system-info", Help: "hub + all expanders in one round-trip", Category: catTopology, RequiresConn: true, RequiresCap: core.CapTopology | core.CapExpanderBus, Run: cmdSystemInfo})
	register(&command{Name: "topo-ports", Usage: "topo-ports [guid]", Help: "list ports for board (default: all)", Category: catTopology, RequiresConn: true, RequiresCap: core.CapTopology | core.CapExpanderBus, Run: cmdTopoPorts})
	register(&command{Name: "topo-roles", Usage: "topo-roles [guid]", Help: "list roles for board (default: all)", Category: catTopology, RequiresConn: true, RequiresCap: core.CapTopology | core.CapExpanderBus, Run: cmdTopoRoles})
	register(&command{Name: "role-attach", Usage: "role-attach <guid> <portKind> <portIdx> <roleKind> [hex-cfg]", Help: "bind a role to (portKind, portIdx) on a board", Category: catTopology, RequiresConn: true, RequiresCap: core.CapTopology | core.CapExpanderBus, Run: cmdRoleAttach})
	register(&command{Name: "role-detach", Usage: "role-detach <guid> <portKind> <portIdx>", Help: "detach the role on (portKind, portIdx)", Category: catTopology, RequiresConn: true, RequiresCap: core.CapTopology | core.CapExpanderBus, Run: cmdRoleDetach})
}

// ─── Expanders ───────────────────────────────────────────────────────

func cmdExpanders(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	list, err := a.c.Expanders.List()
	if err != nil {
		return err
	}
	if len(list) == 0 {
		Note("(no expanders)")
		return nil
	}
	Hdr("expanders")
	for _, e := range list {
		flag := ""
		if e.Collision {
			flag = "  " + cRed("[COLLISION]")
		}
		if e.Identified {
			fmt.Fprintf(out, "  %s  addr=%d  %s %s%s\n",
				cBold(cCyan(e.KindName)), e.USBAddr,
				e.DeviceName,
				cDim("v"+e.FirmwareVersion),
				flag)
		} else {
			fmt.Fprintf(out, "  %s  addr=%d  %s%s\n",
				cBold(cCyan(e.KindName)), e.USBAddr,
				cDim(fmt.Sprintf("VID=%04X PID=%04X (identifying…)", e.VID, e.PID)),
				flag)
		}
	}
	return nil
}

func cmdSystemInfo(a *App, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	si, err := a.c.Expanders.SystemInfo()
	if err != nil {
		return err
	}
	Hdr("hub")
	fmt.Fprintf(out, "  %s  %s  %s\n",
		cBold(si.Hub.DeviceName),
		cDim("v"+si.Hub.FirmwareVersion),
		cDim(fmt.Sprintf("(%s, %d MHz, %s free, build %d)",
			si.Hub.Platform, si.Hub.CPUFreqMHz,
			humanBytes(uint64(si.Hub.FreeRAMBytes)),
			si.Hub.BuildNumber)))
	if len(si.Expanders) == 0 {
		Note("(no expanders)")
		return nil
	}
	Hdr("expanders")
	for _, e := range si.Expanders {
		_ = expp.KindName(e.Kind) // names already populated
		flag := ""
		if e.Collision {
			flag = "  " + cRed("[COLLISION]")
		}
		if e.Identified {
			fmt.Fprintf(out, "  %s  %s %s%s\n",
				cBold(cCyan(e.KindName)),
				e.DeviceName,
				cDim("v"+e.FirmwareVersion),
				flag)
		} else {
			fmt.Fprintf(out, "  %s  %s%s\n",
				cBold(cCyan(e.KindName)),
				cDim(fmt.Sprintf("addr=%d (identifying…)", e.USBAddr)),
				flag)
		}
		if e.Battery != nil && e.Battery.Valid {
			fmt.Fprintf(out, "      %s\n", batteryText(*e.Battery))
		}
	}
	return nil
}

// batteryText renders a per-expander battery summary line.
func batteryText(b expp.BatteryInfo) string {
	if !b.Present {
		return cDim("battery: not present")
	}
	s := fmt.Sprintf("%.2f V", float64(b.VoltageMV)/1000.0)
	if b.CellCount > 0 {
		s += fmt.Sprintf("  %dS", b.CellCount)
	}
	s += fmt.Sprintf("  %d%%", b.Pct)
	if b.Critical() {
		s += "  " + cRed("[CUTOFF]")
	} else if b.Low() {
		s += "  " + cRed("[LOW]")
	}
	return cDim("battery: ") + s
}

// ─── Topology ────────────────────────────────────────────────────────

func cmdTopoPorts(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	guid := ""
	if len(args) >= 1 {
		guid = args[0]
	}
	boards, err := a.c.Topology.PortList(guid)
	if err != nil {
		return err
	}
	// Join the attached role per port: a second round-trip (RoleList over
	// the same scope) keyed by (guid, kind, idx) → roleKind so each port
	// row shows what's currently bound to it.  Best-effort — if RoleList
	// fails we still render the bare port roster.
	roleAt := map[string]byte{}
	if roleBoards, rerr := a.c.Topology.RoleList(guid); rerr == nil {
		for _, rb := range roleBoards {
			for _, r := range rb.Roles {
				roleAt[roleKey(rb.GUID, r.PortKind, r.PortIdx)] = r.RoleKind
			}
		}
	}
	for i, b := range boards {
		if i > 0 {
			fmt.Fprintln(out, )
		}
		printBoardPorts(b, roleAt)
	}
	return nil
}

// roleKey indexes the attached-role map by board GUID + port address.
func roleKey(guid string, kind, idx byte) string {
	return fmt.Sprintf("%s/%d/%d", guid, kind, idx)
}

// attachedRoleText renders the "→ <role>" suffix for a port that has a
// role bound, or "" when the port is unattached.
func attachedRoleText(roleAt map[string]byte, guid string, kind, idx byte) string {
	if rk, ok := roleAt[roleKey(guid, kind, idx)]; ok {
		return "  " + cDim("→") + " " + cMagenta(roles.KindName(rk))
	}
	return ""
}

// printBoardPorts renders one board's port roster grouped by kind, with
// each port on its own row + decoded flag text + any attached role.
func printBoardPorts(b client.BoardPorts, roleAt map[string]byte) {
	label := b.DeviceName
	if label == "" {
		label = b.GUID
	}
	total := len(b.Ports.Servos) + len(b.Ports.Pwms) +
		len(b.Ports.HBridges) + len(b.Ports.Inputs)
	Hdr(fmt.Sprintf("%s %s  (%d ports)",
		cBold(label),
		cDim("["+b.GUID+"]"),
		total))

	if len(b.Ports.Servos) > 0 {
		fmt.Fprintf(out, "  %s  %s\n",
			cDim(padRight("servo", 8)),
			cDim(fmt.Sprintf("(%d, output)", len(b.Ports.Servos))))
		for _, d := range b.Ports.Servos {
			fmt.Fprintf(out, "    %s  %s%s%s\n",
				cBold(fmt.Sprintf("[%2d]", d.Index)),
				cDim(servoFlagText(d.Flags)),
				voltageText(d.VoltageMv),
				attachedRoleText(roleAt, b.GUID, ports.KindServo, d.Index))
		}
	}
	if len(b.Ports.Pwms) > 0 {
		fmt.Fprintf(out, "  %s  %s\n",
			cDim(padRight("pwm", 8)),
			cDim(fmt.Sprintf("(%d)", len(b.Ports.Pwms))))
		for _, d := range b.Ports.Pwms {
			fmt.Fprintf(out, "    %s  %s%s%s\n",
				cBold(fmt.Sprintf("[%2d]", d.Index)),
				cDim(senseFlagText(d.Flags)),
				voltageText(d.VoltageMv),
				attachedRoleText(roleAt, b.GUID, ports.KindPwm, d.Index))
		}
	}
	if len(b.Ports.HBridges) > 0 {
		fmt.Fprintf(out, "  %s  %s\n",
			cDim(padRight("hbridge", 8)),
			cDim(fmt.Sprintf("(%d)", len(b.Ports.HBridges))))
		for _, d := range b.Ports.HBridges {
			fmt.Fprintf(out, "    %s  %s%s%s\n",
				cBold(fmt.Sprintf("[%2d]", d.Index)),
				cDim(senseFlagText(d.Flags)),
				voltageText(d.VoltageMv),
				attachedRoleText(roleAt, b.GUID, ports.KindHBridge, d.Index))
		}
	}
	if len(b.Ports.Inputs) > 0 {
		fmt.Fprintf(out, "  %s  %s\n",
			cDim(padRight("input", 8)),
			cDim(fmt.Sprintf("(%d)", len(b.Ports.Inputs))))
		for _, d := range b.Ports.Inputs {
			fmt.Fprintf(out, "    %s  %s%s%s\n",
				cBold(fmt.Sprintf("[%2d]", d.Index)),
				cGreen(inputFlagText(d.Flags)),
				voltageText(d.VoltageMv),
				attachedRoleText(roleAt, b.GUID, ports.KindInput, d.Index))
		}
	}
}

// voltageText renders the rail voltage as a short trailing chip:
// "  (8 V)" / "  (3.3 V)" / "" when unknown.
func voltageText(mV uint16) string {
	if mV == 0 {
		return ""
	}
	if mV%1000 == 0 {
		return "  " + cDim(fmt.Sprintf("(%d V)", mV/1000))
	}
	return "  " + cDim(fmt.Sprintf("(%.1f V)", float64(mV)/1000.0))
}

func servoFlagText(f byte) string {
	if f&ports.ServoFlagEmits != 0 {
		return "output (pulse)"
	}
	return fmt.Sprintf("flags=0x%02X", f)
}

func senseFlagText(f byte) string {
	parts := ports.SenseFlagsNames(f)
	if len(parts) == 0 {
		return "no sense"
	}
	pretty := []string{}
	for _, p := range parts {
		switch p {
		case "V":
			pretty = append(pretty, "voltage")
		case "I":
			pretty = append(pretty, "current")
		case "T":
			pretty = append(pretty, "temp")
		}
	}
	return strings.Join(pretty, " + ") + " sense"
}

func inputFlagText(f byte) string {
	if f == 0 {
		return "(no capabilities)"
	}
	return strings.Join(ports.InputFlagsNames(f), " | ")
}

func cmdTopoRoles(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	guid := ""
	if len(args) >= 1 {
		guid = args[0]
	}
	boards, err := a.c.Topology.RoleList(guid)
	if err != nil {
		return err
	}
	for i, b := range boards {
		if i > 0 {
			fmt.Fprintln(out, )
		}
		Hdr(fmt.Sprintf("%s  (%d %s)",
			cMagenta(b.GUID),
			len(b.Roles),
			plural2(len(b.Roles), "role")))
		if len(b.Roles) == 0 {
			Note("  no roles attached")
			continue
		}
		for _, r := range b.Roles {
			fmt.Fprintf(out, "  %s%s  %s  %s%s\n",
				cCyan(ports.KindName(r.PortKind)),
				cBold(fmt.Sprintf("[%d]", r.PortIdx)),
				cDim("→"),
				cBold(roles.KindName(r.RoleKind)),
				roleFlagText(r.Flags))
		}
	}
	return nil
}

func roleFlagText(f byte) string {
	if f == 0 {
		return ""
	}
	return "  " + cDim(fmt.Sprintf("flags=0x%02X", f))
}

func plural2(n int, singular string) string {
	if n == 1 {
		return singular
	}
	return singular + "s"
}

func cmdRoleAttach(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 4 {
		return fmt.Errorf("usage: role-attach <guid> <portKind> <portIdx> <roleKind> [hex-cfg]")
	}
	pk, err := parsePortKind(args[1])
	if err != nil {
		return err
	}
	pi, err := parseU8(args[2])
	if err != nil {
		return err
	}
	rk, ok := roles.KindFromName(args[3])
	if !ok {
		v, e := parseU8(args[3])
		if e != nil {
			return fmt.Errorf("unknown role kind: %s", args[3])
		}
		rk = v
	}
	var cfg []byte
	if len(args) >= 5 {
		c, e := hex.DecodeString(strings.TrimPrefix(args[4], "0x"))
		if e != nil {
			return fmt.Errorf("hex-cfg parse: %w", e)
		}
		cfg = c
	}
	if err := a.c.Topology.AttachRole(args[0], pk, pi, rk, cfg); err != nil {
		return err
	}
	Ok("role attached: %s %s[%d] → %s",
		cMagenta(args[0]), cCyan(ports.KindName(pk)), pi, cBold(roles.KindName(rk)))
	return nil
}

func cmdRoleDetach(a *App, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 3 {
		return fmt.Errorf("usage: role-detach <guid> <portKind> <portIdx>")
	}
	pk, err := parsePortKind(args[1])
	if err != nil {
		return err
	}
	pi, err := parseU8(args[2])
	if err != nil {
		return err
	}
	if err := a.c.Topology.DetachRole(args[0], pk, pi); err != nil {
		return err
	}
	Ok("role detached: %s %s[%d]",
		cMagenta(args[0]), cCyan(ports.KindName(pk)), pi)
	return nil
}

// printPortRow renders a single port-kind row.
func printPortRow(label string, descs []ports.PortDescriptor) {
	if len(descs) == 0 {
		return
	}
	fmt.Fprintf(out, "  %s %s", cDim(padRight(label, 8)), cDim(":"))
	for _, d := range descs {
		fmt.Fprintf(out, " %s%s%s",
			cBold(fmt.Sprintf("[%d", d.Index)),
			cDim(fmt.Sprintf(":0x%02X", d.Flags)),
			cBold("]"))
	}
	fmt.Fprintln(out, )
}
