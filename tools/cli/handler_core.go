package main

// ScaleFX CLI - Core Command Handler
// Universal commands available regardless of connected controller type.

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// Note: strconv kept for cmdConnect port selection (needs error check to
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
	baud := DefaultBaud
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
		ports := ListPorts()
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

	c.conn = NewConnection(port, baud, c.verbose)
	if err := c.conn.Connect(); err != nil {
		PrintError("Connect failed: %v", err)
		c.conn = nil
		return
	}
	PrintOK("Connected to %s @ %d baud", port, baud)

	// Drain any pending data
	c.conn.Drain()

	// Auto-identify
	c.doIdentify()

	// Auto-init for HubFX (already initialized on boot)
	if c.controllerType == CtrlHubFX {
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
	port := c.conn.portName
	baud := c.conn.baud
	PrintInfo("Reconnecting to %s...", port)
	c.cmdDisconnect(nil)
	c.cmdConnect([]string{port, strconv.Itoa(baud)})
}

func (c *CLI) cmdPorts(_ []string) {
	ports := ListPorts()
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

	resp, err := c.conn.SendAndWait(CmdInit())
	if err != nil {
		PrintError("INIT failed: %v", err)
		return
	}

	if resp.IsInitReady() {
		info := ParseInitReady(resp.Payload)
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
	} else if resp.IsNACK() {
		PrintError("INIT NACK: %s", resp.ErrorMessage())
	} else {
		PrintWarning("Unexpected response: 0x%02X", resp.PacketType)
	}
}

func (c *CLI) doIdentify() {
	resp, err := c.conn.SendAndWait(CmdIdentify())
	if err != nil {
		if c.verbose {
			PrintWarning("IDENTIFY failed (%v), trying INIT", err)
		}
		// Fallback to INIT
		c.cmdInit(nil)
		return
	}

	if resp.IsIdentify() || resp.IsInitReady() {
		info := ParseInitReady(resp.Payload)
		if info != nil {
			c.info = info
			c.controllerType = info.ControllerType
			PrintOK("Identified: %s v%s (build %d) [%s]",
				info.Name, info.Version, info.Build, info.ControllerType)
		}
	} else if resp.IsNACK() {
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

	resp, err := c.conn.SendAndWait(CmdIdentify())
	if err != nil {
		PrintError("IDENTIFY failed: %v", err)
		return
	}

	if resp.IsIdentify() || resp.IsInitReady() {
		info := ParseInitReady(resp.Payload)
		if info != nil {
			c.info = info
			c.controllerType = info.ControllerType
			PrintOK("IDENTIFY response")
			PrintInitReadyInfo(info)
		}
	} else if resp.IsNACK() {
		PrintError("IDENTIFY NACK: %s", resp.ErrorMessage())
	} else {
		PrintWarning("Unexpected response: 0x%02X", resp.PacketType)
	}
}

func (c *CLI) cmdShutdown(_ []string) {
	c.sendACK(CmdShutdown(), "Shutdown OK")
	c.initialized = false
}

func (c *CLI) cmdStatus(_ []string) {
	if !c.requireConn() {
		return
	}

	resp, err := c.conn.SendAndWait(CmdStatusReq())
	if err != nil {
		PrintError("STATUS failed: %v", err)
		return
	}

	if resp.PacketType == CoreSTATUS {
		PrintOK("STATUS")
		ParseStatusPayload(resp.Payload, c.controllerType)
	} else if resp.IsNACK() {
		PrintError("STATUS NACK: %s", resp.ErrorMessage())
	} else {
		PrintWarning("Unexpected: 0x%02X", resp.PacketType)
	}
}

func (c *CLI) cmdReboot(_ []string) {
	_ = c.conn.Send(CmdReboot())
	PrintOK("Reboot sent")
	c.initialized = false
}

func (c *CLI) cmdBootsel(_ []string) {
	_ = c.conn.Send(CmdBootsel())
	PrintOK("BOOTSEL/DFU sent — device will disconnect")
	c.initialized = false
}

func (c *CLI) cmdI2CScan(_ []string) {
	if !c.requireConn() {
		return
	}

	resp, err := c.conn.SendAndWait(CmdI2CScan())
	if err != nil {
		PrintError("I2C scan failed: %v", err)
		return
	}

	if resp.PacketType == CoreI2C_SCAN_RES {
		PrintOK("I2C Scan")
		ParseI2CScanResult(resp.Payload)
	} else if resp.IsACK() {
		PrintOK("I2C scan ACK (no result packet)")
	} else if resp.IsNACK() {
		PrintError("I2C scan NACK: %s", resp.ErrorMessage())
	}
}

func (c *CLI) cmdDiagHistory(args []string) {
	count := byte(20)
	if len(args) > 0 {
		if n := atoi(args[0]); n > 0 && n <= 255 {
			count = byte(n)
		}
	}
	c.sendACK(CmdDiagHistory(count), fmt.Sprintf("Requested %d log entries", count))
}

func (c *CLI) cmdKeepalive(_ []string) {
	c.sendACK(CmdKeepalive(), "Keepalive OK")
}

func (c *CLI) cmdVerbose(args []string) {
	if len(args) > 0 {
		c.verbose = parseBool(args[0])
	} else {
		c.verbose = !c.verbose
	}
	if c.conn != nil {
		c.conn.verbose = c.verbose
	}
	PrintInfo("Verbose: %s", onOff(c.verbose))
}
