package main

import (
	"encoding/hex"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"

	"scalefx/client"
	"scalefx/protocol/audio"
	"scalefx/protocol/core"
	"scalefx/protocol/enginefx"
	expp "scalefx/protocol/expanders"
	"scalefx/protocol/gear"
	"scalefx/protocol/gunfx"
	"scalefx/protocol/landing"
	"scalefx/protocol/ports"
	"scalefx/protocol/roles"
	"scalefx/protocol/storage"
	"scalefx/protocol/topology"
)

// command is one registered CLI verb.
type command struct {
	Name  string
	Usage string
	Help  string
	Run   func(a *app, args []string) error
}

// aliases lets short forms dispatch to canonical command names.
var aliases = map[string]string{
	"ls":     "files",
	"rm":     "delete",
	"q":      "quit",
	"?":      "help",
	"caps":   "capabilities",
	"id":     "identify",
}

var commands map[string]*command

func init() {
	commands = map[string]*command{
		// ── Connection ─────────────────────────────────────────────
		"help":       {Name: "help", Usage: "help [cmd]", Help: "list commands or describe one", Run: cmdHelp},
		"ports":      {Name: "ports", Usage: "ports", Help: "list attached serial ports", Run: cmdPorts},
		"connect":    {Name: "connect", Usage: "connect <port>", Help: "open a serial port (or tcp://host:port)", Run: cmdConnect},
		"disconnect": {Name: "disconnect", Usage: "disconnect", Help: "close the current port", Run: cmdDisconnect},
		"verbose":    {Name: "verbose", Usage: "verbose <on|off>", Help: "toggle wire-level packet logging", Run: cmdVerbose},
		"quit":       {Name: "quit", Usage: "quit", Help: "exit the CLI", Run: func(_ *app, _ []string) error { os.Exit(0); return nil }},

		// ── Core hub ───────────────────────────────────────────────
		"identify":     {Name: "identify", Usage: "identify", Help: "print hub identity", Run: cmdIdentify},
		"status":       {Name: "status", Usage: "status", Help: "print STATUS payload", Run: cmdStatus},
		"capabilities": {Name: "capabilities", Usage: "capabilities", Help: "list hub capability bits", Run: cmdCapabilities},
		"init":         {Name: "init", Usage: "init [mode] [flags]", Help: "send INIT (default: SLAVE, flags=0)", Run: cmdInit},
		"reboot":       {Name: "reboot", Usage: "reboot", Help: "reboot the hub", Run: cmdReboot},
		"bootsel":      {Name: "bootsel", Usage: "bootsel", Help: "enter bootloader (Pico only)", Run: cmdBootsel},
		"keepalive":    {Name: "keepalive", Usage: "keepalive", Help: "send one KEEPALIVE packet", Run: cmdKeepalive},
		"i2c-scan":     {Name: "i2c-scan", Usage: "i2c-scan", Help: "scan the I²C bus", Run: cmdI2CScan},

		// ── Expanders ──────────────────────────────────────────────
		"expanders":   {Name: "expanders", Usage: "expanders", Help: "list connected expanders", Run: cmdExpanders},
		"system-info": {Name: "system-info", Usage: "system-info", Help: "hub + all expanders in one round-trip", Run: cmdSystemInfo},

		// ── Topology ───────────────────────────────────────────────
		"topo-ports":  {Name: "topo-ports", Usage: "topo-ports [guid]", Help: "list ports for board (default: all)", Run: cmdTopoPorts},
		"topo-roles":  {Name: "topo-roles", Usage: "topo-roles [guid]", Help: "list roles for board (default: all)", Run: cmdTopoRoles},
		"role-attach": {Name: "role-attach", Usage: "role-attach <guid> <portKind> <portIdx> <roleKind> [hex-cfg]", Help: "bind a role to (portKind, portIdx) on a board", Run: cmdRoleAttach},
		"role-detach": {Name: "role-detach", Usage: "role-detach <guid> <portKind> <portIdx>", Help: "detach the role on (portKind, portIdx)", Run: cmdRoleDetach},

		// ── Storage / files ────────────────────────────────────────
		"sd-init":      {Name: "sd-init", Usage: "sd-init [speed_mhz]", Help: "(re-)initialise the SD card driver", Run: cmdSdInit},
		"sd-status":    {Name: "sd-status", Usage: "sd-status", Help: "print SD card status", Run: cmdSdStatus},
		"flash-status": {Name: "flash-status", Usage: "flash-status", Help: "print LittleFS flash status", Run: cmdFlashStatus},
		"files":        {Name: "files", Usage: "files [path] [sd|flash]", Help: "list a directory (default: /, sd)", Run: cmdFiles},
		"tree":         {Name: "tree", Usage: "tree [path] [sd|flash]", Help: "recursive listing", Run: cmdTree},
		"file-info":    {Name: "file-info", Usage: "file-info <path>", Help: "metadata for a path", Run: cmdFileInfo},
		"mkdir":        {Name: "mkdir", Usage: "mkdir [-p] <path> [sd|flash]", Help: "create a directory", Run: cmdMkdir},
		"delete":       {Name: "delete", Usage: "delete [-r] <path> [sd|flash]", Help: "remove a file or tree", Run: cmdDelete},
		"upload":       {Name: "upload", Usage: "upload <local> <remote> [sd|flash]", Help: "upload a local file", Run: cmdUpload},
		"download":     {Name: "download", Usage: "download <remote> <local>", Help: "download a file", Run: cmdDownload},

		// ── Audio ──────────────────────────────────────────────────
		"play":         {Name: "play", Usage: "play <ch> <path> [vol] [output]", Help: "start playback on a mixer channel", Run: cmdPlay},
		"audio-stop":   {Name: "audio-stop", Usage: "audio-stop [ch|all]", Help: "stop one channel or all", Run: cmdAudioStop},
		"volume":       {Name: "volume", Usage: "volume <ch|master> <0-100>", Help: "set channel or master volume", Run: cmdVolume},
		"fade":         {Name: "fade", Usage: "fade <ch>", Help: "fade-stop a channel", Run: cmdFade},
		"queue":        {Name: "queue", Usage: "queue <ch> <path> [now|finish]", Help: "queue a follow-on sound", Run: cmdQueue},
		"queue-clear":  {Name: "queue-clear", Usage: "queue-clear <ch|all>", Help: "clear queue on a channel", Run: cmdQueueClear},
		"audio-status": {Name: "audio-status", Usage: "audio-status", Help: "raw AUDIO_STATUS_RESP payload", Run: cmdAudioStatus},
		"codec-status": {Name: "codec-status", Usage: "codec-status", Help: "raw CODEC_STATUS_RESP payload", Run: cmdCodecStatus},

		// ── LightFX ───────────────────────────────────────────────
		"light-programs":    {Name: "light-programs", Usage: "light-programs", Help: "list LightFX programs registered on the hub", Run: cmdLightPrograms},
		"light-status":      {Name: "light-status", Usage: "light-status", Help: "active LightFX program + master brightness", Run: cmdLightStatus},
		"light-select":      {Name: "light-select", Usage: "light-select <idx|name>", Help: "switch to a LightFX program (idx or name)", Run: cmdLightSelect},
		"light-reset":       {Name: "light-reset", Usage: "light-reset", Help: "drop the active LightFX program; LEDs off", Run: cmdLightReset},
		"light-brightness":  {Name: "light-brightness", Usage: "light-brightness <0-100>", Help: "set LightFX master brightness percent", Run: cmdLightBrightness},

		// ── Landing lights ────────────────────────────────────────
		"landing-list":   {Name: "landing-list", Usage: "landing-list", Help: "list configured landing lights (owner + phase)", Run: cmdLandingList},
		"landing-status": {Name: "landing-status", Usage: "landing-status", Help: "per-light lifecycle phases", Run: cmdLandingStatus},
		"landing-on":     {Name: "landing-on", Usage: "landing-on <id>", Help: "deploy + power on a landing light", Run: cmdLandingOn},
		"landing-off":    {Name: "landing-off", Usage: "landing-off <id>", Help: "power off + retract a landing light", Run: cmdLandingOff},

		// ── GearControl ───────────────────────────────────────────
		"gear-list":    {Name: "gear-list", Usage: "gear-list", Help: "list configured gear units", Run: cmdGearList},
		"gear-status":  {Name: "gear-status", Usage: "gear-status", Help: "per-unit gear lifecycle phases", Run: cmdGearStatus},
		"gear-deploy":  {Name: "gear-deploy", Usage: "gear-deploy <id>", Help: "lower a gear unit", Run: cmdGearDeploy},
		"gear-retract": {Name: "gear-retract", Usage: "gear-retract <id>", Help: "raise a gear unit", Run: cmdGearRetract},
		"gear-stop":    {Name: "gear-stop", Usage: "gear-stop <id>", Help: "halt motion / clear error state", Run: cmdGearStop},
		"gear-all":     {Name: "gear-all", Usage: "gear-all <stop|deploy|retract>", Help: "apply action to every configured gear", Run: cmdGearAll},

		// ── EngineFX ──────────────────────────────────────────────
		"engine-start":  {Name: "engine-start", Usage: "engine-start", Help: "kick off engine startup sequence", Run: cmdEngineStart},
		"engine-stop":   {Name: "engine-stop", Usage: "engine-stop", Help: "kick off engine shutdown sequence", Run: cmdEngineStop},
		"engine-status": {Name: "engine-status", Usage: "engine-status", Help: "current engine state + RC toggle", Run: cmdEngineStatus},

		// ── GunFX ────────────────────────────────────────────────
		"gun-fire":   {Name: "gun-fire", Usage: "gun-fire <id>", Help: "fire exactly one shot", Run: cmdGunFire},
		"gun-start":  {Name: "gun-start", Usage: "gun-start <id> [rpm]", Help: "start auto-fire at <rpm> rounds/min (0 = default)", Run: cmdGunStart},
		"gun-stop":   {Name: "gun-stop", Usage: "gun-stop <id>", Help: "stop auto-fire", Run: cmdGunStop},
		"gun-smoke":  {Name: "gun-smoke", Usage: "gun-smoke <id> <on|off>", Help: "arm/disarm the smoke heater", Run: cmdGunSmoke},
		"gun-status": {Name: "gun-status", Usage: "gun-status", Help: "per-gun firing + smoke state", Run: cmdGunStatus},

		// ── Alerts ────────────────────────────────────────────────
		"alert":        {Name: "alert", Usage: "alert <info|warning|error|critical> [outputMask]", Help: "play the preset alert sound", Run: cmdAlert},
		"alert-stop":   {Name: "alert-stop", Usage: "alert-stop", Help: "silence the alert channel", Run: cmdAlertStop},
		"alert-status": {Name: "alert-status", Usage: "alert-status", Help: "show alert channel state + last severity", Run: cmdAlertStatus},

		// ── Async events ──────────────────────────────────────────
		"subscribe":   {Name: "subscribe", Usage: "subscribe", Help: "print every async packet to stdout (Ctrl+C to stop)", Run: cmdSubscribe},
	}
}

// ─── Help / connection ───────────────────────────────────────────────

func cmdHelp(a *app, args []string) error {
	if len(args) > 0 {
		c, ok := commands[args[0]]
		if !ok {
			if alt, ok2 := aliases[args[0]]; ok2 {
				c = commands[alt]
				ok = true
			}
		}
		if !ok {
			return fmt.Errorf("unknown command: %s", args[0])
		}
		fmt.Printf("%s\n  %s\n  %s\n", c.Name, c.Usage, c.Help)
		return nil
	}
	names := make([]string, 0, len(commands))
	for n := range commands {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		c := commands[n]
		fmt.Printf("  %-14s %s\n", c.Name, c.Help)
	}
	return nil
}

func cmdPorts(a *app, args []string) error {
	pl := client.ListSerialPortsDetailed()
	if len(pl) == 0 {
		fmt.Println("(no serial ports found)")
		return nil
	}
	for _, p := range pl {
		fmt.Printf("  %-12s  %s\n", p.Name, p.Description)
	}
	return nil
}

func cmdConnect(a *app, args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: connect <port>")
	}
	return a.connect(args[0])
}

func cmdDisconnect(a *app, args []string) error {
	if a.c != nil {
		a.c.Close()
		a.c = nil
		fmt.Println("disconnected.")
	}
	return nil
}

func cmdVerbose(a *app, args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: verbose <on|off>")
	}
	v := args[0] == "on" || args[0] == "true" || args[0] == "1"
	a.verbose = v
	if a.c != nil {
		a.c.SetVerbose(v)
	}
	if v {
		fmt.Println("wire logging: ON")
	} else {
		fmt.Println("wire logging: off")
	}
	return nil
}

// ─── Core hub ────────────────────────────────────────────────────────

func cmdIdentify(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	id, err := a.c.Hub.Identify()
	if err != nil {
		return err
	}
	fmt.Printf("  Name        : %s\n", id.DeviceName)
	fmt.Printf("  GUID        : %s\n", id.GUID)
	fmt.Printf("  Kind        : %s\n", boardKindLabel(id))
	fmt.Printf("  Firmware    : v%s build %d\n", id.FirmwareVersion, id.BuildNumber)
	fmt.Printf("  Platform    : %s (%d MHz)\n", id.Platform, id.CPUFreqMHz)
	fmt.Printf("  Free RAM    : %s\n", humanBytes(uint64(id.FreeRAMBytes)))
	fmt.Printf("  Features    : 0x%08X\n", id.Capabilities)
	for _, name := range id.CapabilityNames() {
		fmt.Printf("              · %s\n", name)
	}
	return nil
}

func boardKindLabel(id client.Identity) string {
	k := id.Kind()
	if k == client.BoardUnknown {
		return "unknown"
	}
	return string(k)
}

func cmdStatus(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Hub.Status()
	if err != nil {
		return err
	}
	fmt.Printf("  Counter      : %d\n", s.Counter)
	fmt.Printf("  Uptime       : %d ms\n", s.UptimeMs)
	fmt.Printf("  Free RAM     : %s\n", humanBytes(uint64(s.FreeRAMBytes)))
	fmt.Printf("  Last activity: %d ms ago\n", s.LastActivityMs)
	fmt.Printf("  Keepalives   : %d\n", s.KeepaliveCount)
	fmt.Printf("  Board state  : %s\n", s.BoardStateName)
	fmt.Printf("  Init flags   : 0x%02X\n", s.InitFlags)
	if len(s.ModuleData) > 0 {
		fmt.Printf("  Module data  : %d bytes (raw)\n", len(s.ModuleData))
	}
	return nil
}

func cmdCapabilities(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	id, err := a.c.Hub.Identify()
	if err != nil {
		return err
	}
	names := id.CapabilityNames()
	if len(names) == 0 {
		fmt.Println("(no capabilities advertised)")
		return nil
	}
	for _, n := range names {
		fmt.Printf("  %s\n", n)
	}
	return nil
}

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
	fmt.Println("OK")
	return nil
}

func cmdReboot(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if err := a.c.Hub.Reboot(); err != nil {
		return err
	}
	fmt.Println("reboot requested.")
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
	return a.c.Hub.Keepalive()
}

func cmdI2CScan(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	devs, err := a.c.Hub.I2CScan()
	if err != nil {
		return err
	}
	for _, d := range devs {
		mark := " "
		if d.Found {
			mark = "*"
		}
		fmt.Printf("  %s 0x%02X (id=0x%02X)\n", mark, d.Address, d.ID)
	}
	return nil
}

// ─── Expanders ───────────────────────────────────────────────────────

func cmdExpanders(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	list, err := a.c.Expanders.List()
	if err != nil {
		return err
	}
	if len(list) == 0 {
		fmt.Println("(no expanders)")
		return nil
	}
	for _, e := range list {
		flag := ""
		if e.Collision {
			flag = " [COLLISION]"
		}
		if e.Identified {
			fmt.Printf("  %s  addr=%d  %s v%s  caps=%v  build=%d%s\n",
				e.KindName, e.USBAddr, e.DeviceName, e.FirmwareVersion,
				core.CapabilityNames(e.Capabilities), e.BuildNumber, flag)
		} else {
			fmt.Printf("  %s  addr=%d  VID=%04X PID=%04X (identifying...)%s\n",
				e.KindName, e.USBAddr, e.VID, e.PID, flag)
		}
	}
	return nil
}

func cmdSystemInfo(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	si, err := a.c.Expanders.SystemInfo()
	if err != nil {
		return err
	}
	fmt.Printf("Hub:\n")
	fmt.Printf("  %s  v%s  (%s, %d MHz, %s free)  caps=%v  build=%d\n",
		si.Hub.DeviceName, si.Hub.FirmwareVersion, si.Hub.Platform,
		si.Hub.CPUFreqMHz, humanBytes(uint64(si.Hub.FreeRAMBytes)),
		core.CapabilityNames(si.Hub.Capabilities), si.Hub.BuildNumber)
	if len(si.Expanders) == 0 {
		fmt.Println("Expanders: (none)")
		return nil
	}
	fmt.Println("Expanders:")
	for _, e := range si.Expanders {
		_ = expp.KindName(e.Kind) // names already populated
		flag := ""
		if e.Collision {
			flag = " [COLLISION]"
		}
		if e.Identified {
			fmt.Printf("  - %s  %s v%s  caps=%v%s\n",
				e.KindName, e.DeviceName, e.FirmwareVersion,
				core.CapabilityNames(e.Capabilities), flag)
		} else {
			fmt.Printf("  - %s  addr=%d (identifying...)%s\n",
				e.KindName, e.USBAddr, flag)
		}
	}
	return nil
}

// ─── Topology ────────────────────────────────────────────────────────

func cmdTopoPorts(a *app, args []string) error {
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
	for _, b := range boards {
		label := b.DeviceName
		if label == "" {
			label = b.GUID
		}
		fmt.Printf("%s (%s):\n", label, b.GUID)
		printPortRow("servo  ", b.Ports.Servos)
		printPortRow("pwm    ", b.Ports.Pwms)
		printPortRow("hbridge", b.Ports.HBridges)
		printPortRow("input  ", b.Ports.Inputs)
	}
	return nil
}

func cmdTopoRoles(a *app, args []string) error {
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
	for _, b := range boards {
		fmt.Printf("%s:\n", b.GUID)
		if len(b.Roles) == 0 {
			fmt.Println("  (no roles attached)")
			continue
		}
		for _, r := range b.Roles {
			fmt.Printf("  %s[%d] → %s  flags=0x%02X\n",
				ports.KindName(r.PortKind), r.PortIdx,
				roles.KindName(r.RoleKind), r.Flags)
		}
	}
	return nil
}

func cmdRoleAttach(a *app, args []string) error {
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
	fmt.Println("OK")
	return nil
}

func cmdRoleDetach(a *app, args []string) error {
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
	fmt.Println("OK")
	return nil
}

// ─── Storage ─────────────────────────────────────────────────────────

func cmdSdInit(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	speed := byte(0)
	if len(args) >= 1 {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		speed = v
	}
	return a.c.Storage.SdInit(speed)
}

func cmdSdStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Storage.SdStatus()
	if err != nil {
		return err
	}
	fmt.Printf("  Initialized: %v\n", s.Initialized)
	fmt.Printf("  Card size  : %s\n", humanBytes(uint64(s.CardSizeMB)*1024*1024))
	fmt.Printf("  Total      : %s\n", humanBytes(uint64(s.TotalSpaceMB)*1024*1024))
	fmt.Printf("  Used       : %s\n", humanBytes(uint64(s.UsedSpaceMB)*1024*1024))
	fmt.Printf("  Free       : %s\n", humanBytes(uint64(s.FreeSpaceMB)*1024*1024))
	fmt.Printf("  FAT type   : %d\n", s.FatType)
	if s.CardType != 0 {
		fmt.Printf("  Card type  : %d / bus mode %d\n", s.CardType, s.BusMode)
	}
	return nil
}

func cmdFlashStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Storage.FlashStatus()
	if err != nil {
		return err
	}
	fmt.Printf("  Initialized: %v\n", s.Initialized)
	fmt.Printf("  Total      : %s\n", humanBytes(uint64(s.TotalSpaceMB)*1024*1024))
	fmt.Printf("  Used       : %s\n", humanBytes(uint64(s.UsedSpaceMB)*1024*1024))
	fmt.Printf("  Free       : %s\n", humanBytes(uint64(s.FreeSpaceMB)*1024*1024))
	return nil
}

func cmdFiles(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	path, target := "/", storage.TargetSD
	if len(args) >= 1 {
		path = args[0]
	}
	if len(args) >= 2 {
		t, err := parseTarget(args[1])
		if err != nil {
			return err
		}
		target = t
	}
	out, err := a.c.Storage.FileList(path, target)
	if err != nil {
		return err
	}
	fmt.Print(out)
	if !strings.HasSuffix(out, "\n") {
		fmt.Println()
	}
	return nil
}

func cmdTree(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	path, target := "/", storage.TargetSD
	if len(args) >= 1 {
		path = args[0]
	}
	if len(args) >= 2 {
		t, err := parseTarget(args[1])
		if err != nil {
			return err
		}
		target = t
	}
	out, err := a.c.Storage.FileTree(path, target)
	if err != nil {
		return err
	}
	fmt.Print(out)
	return nil
}

func cmdFileInfo(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: file-info <path>")
	}
	info, err := a.c.Storage.FileInfo(args[0])
	if err != nil {
		return err
	}
	if !info.Exists {
		fmt.Println("(does not exist)")
		return nil
	}
	kind := "file"
	if info.IsDir {
		kind = "dir"
	}
	fmt.Printf("  %s  %s  size=%s\n", args[0], kind, humanBytes(uint64(info.Size)))
	return nil
}

func cmdMkdir(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	flags := storage.MkdirFlagNone
	if len(args) >= 1 && args[0] == "-p" {
		flags = storage.MkdirFlagParents
		args = args[1:]
	}
	if len(args) < 1 {
		return fmt.Errorf("usage: mkdir [-p] <path> [sd|flash]")
	}
	target := storage.TargetSD
	if len(args) >= 2 {
		t, err := parseTarget(args[1])
		if err != nil {
			return err
		}
		target = t
	}
	if err := a.c.Storage.FileMkdir(args[0], target, flags); err != nil {
		return err
	}
	fmt.Println("OK")
	return nil
}

func cmdDelete(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	flags := storage.DeleteFlagNone
	if len(args) >= 1 && args[0] == "-r" {
		flags = storage.DeleteFlagRecursive
		args = args[1:]
	}
	if len(args) < 1 {
		return fmt.Errorf("usage: delete [-r] <path> [sd|flash]")
	}
	target := storage.TargetSD
	if len(args) >= 2 {
		t, err := parseTarget(args[1])
		if err != nil {
			return err
		}
		target = t
	}
	if err := a.c.Storage.FileDelete(args[0], target, flags); err != nil {
		return err
	}
	fmt.Println("OK")
	return nil
}

func cmdUpload(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: upload <local> <remote> [sd|flash]")
	}
	target := storage.TargetSD
	if len(args) >= 3 {
		t, err := parseTarget(args[2])
		if err != nil {
			return err
		}
		target = t
	}
	opt := client.UploadOptions{
		Path:   args[1],
		Target: target,
		OnProgress: func(sent, total int64) {
			fmt.Printf("\r  %s / %s  (%d%%)",
				humanBytes(uint64(sent)),
				humanBytes(uint64(total)),
				sent*100/(total+1))
		},
	}
	res, err := a.c.Storage.FileUpload(args[0], opt)
	fmt.Println()
	if err != nil {
		return err
	}
	fmt.Printf("  uploaded %s in %s (%s)\n",
		humanBytes(uint64(res.BytesSent)), res.Elapsed,
		hex.EncodeToString(res.MD5[:]))
	return nil
}

func cmdDownload(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 2 {
		return fmt.Errorf("usage: download <remote> <local>")
	}
	res, err := a.c.Storage.FileDownload(args[0], 0)
	if err != nil {
		return err
	}
	if err := os.WriteFile(args[1], res.Data, 0o644); err != nil {
		return err
	}
	fmt.Printf("  downloaded %s in %s (%s)\n",
		humanBytes(uint64(len(res.Data))), res.Elapsed,
		hex.EncodeToString(res.MD5[:]))
	return nil
}

// ─── Audio ───────────────────────────────────────────────────────────

func cmdPlay(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: play <ch> <path> [vol] [output(1|2|all)]")
	}
	ch, err := parseU8(args[0])
	if err != nil {
		return err
	}
	opt := audio.PlayOptions{Channel: ch, Path: args[1], Volume: 100, Output: audio.OutputAll}
	if len(args) >= 3 {
		v, err := parseU8(args[2])
		if err != nil {
			return err
		}
		opt.Volume = v
	}
	if len(args) >= 4 {
		switch args[3] {
		case "1":
			opt.Output = audio.OutputCh1
		case "2":
			opt.Output = audio.OutputCh2
		case "all":
			opt.Output = audio.OutputAll
		default:
			return fmt.Errorf("output must be 1|2|all")
		}
	}
	return a.c.Audio.Play(opt)
}

func cmdAudioStop(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	ch := audio.ChAll
	if len(args) >= 1 && args[0] != "all" {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		ch = v
	}
	return a.c.Audio.Stop(ch)
}

func cmdVolume(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 2 {
		return fmt.Errorf("usage: volume <ch|master> <0-100>")
	}
	ch := audio.ChAll
	if args[0] != "master" {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		ch = v
	}
	vol, err := parseU8(args[1])
	if err != nil {
		return err
	}
	return a.c.Audio.Volume(ch, vol)
}

func cmdFade(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) != 1 {
		return fmt.Errorf("usage: fade <ch>")
	}
	ch, err := parseU8(args[0])
	if err != nil {
		return err
	}
	return a.c.Audio.Fade(ch)
}

func cmdQueue(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: queue <ch> <path> [now|finish]")
	}
	ch, err := parseU8(args[0])
	if err != nil {
		return err
	}
	opt := audio.QueueOptions{Channel: ch, Path: args[1], Volume: 100, Behavior: audio.QueueFinishLoop}
	if len(args) >= 3 {
		switch args[2] {
		case "now":
			opt.Behavior = audio.QueueStopNow
		case "finish":
			opt.Behavior = audio.QueueFinishLoop
		default:
			return fmt.Errorf("behavior must be now|finish")
		}
	}
	return a.c.Audio.Queue(opt)
}

func cmdQueueClear(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	ch := audio.ChAll
	if len(args) >= 1 && args[0] != "all" {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		ch = v
	}
	return a.c.Audio.QueueClear(ch)
}

func cmdAudioStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Audio.Status()
	if err != nil {
		return err
	}
	fmt.Printf("  %d bytes (raw): %s\n", len(s.Raw), hex.EncodeToString(s.Raw))
	return nil
}

func cmdCodecStatus(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	s, err := a.c.Audio.CodecStatus()
	if err != nil {
		return err
	}
	fmt.Printf("  %d bytes (raw): %s\n", len(s.Raw), hex.EncodeToString(s.Raw))
	return nil
}

// ─── Subscribe ───────────────────────────────────────────────────────

func cmdSubscribe(a *app, _ []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	a.c.Events.OnLog(func(m client.LogMessage) {
		fmt.Printf("[LOG  %s @%dms] %s\n", core.DiagLevelName(m.Level), m.Millis, m.Message)
	})
	a.c.Events.OnExpanderConnected(func(e expp.ConnectedEvent) {
		fmt.Printf("[EXP+] %s addr=%d  VID=%04X PID=%04X\n", e.KindName, e.USBAddr, e.VID, e.PID)
	})
	a.c.Events.OnExpanderIdentified(func(e expp.ExpanderEntry) {
		fmt.Printf("[EXP=] %s addr=%d  guid=%s  %s v%s\n",
			e.KindName, e.USBAddr, e.GUID, e.DeviceName, e.FirmwareVersion)
	})
	a.c.Events.OnExpanderDisconnected(func(e expp.DisconnectedEvent) {
		fmt.Printf("[EXP-] %s addr=%d  guid=%s\n", e.KindName, e.USBAddr, e.GUID)
	})
	a.c.Events.OnExpanderCollision(func(e expp.CollisionEvent) {
		fmt.Printf("[!!! GUID COLLISION] guid=%s  addrA=%d  addrB=%d\n", e.GUID, e.USBAddrA, e.USBAddrB)
	})
	a.c.Events.OnRoleEvent(func(ev topology.RoleEvent) {
		fmt.Printf("[ROLE] guid=%s  inner=%s  payload=%s\n",
			ev.GUID, ev.InnerType, hex.EncodeToString(ev.InnerPayload))
	})
	a.c.Events.OnUploadProgress(func(p storage.UploadProgress) {
		fmt.Printf("[UPLOAD] seg=%d  bytes=%d  ring=%d%%\n", p.SegmentIdx, p.BytesReceived, p.RingFillPct)
	})
	a.c.Events.OnLandingLightPhase(func(ev landing.PhaseChange) {
		fmt.Printf("[LL    ] landing[%d] → %s\n", ev.ID, landing.PhaseName(ev.Phase))
	})
	a.c.Events.OnGearPhase(func(ev gear.PhaseChange) {
		fmt.Printf("[GEAR  ] gear[%d] → %s\n", ev.ID, gear.PhaseName(ev.Phase))
	})
	a.c.Events.OnEngineState(func(ev enginefx.StateChange) {
		fmt.Printf("[ENG   ] engine state → %s\n", enginefx.StateName(ev.State))
	})
	a.c.Events.OnGunShot(func(ev gunfx.Shot) {
		fmt.Printf("[GUN   ] gun[%d] shot fired.\n", ev.ID)
	})
	fmt.Println("subscribing — return to prompt; events stream until disconnect.")
	return nil
}

// ─── Parsing helpers ─────────────────────────────────────────────────

func parseU8(s string) (byte, error) {
	v, err := strconv.ParseUint(strings.TrimPrefix(s, "0x"), 0, 8)
	if err != nil {
		return 0, fmt.Errorf("parse u8 %q: %w", s, err)
	}
	return byte(v), nil
}

func parsePortKind(s string) (byte, error) {
	switch strings.ToLower(s) {
	case "servo":
		return ports.KindServo, nil
	case "pwm":
		return ports.KindPwm, nil
	case "hbridge":
		return ports.KindHBridge, nil
	case "input":
		return ports.KindInput, nil
	}
	return parseU8(s)
}

func parseTarget(s string) (byte, error) {
	switch strings.ToLower(s) {
	case "sd":
		return storage.TargetSD, nil
	case "flash":
		return storage.TargetFlash, nil
	}
	return 0, fmt.Errorf("target must be sd|flash, got %q", s)
}

func printPortRow(label string, descs []ports.PortDescriptor) {
	if len(descs) == 0 {
		return
	}
	fmt.Printf("  %s :", label)
	for _, d := range descs {
		fmt.Printf(" [%d:0x%02X]", d.Index, d.Flags)
	}
	fmt.Println()
}

func humanBytes(n uint64) string {
	switch {
	case n < 1024:
		return fmt.Sprintf("%d B", n)
	case n < 1024*1024:
		return fmt.Sprintf("%.1f KB", float64(n)/1024)
	case n < 1024*1024*1024:
		return fmt.Sprintf("%.1f MB", float64(n)/(1024*1024))
	default:
		return fmt.Sprintf("%.2f GB", float64(n)/(1024*1024*1024))
	}
}
