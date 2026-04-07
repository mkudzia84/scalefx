package engine

// ScaleFX Engine - Core Command Handler
// Universal commands available regardless of connected controller type.

import (
	"fmt"
	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"strconv"
)

func (e *Engine) coreCommands() *CmdGroup {
	return &CmdGroup{
		Name:       "Core",
		Controller: "",
		Color:      ColorWhite,
		Commands: map[string]CmdEntry{
			"connect":    {e.cmdConnect, "connect [port] [baud]", "Connect to serial port", false},
			"disconnect": {e.cmdDisconnect, "disconnect", "Disconnect from port", false},
			"reconnect":  {e.cmdReconnect, "reconnect", "Disconnect and reconnect to same port", false},
			"ports":      {e.cmdPorts, "ports", "List available serial ports", false},
			"init":       {e.cmdInit, "init", "Send INIT to controller", false},
			"identify":   {e.cmdIdentify, "identify", "Identify controller (no state change)", false},
			"shutdown":   {e.cmdShutdown, "shutdown", "Send SHUTDOWN to controller", true},
			"status":     {e.cmdStatus, "status", "Request controller status", true},
			"reboot":     {e.cmdReboot, "reboot", "Reboot controller", false},
			"bootsel":    {e.cmdBootsel, "bootsel", "Enter BOOTSEL/DFU mode", false},
			"i2c.scan":   {e.cmdI2CScan, "i2c.scan", "Scan I2C bus for devices", true},
			"diag":       {e.cmdDiagHistory, "diag [count]", "Request diagnostic log history", true},
			"keepalive":  {e.cmdKeepalive, "keepalive", "Send keepalive ping", true},
			"verbose":    {e.cmdVerbose, "verbose [on|off]", "Toggle verbose mode", false},
		},
	}
}

// ─── Core Command Handlers ───

func (e *Engine) cmdConnect(args []string) {
	port := e.Port
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
		// List ports and prompt via callback
		ports := protocol.ListPorts()
		if len(ports) == 0 {
			e.Out.Error("No serial ports found")
			return
		}
		if len(ports) == 1 {
			port = ports[0]
			e.Out.Info("Auto-selecting: %s", port)
		} else if e.PromptSelectPort != nil {
			port = e.PromptSelectPort(ports)
			if port == "" {
				return // cancelled
			}
		} else {
			e.Out.Info("Available ports:")
			for i, p := range ports {
				e.Out.Printf("  %d: %s\n", i+1, p)
			}
			e.Out.Error("No port specified")
			return
		}
	}

	if e.Conn != nil {
		e.StopListenerLoop()
		e.Conn.Close()
	}

	e.Conn = protocol.NewConnection(port, baud, e.Verbose)
	if err := e.Conn.Connect(); err != nil {
		e.Out.Error("Connect failed: %v", err)
		e.Conn = nil
		return
	}
	e.Out.OK("Connected to %s @ %d baud", port, baud)
	e.API = api.NewClient(e.Conn)

	// Drain any pending data
	e.Conn.Drain()

	// Auto-identify
	e.doIdentify()

	// Auto-init for HubFX (already initialized on boot)
	if e.ControllerType == core.CtrlHubFX {
		e.Initialized = true
	}

	e.StartListenerLoop()
}

func (e *Engine) cmdDisconnect(_ []string) {
	if e.Conn == nil {
		e.Out.Warning("Not connected")
		return
	}
	e.StopListenerLoop()
	e.Conn.Close()
	e.Conn = nil
	e.API = nil
	e.ControllerType = ""
	e.Initialized = false
	e.Info = nil
	e.Out.OK("Disconnected")

	if e.OnDisconnect != nil {
		e.OnDisconnect()
	}
}

func (e *Engine) cmdReconnect(_ []string) {
	if e.Conn == nil {
		e.Out.Warning("Not connected — use 'connect' instead")
		return
	}
	port := e.Conn.PortName()
	baud := e.Conn.Baud()
	e.Out.Info("Reconnecting to %s...", port)
	e.cmdDisconnect(nil)
	e.cmdConnect([]string{port, strconv.Itoa(baud)})
}

func (e *Engine) cmdPorts(_ []string) {
	ports := protocol.ListPorts()
	if len(ports) == 0 {
		e.Out.Info("No serial ports found")
		return
	}
	for _, p := range ports {
		e.Out.Printf("  %s\n", p)
	}
}

func (e *Engine) cmdInit(_ []string) {
	if !e.requireConn() {
		return
	}

	r := e.API.Core.Init()
	if r.OK && r.Response != nil && r.Response.IsInitReady() {
		info := ParseInitReady(r.Response.Payload)
		if info != nil {
			e.Info = info
			e.ControllerType = info.ControllerType
			e.Initialized = true
			e.Out.OK("INIT_READY")
			e.PrintInitReadyInfo(info)
		} else {
			e.Initialized = true
			e.Out.OK("INIT_READY (parse error)")
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		e.Out.Error("INIT NACK: %s", r.Response.ErrorMessage())
	} else if r.Error != "" {
		e.Out.Error("INIT failed: %s", r.Error)
	} else {
		e.Out.Warning("Unexpected response: 0x%02X", r.Response.PacketType)
	}
}

func (e *Engine) doIdentify() {
	r := e.API.Core.Identify()
	if r.Error != "" {
		if e.Verbose {
			e.Out.Warning("IDENTIFY failed (%s), trying INIT", r.Error)
		}
		e.cmdInit(nil)
		return
	}

	if r.OK && r.Response != nil && (r.Response.IsIdentify() || r.Response.IsInitReady()) {
		info := ParseInitReady(r.Response.Payload)
		if info != nil {
			e.Info = info
			e.ControllerType = info.ControllerType
			e.Out.OK("Identified: %s v%s (build %d) [%s]",
				info.Name, info.Version, info.Build, info.ControllerType)
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		if e.Verbose {
			e.Out.Warning("IDENTIFY not supported, trying INIT")
		}
		e.cmdInit(nil)
	}
}

func (e *Engine) cmdIdentify(_ []string) {
	if !e.requireConn() {
		return
	}

	r := e.API.Core.Identify()
	if r.OK && r.Response != nil && (r.Response.IsIdentify() || r.Response.IsInitReady()) {
		info := ParseInitReady(r.Response.Payload)
		if info != nil {
			e.Info = info
			e.ControllerType = info.ControllerType
			e.Out.OK("IDENTIFY response")
			e.PrintInitReadyInfo(info)
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		e.Out.Error("IDENTIFY NACK: %s", r.Response.ErrorMessage())
	} else if r.Error != "" {
		e.Out.Error("IDENTIFY failed: %s", r.Error)
	} else {
		e.Out.Warning("Unexpected response: 0x%02X", r.Response.PacketType)
	}
}

func (e *Engine) cmdShutdown(_ []string) {
	e.ack(e.API.Core.Shutdown(), "Shutdown OK")
	e.Initialized = false
}

func (e *Engine) cmdStatus(_ []string) {
	if !e.requireConn() {
		return
	}
	r := e.API.Core.Status()
	if r.OK && r.Response != nil && r.Response.PacketType == core.Status {
		e.Out.OK("STATUS")
		e.ParseStatusPayload(r.Response.Payload)
	} else if r.Error != "" {
		e.Out.Error("STATUS failed: %s", r.Error)
	}
}

func (e *Engine) cmdReboot(_ []string) {
	_ = e.API.Core.Reboot()
	e.Out.OK("Reboot sent")
	e.Initialized = false
}

func (e *Engine) cmdBootsel(_ []string) {
	_ = e.API.Core.Bootsel()
	e.Out.OK("BOOTSEL/DFU sent — device will disconnect")
	e.Initialized = false
}

func (e *Engine) cmdI2CScan(_ []string) {
	if !e.requireConn() {
		return
	}
	r := e.API.Core.I2CScan()
	if r.OK && r.Response != nil {
		if r.Response.PacketType == core.I2cScanRes {
			e.Out.OK("I2C Scan")
			e.ParseI2CScanResult(r.Response.Payload)
		} else if r.Response.IsACK() {
			e.Out.OK("I2C scan ACK (no result packet)")
		}
	} else if r.Error != "" {
		e.Out.Error("I2C scan: %s", r.Error)
	}
}

func (e *Engine) cmdDiagHistory(args []string) {
	count := byte(20)
	if len(args) > 0 {
		if n := Atoi(args[0]); n > 0 && n <= 255 {
			count = byte(n)
		}
	}
	e.ack(e.API.Core.DiagHistory(count), fmt.Sprintf("Requested %d log entries", count))
}

func (e *Engine) cmdKeepalive(_ []string) {
	e.ack(e.API.Core.Keepalive(), "Keepalive OK")
}

func (e *Engine) cmdVerbose(args []string) {
	if len(args) > 0 {
		e.Verbose = ParseBool(args[0])
	} else {
		e.Verbose = !e.Verbose
	}
	if e.Conn != nil {
		e.Conn.SetVerbose(e.Verbose)
	}
	e.Out.Info("Verbose: %s", OnOff(e.Verbose))
}
