// USB Slave Detection Diagnostic Tool
//
// Connects to HubFX ESP32-S3 via serial and runs a comprehensive
// diagnostic sequence to troubleshoot USB host and slave detection.
//
// Usage:
//   go run . -port COM16            # basic diagnostics
//   go run . -port COM16 -verbose   # with raw packet logging
//   go run . -port COM16 -loop 5    # repeat diagnostics every 5 seconds
//   go run . -port COM16 -reset     # reset USB bus before diagnostics

package main

import (
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	"scalefx/protocol"
	"scalefx/protocol/core"
	"scalefx/protocol/hubfx"
)

// ANSI colors
const (
	cReset  = "\033[0m"
	cRed    = "\033[31m"
	cGreen  = "\033[32m"
	cYellow = "\033[33m"
	cCyan   = "\033[36m"
	cBold   = "\033[1m"
	cDim    = "\033[2m"
)

func ok(s string) string   { return cGreen + s + cReset }
func warn(s string) string { return cYellow + s + cReset }
func fail(s string) string { return cRed + s + cReset }
func head(s string) string { return cBold + cCyan + s + cReset }

// DiagResult collects structured diagnostic output.
type DiagResult struct {
	Section string
	OK      bool
	Lines   []string
}

func (d *DiagResult) add(format string, args ...any) {
	d.Lines = append(d.Lines, fmt.Sprintf(format, args...))
}

func main() {
	port := flag.String("port", "", "Serial port (e.g., COM16)")
	verbose := flag.Bool("verbose", false, "Enable packet-level logging")
	doReset := flag.Bool("reset", false, "Reset USB bus before diagnostics")
	loopSec := flag.Int("loop", 0, "Repeat diagnostics every N seconds (0=once)")
	baud := flag.Int("baud", 6000000, "Baud rate")
	timeoutSec := flag.Int("timeout", 5, "Response timeout in seconds")
	flag.Parse()

	if *port == "" {
		fmt.Println("Available serial ports:")
		ports := protocol.ListPortsDetailed()
		if len(ports) == 0 {
			fmt.Println("  (none)")
		}
		for _, p := range ports {
			desc := ""
			if p.Description != "" {
				desc = " (" + p.Description + ")"
			}
			fmt.Printf("  %s%s\n", p.Name, desc)
		}
		fmt.Println("\nUsage: go run . -port COMx")
		os.Exit(1)
	}

	// Run once or in a loop
	for {
		runDiagnostics(*port, *baud, *verbose, *doReset, *timeoutSec)
		if *loopSec <= 0 {
			break
		}
		fmt.Printf("\n%s Waiting %ds before next scan...\n", cDim+"───"+cReset, *loopSec)
		time.Sleep(time.Duration(*loopSec) * time.Second)
	}
}

func runDiagnostics(portName string, baud int, verbose bool, doReset bool, timeoutSec int) {
	fmt.Printf("\n%s\n", head("═══════════════════════════════════════════"))
	fmt.Printf("%s\n", head("  ScaleFX USB Slave Detection Diagnostics  "))
	fmt.Printf("%s\n\n", head("═══════════════════════════════════════════"))

	// ── Step 1: Connect ──
	fmt.Printf("%s Connecting to %s @ %d baud (timeout=%ds)...\n", head("▶"), portName, baud, timeoutSec)
	conn := protocol.NewConnection(portName, baud, verbose)
	conn.SetTimeout(time.Duration(timeoutSec) * time.Second)
	if err := conn.Connect(); err != nil {
		fmt.Printf("  %s %s\n", fail("FAIL"), err)
		return
	}
	defer conn.Close()

	// Capture async log messages during diagnostics
	logMessages := make([]string, 0)
	conn.SetCallback(func(resp *protocol.Response) {
		if resp.PacketType == protocol.PacketType(core.LogMessage) && len(resp.Payload) >= 5 {
			level := resp.Payload[0]
			ts := protocol.ReadU32LE(resp.Payload, 1)
			msg := string(resp.Payload[5:])
			levelNames := map[byte]string{0: "DBG", 1: "INF", 2: "WRN", 3: "ERR"}
			ln := levelNames[level]
			if ln == "" {
				ln = fmt.Sprintf("L%d", level)
			}
			logLine := fmt.Sprintf("[%6d.%03d] %-3s %s", ts/1000, ts%1000, ln, msg)
			logMessages = append(logMessages, logLine)
		}
	})

	fmt.Printf("  %s Connected\n", ok("OK"))

	// ── Step 2: Identify ──
	printPhase("Identifying controller")
	identResult := identify(conn)
	printDiag(identResult)
	if !identResult.OK {
		fmt.Printf("\n  %s Cannot continue — not a HubFX controller\n", fail("ABORT"))
		return
	}

	// ── Step 3: Status ──
	printPhase("Querying HubFX status")
	statusResult := queryStatus(conn)
	printDiag(statusResult)

	// ── Step 4: USB bus reset (optional) ──
	if doReset {
		printPhase("Resetting USB bus")
		resetResult := resetUsb(conn)
		printDiag(resetResult)
		// Wait for bus to stabilize after reset
		fmt.Printf("  Waiting 8s for USB bus to stabilize...\n")
		time.Sleep(8 * time.Second)
	}

	// ── Step 5: USB devices ──
	printPhase("Enumerating USB devices")
	usbResult := queryUsbDevices(conn)
	printDiag(usbResult)

	// ── Step 6: Slave list ──
	printPhase("Querying slave list")
	slaveResult := querySlaveList(conn)
	printDiag(slaveResult)

	// ── Step 7: Slave info per type ──
	for _, st := range []struct {
		name  string
		stype byte
	}{
		{"GunFX", hubfx.SlaveTypeGunFx},
		{"LightFX", hubfx.SlaveTypeLightFx},
		{"GearControl", hubfx.SlaveTypeGearControl},
	} {
		printPhase(fmt.Sprintf("Querying %s slave info", st.name))
		infoResult := querySlaveInfo(conn, st.stype, st.name)
		printDiag(infoResult)
	}

	// ── Step 8: Second status (check if slaves changed after queries) ──
	printPhase("Final status check")
	finalStatus := queryStatus(conn)
	printDiag(finalStatus)

	// ── Log messages collected ──
	if len(logMessages) > 0 {
		fmt.Printf("\n%s\n", head("── Diagnostic Log Messages (async) ──"))
		for _, line := range logMessages {
			color := cDim
			if strings.Contains(line, "ERR") {
				color = cRed
			} else if strings.Contains(line, "WRN") {
				color = cYellow
			}
			fmt.Printf("  %s%s%s\n", color, line, cReset)
		}
	}

	fmt.Printf("\n%s\n", head("═══ Diagnostics complete ═══"))
}

func printPhase(name string) {
	fmt.Printf("\n%s %s\n", head("▶"), name)
}

func printDiag(d DiagResult) {
	for _, line := range d.Lines {
		fmt.Printf("  %s\n", line)
	}
}

// ─── Diagnostic Steps ───

func identify(conn *protocol.Connection) DiagResult {
	d := DiagResult{Section: "identify"}

	resp, err := conn.SendAndWait(core.CmdIdentify())
	if err != nil {
		d.add("%s IDENTIFY failed: %s", fail("✗"), err)

		// Fallback: try INIT
		d.add("  Trying INIT as fallback...")
		resp, err = conn.SendAndWait(core.CmdInit())
		if err != nil {
			d.add("%s INIT also failed: %s", fail("✗"), err)
			return d
		}
	}

	if resp.IsNACK() {
		d.add("%s NACK: %s", fail("✗"), resp.ErrorMessage())
		return d
	}

	if !resp.IsIdentify() && !resp.IsInitReady() {
		d.add("%s Unexpected response type: 0x%02X", fail("✗"), resp.PacketType)
		return d
	}

	info := parseInitReady(resp.Payload)
	if info == nil {
		d.add("%s Failed to parse INIT_READY payload", fail("✗"))
		return d
	}

	d.add("%s %s v%s (build %d)", ok("✓"), info.Name, info.Version, info.Build)
	d.add("  Platform: %s @ %d MHz, Free RAM: %d bytes", info.Platform, info.CPUMHz, info.FreeRAM)

	if !strings.HasPrefix(info.Name, "HubFX") {
		d.add("%s Not a HubFX controller — slave diagnostics won't work", warn("!"))
		return d
	}

	d.OK = true
	return d
}

func queryStatus(conn *protocol.Connection) DiagResult {
	d := DiagResult{Section: "status"}

	resp, err := conn.SendAndWait(core.CmdStatusReq())
	if err != nil {
		d.add("%s STATUS failed: %s", fail("✗"), err)
		return d
	}
	if resp.IsNACK() {
		d.add("%s NACK: %s", fail("✗"), resp.ErrorMessage())
		return d
	}

	payload := resp.Payload
	// Core header: 20 bytes [counter:4][uptime:4][freeRam:4][lastActivity:4][keepalives:4]
	if len(payload) < 20 {
		d.add("%s STATUS payload too short (%d bytes)", fail("✗"), len(payload))
		return d
	}

	counter := protocol.ReadU32LE(payload, 0)
	uptime := protocol.ReadU32LE(payload, 4)
	freeRam := protocol.ReadU32LE(payload, 8)
	lastActivity := protocol.ReadU32LE(payload, 12)
	keepalives := protocol.ReadU32LE(payload, 16)

	d.add("%s Core status:", ok("✓"))
	d.add("  Counter:       %d", counter)
	d.add("  Uptime:        %ds", uptime/1000)
	d.add("  Free RAM:      %d bytes", freeRam)
	d.add("  Last Activity: %dms ago", lastActivity)
	d.add("  Keepalives:    %d", keepalives)

	// Module-specific data starts at byte 20
	modData := payload[20:]
	if len(modData) >= 2 {
		flags := modData[0]
		slaveMask := modData[1]

		core1Ready := flags&0x01 != 0
		audioInit := flags&0x02 != 0
		flashReady := flags&0x04 != 0
		usbReady := flags&0x08 != 0
		sdReady := flags&0x10 != 0

		d.add("")
		d.add("  HubFX Flags: 0x%02X", flags)
		d.add("    Core 1:    %s", boolStatus(core1Ready))
		d.add("    Audio:     %s", boolStatus(audioInit))
		d.add("    Flash:     %s", boolStatus(flashReady))
		d.add("    SD Card:   %s", boolStatus(sdReady))
		d.add("    USB Host:  %s", boolStatus(usbReady))

		d.add("")
		d.add("  Slave Mask: 0x%02X", slaveMask)
		slaveNames := []string{"GunFX", "LightFX", "GearControl"}
		anyReady := false
		for i, name := range slaveNames {
			ready := slaveMask&(1<<i) != 0
			d.add("    %-12s %s", name+":", boolStatus(ready))
			if ready {
				anyReady = true
			}
		}
		if !anyReady {
			d.add("  %s No slaves detected in status", warn("!"))
		}
	}

	if len(modData) >= 6 {
		loop1Count := protocol.ReadU32LE(modData, 2)
		d.add("  Core 1 iterations: %d", loop1Count)
	}

	d.OK = true
	return d
}

func resetUsb(conn *protocol.Connection) DiagResult {
	d := DiagResult{Section: "usb_reset"}

	resp, err := conn.SendExpectACK(hubfx.CmdUsbResetBus())
	if err != nil {
		d.add("%s USB reset failed: %s", fail("✗"), err)
		return d
	}
	if resp.IsACK() {
		d.add("%s USB bus reset sent", ok("✓"))
		d.OK = true
	} else {
		d.add("%s USB reset NACK: %s", fail("✗"), resp.ErrorMessage())
	}
	return d
}

func queryUsbDevices(conn *protocol.Connection) DiagResult {
	d := DiagResult{Section: "usb_devices"}

	resp, err := conn.SendAndWait(hubfx.CmdUsbDevicesReq())
	if err != nil {
		d.add("%s USB_DEVICES_REQ failed: %s", fail("✗"), err)
		return d
	}
	if resp.IsNACK() {
		d.add("%s NACK: %s", fail("✗"), resp.ErrorMessage())
		return d
	}

	payload := resp.Payload
	if len(payload) < 4 {
		d.add("%s USB_DEVICES_RESP too short (%d bytes)", fail("✗"), len(payload))
		return d
	}

	pos := 0
	initialized := payload[pos] != 0; pos++
	taskRunning := payload[pos] != 0; pos++
	backendLen := int(payload[pos]); pos++

	if pos+backendLen > len(payload) {
		d.add("%s Malformed response (backend length overflow)", fail("✗"))
		return d
	}
	backend := string(payload[pos : pos+backendLen]); pos += backendLen

	if pos >= len(payload) {
		d.add("%s Malformed response (no device count)", fail("✗"))
		return d
	}
	deviceCount := int(payload[pos]); pos++

	d.add("%s USB Host: %s", ok("✓"), backend)
	d.add("  Initialized: %s", boolStatus(initialized))
	d.add("  Task:        %s", boolStatus(taskRunning))
	d.add("  CDC Devices: %d", deviceCount)

	if !initialized {
		d.add("  %s USB host not initialized — check hardware/firmware", warn("!"))
	}
	if !taskRunning {
		d.add("  %s CDC task not running — USB host may have crashed", fail("!"))
	}

	if deviceCount == 0 {
		d.add("")
		d.add("  %s No USB devices detected", warn("!"))
		d.add("  Possible causes:")
		d.add("    • No Pico controllers physically connected")
		d.add("    • USB cables are data-only or damaged")
		d.add("    • Pico controllers not powered on")
		d.add("    • USB hub/OTG adapter issue")
		d.add("    • Try: -reset flag to power-cycle the USB bus")
		d.OK = true  // not a protocol error, just no devices
		return d
	}

	stateNames := map[byte]string{0: "Disconnected", 1: "Connected", 2: "Mounted", 3: "Ready"}
	knownPIDs := map[uint16]string{
		0x0180: "GunFX",
		0x0181: "LightFX",
		0x0182: "GearControl",
		0x000A: "Pico-Default",
	}

	d.add("")
	for i := 0; i < deviceCount; i++ {
		if pos+7 > len(payload) {
			d.add("  [%d] %s truncated at device %d", i, fail("✗"), i)
			break
		}
		addr := payload[pos]; pos++
		vid := protocol.ReadU16LE(payload, pos); pos += 2
		pid := protocol.ReadU16LE(payload, pos); pos += 2
		state := payload[pos]; pos++
		slaveType := payload[pos]; pos++

		stateText := stateNames[state]
		if stateText == "" {
			stateText = fmt.Sprintf("Unknown(0x%02X)", state)
		}
		stateColor := fail(stateText)
		if state == 3 {
			stateColor = ok(stateText)
		} else if state >= 1 {
			stateColor = warn(stateText)
		}

		pidName := ""
		if name, exists := knownPIDs[pid]; exists {
			pidName = " [" + name + "]"
		}

		slaveText := ""
		if slaveType > 0 {
			slaveText = " → " + hubfx.SlaveTypeName(slaveType)
		}

		d.add("  [%d] addr=%d VID=%04X PID=%04X%s  %s%s",
			i, addr, vid, pid, pidName, stateColor, slaveText)

		// Diagnostic hints for this device
		if vid == 0x2E8A && state < 2 {
			d.add("      %s Raspberry Pi Pico detected but not mounted — CDC-ACM open may have failed", warn("!"))
		}
		if pid == 0x000A {
			d.add("      %s Default PID — tusb_config.h custom PID may not be effective", warn("!"))
		}
		if state == 2 && slaveType == 0 {
			d.add("      %s Mounted but no slave type assigned — INIT handshake may have failed", warn("!"))
		}
	}

	d.OK = true
	return d
}

func querySlaveList(conn *protocol.Connection) DiagResult {
	d := DiagResult{Section: "slave_list"}

	resp, err := conn.SendAndWait(hubfx.CmdSlaveList())
	if err != nil {
		d.add("%s SLAVE_LIST failed: %s", fail("✗"), err)
		return d
	}
	if resp.IsNACK() {
		d.add("%s NACK: %s", fail("✗"), resp.ErrorMessage())
		return d
	}
	if resp.PacketType != hubfx.SlaveListResp {
		d.add("%s Unexpected response type: 0x%02X (expected SLAVE_LIST_RESP=0x81)",
			fail("✗"), resp.PacketType)
		return d
	}

	payload := resp.Payload
	if len(payload) < 1 {
		d.add("%s Empty slave list response", warn("!"))
		d.OK = true
		return d
	}

	count := payload[0]
	d.add("%s Registered slaves: %d", ok("✓"), count)

	if count == 0 {
		d.add("  %s No slaves in registry — SlaveManager may not have registered descriptors", warn("!"))
		d.OK = true
		return d
	}

	pos := 1
	for i := 0; i < int(count); i++ {
		if pos+4 > len(payload) {
			d.add("  [%d] %s truncated", i, fail("✗"))
			break
		}
		stype := payload[pos]
		connected := payload[pos+1] != 0
		ready := payload[pos+2] != 0
		nameLen := int(payload[pos+3])
		pos += 4

		name := ""
		if nameLen > 0 && pos+nameLen <= len(payload) {
			name = string(payload[pos : pos+nameLen])
			pos += nameLen
		}

		typeName := hubfx.SlaveTypeName(stype)
		statusText := fail("disconnected")
		if ready {
			statusText = ok("ready")
		} else if connected {
			statusText = warn("connected (not initialized)")
		}

		displayName := ""
		if name != "" {
			displayName = fmt.Sprintf(" (%s)", name)
		}
		d.add("  [%d] %-12s%s: %s", i, typeName, displayName, statusText)

		// Diagnostic hints
		if connected && !ready {
			d.add("      %s USB link up but INIT handshake failed — check slave firmware", warn("!"))
		}
		if !connected {
			d.add("      %s No USB connection — check cable and power", warn("!"))
		}
	}

	d.OK = true
	return d
}

func querySlaveInfo(conn *protocol.Connection, slaveType byte, name string) DiagResult {
	d := DiagResult{Section: "slave_info_" + name}

	resp, err := conn.SendAndWait(hubfx.CmdSlaveInfo(slaveType))
	if err != nil {
		d.add("%s SLAVE_INFO(%s) failed: %s", fail("✗"), name, err)
		return d
	}
	if resp.IsNACK() {
		errCode := resp.ErrorCode()
		errMsg := resp.ErrorMessage()
		switch errCode {
		case protocol.ErrorCode(hubfx.ErrSlaveNotFound):
			d.add("%s %s: not registered in SlaveManager", warn("—"), name)
		case protocol.ErrorCode(hubfx.ErrSlaveNotConnected):
			d.add("%s %s: registered but no USB connection", warn("—"), name)
		default:
			d.add("%s %s: NACK %s", fail("✗"), name, errMsg)
		}
		d.OK = true // Expected for disconnected slaves
		return d
	}
	if resp.PacketType != hubfx.SlaveInfoResp {
		d.add("%s Unexpected response type: 0x%02X", fail("✗"), resp.PacketType)
		return d
	}

	payload := resp.Payload
	if len(payload) < 3 {
		d.add("%s %s: response too short (%d bytes)", fail("✗"), name, len(payload))
		return d
	}

	pos := 0
	stype := payload[pos]; pos++
	ready := payload[pos] != 0; pos++
	connected := payload[pos] != 0; pos++

	readStr := func() string {
		if pos >= len(payload) {
			return ""
		}
		slen := int(payload[pos]); pos++
		if slen == 0 || pos+slen > len(payload) {
			return ""
		}
		s := string(payload[pos : pos+slen]); pos += slen
		return s
	}

	boardName := readStr()
	version := readStr()
	platform := readStr()

	var cpuMHz, freeRAM, buildNum uint32
	if pos+4 <= len(payload) {
		cpuMHz = protocol.ReadU32LE(payload, pos); pos += 4
	}
	if pos+4 <= len(payload) {
		freeRAM = protocol.ReadU32LE(payload, pos); pos += 4
	}
	if pos+4 <= len(payload) {
		buildNum = protocol.ReadU32LE(payload, pos)
	}

	_ = stype // already known from input

	statusText := fail("disconnected")
	if ready {
		statusText = ok("ready")
	} else if connected {
		statusText = warn("connected")
	}

	d.add("%s %s: %s", ok("✓"), name, statusText)
	if boardName != "" {
		d.add("  Name:     %s", boardName)
	}
	if version != "" {
		d.add("  Version:  %s (build %d)", version, buildNum)
	}
	if platform != "" {
		d.add("  Platform: %s @ %d MHz", platform, cpuMHz)
	}
	if freeRAM > 0 {
		d.add("  Free RAM: %d bytes", freeRAM)
	}

	d.OK = true
	return d
}

// ─── Helpers ───

type initReadyInfo struct {
	Name     string
	Version  string
	Platform string
	CPUMHz   uint32
	FreeRAM  uint32
	Build    uint32
}

func parseInitReady(payload []byte) *initReadyInfo {
	if len(payload) < 3 {
		return nil
	}
	info := &initReadyInfo{}
	pos := 0

	readStr := func() string {
		if pos >= len(payload) {
			return ""
		}
		slen := int(payload[pos]); pos++
		if slen == 0 || pos+slen > len(payload) {
			return ""
		}
		s := string(payload[pos : pos+slen]); pos += slen
		return s
	}

	info.Name = readStr()
	info.Version = readStr()
	info.Platform = readStr()

	if pos+12 > len(payload) {
		return info
	}
	info.CPUMHz = protocol.ReadU32LE(payload, pos); pos += 4
	info.FreeRAM = protocol.ReadU32LE(payload, pos); pos += 4
	info.Build = protocol.ReadU32LE(payload, pos)
	return info
}

func boolStatus(v bool) string {
	if v {
		return ok("YES")
	}
	return fail("NO")
}
