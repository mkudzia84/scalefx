package main

// ScaleFX CLI - Core Command Handler
// Universal commands available regardless of connected controller type.

import (
	"bufio"
	"fmt"
	"os"
	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"strconv"
	"strings"
)

// Note: api import kept for api.NewClient in cmdConnect.
// strconv kept for cmdConnect port selection (needs error check to
// distinguish numeric index from port name string).

func (c *CLI) coreCommands() *cmdGroup {
	return &cmdGroup{
		Name:       "Core",
		Controller: "",
		Color:      colorWhite,
		Commands: map[string]cmdEntry{
			"connect":    {c.cmdConnect, "connect [port] [baud]", "Connect to serial port", false},
			"disconnect": {c.cmdDisconnect, "disconnect", "Disconnect from port", false},
			"reconnect":  {c.cmdReconnect, "reconnect", "Disconnect and reconnect to same port", false},
			"ports":      {c.cmdPorts, "ports", "List available serial ports", false},
			"init":       {c.cmdInit, "init", "Send INIT to controller", false},
			"identify":   {c.cmdIdentify, "identify", "Identify controller (no state change)", false},
			"shutdown":   {c.cmdShutdown, "shutdown", "Send SHUTDOWN to controller", true},
			"status":     {c.cmdStatus, "status", "Request controller status", true},
			"reboot":     {c.cmdReboot, "reboot", "Reboot controller", false},
			"bootsel":    {c.cmdBootsel, "bootsel", "Enter BOOTSEL/DFU mode", false},
			"i2c.scan":   {c.cmdI2CScan, "i2c.scan", "Scan I2C bus for devices", true},
			"diag":       {c.cmdDiagHistory, "diag [count]", "Request diagnostic log history", true},
			"keepalive":  {c.cmdKeepalive, "keepalive", "Send keepalive ping", true},
			"verbose":    {c.cmdVerbose, "verbose [on|off]", "Toggle verbose mode", false},
		},
	}
}

// ─── Core Command Handlers ───

func (c *CLI) cmdConnect(args []string) {
	port := c.port
	baud := protocol.DefaultBaud
	if len(args) > 0 {
		port = args[0]
	}
	if len(args) > 1 {
		if b, err := strconv.Atoi(args[1]); err == nil && b > 0 {
			baud = b
		}
	}

	if port == "" {
		// List ports and prompt
		ports := protocol.ListPorts()
		if len(ports) == 0 {
			PrintError("No serial ports found")
			return
		}
		PrintInfo("Available ports:")
		for i, p := range ports {
			fmt.Printf("  %d: %s\n", i+1, p)
		}
		if len(ports) == 1 {
			port = ports[0]
			PrintInfo("Auto-selecting: %s", port)
		} else {
			fmt.Print("Port: ")
			scanner := bufio.NewScanner(os.Stdin)
			if !scanner.Scan() {
				return
			}
			sel := strings.TrimSpace(scanner.Text())
			if idx, err := strconv.Atoi(sel); err == nil && idx >= 1 && idx <= len(ports) {
				port = ports[idx-1]
			} else {
				port = sel
			}
		}
	}

	if c.conn != nil {
		c.stopListenerLoop()
		c.conn.Close()
	}

	c.conn = protocol.NewConnection(port, baud, c.verbose)
	if err := c.conn.Connect(); err != nil {
		PrintError("Connect failed: %v", err)
		c.conn = nil
		return
	}
	PrintOK("Connected to %s @ %d baud", port, baud)
	c.api = api.NewClient(c.conn)

	// Drain any pending data
	c.conn.Drain()

	// Auto-identify
	c.doIdentify()

	// Auto-init for HubFX (already initialized on boot)
	if c.controllerType == core.CtrlHubFX {
		c.initialized = true
	}

	c.startListenerLoop()
}

func (c *CLI) cmdDisconnect(_ []string) {
	if c.conn == nil {
		PrintWarning("Not connected")
		return
	}
	c.stopListenerLoop()
	c.conn.Close()
	c.conn = nil
	c.api = nil
	c.controllerType = ""
	c.initialized = false
	c.info = nil
	PrintOK("Disconnected")
}

func (c *CLI) cmdReconnect(_ []string) {
	if c.conn == nil {
		PrintWarning("Not connected — use 'connect' instead")
		return
	}
	port := c.conn.PortName()
	baud := c.conn.Baud()
	PrintInfo("Reconnecting to %s...", port)
	c.cmdDisconnect(nil)
	c.cmdConnect([]string{port, strconv.Itoa(baud)})
}

func (c *CLI) cmdPorts(_ []string) {
	ports := protocol.ListPorts()
	if len(ports) == 0 {
		PrintInfo("No serial ports found")
		return
	}
	for _, p := range ports {
		fmt.Println("  " + p)
	}
}

func (c *CLI) cmdInit(_ []string) {
	if !c.requireConn() {
		return
	}

	r := c.api.Core.Init()
	if r.OK && r.Response != nil && r.Response.IsInitReady() {
		info := ParseInitReady(r.Response.Payload)
		if info != nil {
			c.info = info
			c.controllerType = info.ControllerType
			c.initialized = true
			PrintOK("INIT_READY")
			PrintInitReadyInfo(info)
		} else {
			c.initialized = true
			PrintOK("INIT_READY (parse error)")
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		PrintError("INIT NACK: %s", r.Response.ErrorMessage())
	} else if r.Error != "" {
		PrintError("INIT failed: %s", r.Error)
	} else {
		PrintWarning("Unexpected response: 0x%02X", r.Response.PacketType)
	}
}

func (c *CLI) doIdentify() {
	r := c.api.Core.Identify()
	if r.Error != "" {
		if c.verbose {
			PrintWarning("IDENTIFY failed (%s), trying INIT", r.Error)
		}
		c.cmdInit(nil)
		return
	}

	if r.OK && r.Response != nil && (r.Response.IsIdentify() || r.Response.IsInitReady()) {
		info := ParseInitReady(r.Response.Payload)
		if info != nil {
			c.info = info
			c.controllerType = info.ControllerType
			PrintOK("Identified: %s v%s (build %d) [%s]",
				info.Name, info.Version, info.Build, info.ControllerType)
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		if c.verbose {
			PrintWarning("IDENTIFY not supported, trying INIT")
		}
		c.cmdInit(nil)
	}
}

func (c *CLI) cmdIdentify(_ []string) {
	if !c.requireConn() {
		return
	}

	r := c.api.Core.Identify()
	if r.OK && r.Response != nil && (r.Response.IsIdentify() || r.Response.IsInitReady()) {
		info := ParseInitReady(r.Response.Payload)
		if info != nil {
			c.info = info
			c.controllerType = info.ControllerType
			PrintOK("IDENTIFY response")
			PrintInitReadyInfo(info)
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		PrintError("IDENTIFY NACK: %s", r.Response.ErrorMessage())
	} else if r.Error != "" {
		PrintError("IDENTIFY failed: %s", r.Error)
	} else {
		PrintWarning("Unexpected response: 0x%02X", r.Response.PacketType)
	}
}

func (c *CLI) cmdShutdown(_ []string) {
	c.ack(c.api.Core.Shutdown(), "Shutdown OK")
	c.initialized = false
}

func (c *CLI) cmdStatus(_ []string) {
	if !c.requireConn() {
		return
	}
	r := c.api.Core.Status()
	if r.OK && r.Response != nil && r.Response.PacketType == core.Status {
		PrintOK("STATUS")
		ParseStatusPayload(r.Response.Payload, c.controllerType)
	} else if r.Error != "" {
		PrintError("STATUS failed: %s", r.Error)
	}
}

func (c *CLI) cmdReboot(_ []string) {
	_ = c.api.Core.Reboot()
	PrintOK("Reboot sent")
	c.initialized = false
}

func (c *CLI) cmdBootsel(_ []string) {
	_ = c.api.Core.Bootsel()
	PrintOK("BOOTSEL/DFU sent — device will disconnect")
	c.initialized = false
}

func (c *CLI) cmdI2CScan(_ []string) {
	if !c.requireConn() {
		return
	}
	r := c.api.Core.I2CScan()
	if r.OK && r.Response != nil {
		if r.Response.PacketType == core.I2cScanRes {
			PrintOK("I2C Scan")
			ParseI2CScanResult(r.Response.Payload)
		} else if r.Response.IsACK() {
			PrintOK("I2C scan ACK (no result packet)")
		}
	} else if r.Error != "" {
		PrintError("I2C scan: %s", r.Error)
	}
}

func (c *CLI) cmdDiagHistory(args []string) {
	count := byte(20)
	if len(args) > 0 {
		if n := atoi(args[0]); n > 0 && n <= 255 {
			count = byte(n)
		}
	}
	c.ack(c.api.Core.DiagHistory(count), fmt.Sprintf("Requested %d log entries", count))
}

func (c *CLI) cmdKeepalive(_ []string) {
	c.ack(c.api.Core.Keepalive(), "Keepalive OK")
}

func (c *CLI) cmdVerbose(args []string) {
	if len(args) > 0 {
		c.verbose = parseBool(args[0])
	} else {
		c.verbose = !c.verbose
	}
	if c.conn != nil {
		c.conn.SetVerbose(c.verbose)
	}
	PrintInfo("Verbose: %s", onOff(c.verbose))
}
