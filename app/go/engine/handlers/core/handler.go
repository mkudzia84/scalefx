package core

// ScaleFX Engine - Core Command Handler
// Universal commands available regardless of connected controller type.

import (
	"fmt"
	"scalefx/api"
	"scalefx/engine"
	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	"strconv"
)

// Handler groups all core protocol commands.
type Handler struct {
	E *engine.Engine
}

// Register adds the core command group to the engine.
func Register(eng *engine.Engine) {
	h := &Handler{E: eng}
	eng.AddGroup(h.commands())
}

func (h *Handler) commands() *engine.CmdGroup {
	return &engine.CmdGroup{
		Name:       "Core",
		Controller: "",
		Color:      engine.ColorWhite,
		Commands: map[string]engine.CmdEntry{
			"connect":    {h.cmdConnect, "connect [port] [baud]", "Connect to serial port", false},
			"disconnect": {h.cmdDisconnect, "disconnect", "Disconnect from port", false},
			"reconnect":  {h.cmdReconnect, "reconnect", "Disconnect and reconnect to same port", false},
			"ports":      {h.cmdPorts, "ports", "List available serial ports", false},
			"init":       {h.cmdInit, "init", "Send INIT to controller", false},
			"identify":   {h.cmdIdentify, "identify", "Identify controller (no state change)", false},
			"shutdown":   {h.cmdShutdown, "shutdown", "Send SHUTDOWN to controller", true},
			"status":     {h.cmdStatus, "status", "Request controller status", true},
			"reboot":     {h.cmdReboot, "reboot", "Reboot controller", false},
			"bootsel":    {h.cmdBootsel, "bootsel", "Enter BOOTSEL/DFU mode", false},
			"i2c.scan":   {h.cmdI2CScan, "i2c.scan", "Scan I2C bus for devices", true},
			"diag":       {h.cmdDiagHistory, "diag [count]", "Request diagnostic log history", true},
			"keepalive":  {h.cmdKeepalive, "keepalive", "Send keepalive ping", true},
			"verbose":    {h.cmdVerbose, "verbose [on|off]", "Toggle verbose mode", false},
		},
	}
}

// ─── Core Command Handlers ───

func (h *Handler) cmdConnect(args []string) {
	port := h.E.Port
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
		ports := protocol.ListPorts()
		if len(ports) == 0 {
			h.E.Out.Error("No serial ports found")
			return
		}
		if len(ports) == 1 {
			port = ports[0]
			h.E.Out.Info("Auto-selecting: %s", port)
		} else if h.E.PromptSelectPort != nil {
			port = h.E.PromptSelectPort(ports)
			if port == "" {
				return
			}
		} else {
			h.E.Out.Info("Available ports:")
			for i, p := range ports {
				h.E.Out.Printf("  %d: %s\n", i+1, p)
			}
			h.E.Out.Error("No port specified")
			return
		}
	}

	if h.E.Conn != nil {
		h.E.StopListenerLoop()
		h.E.Conn.Close()
	}

	h.E.Conn = protocol.NewConnection(port, baud, h.E.Verbose)
	if err := h.E.Conn.Connect(); err != nil {
		h.E.Out.Error("Connect failed: %v", err)
		h.E.Conn = nil
		return
	}
	h.E.Out.OK("Connected to %s @ %d baud", port, baud)
	h.E.API = api.NewClient(h.E.Conn)

	h.E.Conn.Drain()
	h.doIdentify()

	if h.E.ControllerType == pcore.CtrlHubFX {
		h.E.Initialized = true
	}

	h.E.StartListenerLoop()
}

func (h *Handler) cmdDisconnect(_ []string) {
	if h.E.Conn == nil {
		h.E.Out.Warning("Not connected")
		return
	}
	h.E.StopListenerLoop()
	h.E.Conn.Close()
	h.E.Conn = nil
	h.E.API = nil
	h.E.ControllerType = ""
	h.E.Initialized = false
	h.E.Info = nil
	h.E.Out.OK("Disconnected")

	if h.E.OnDisconnect != nil {
		h.E.OnDisconnect()
	}
}

func (h *Handler) cmdReconnect(_ []string) {
	if h.E.Conn == nil {
		h.E.Out.Warning("Not connected — use 'connect' instead")
		return
	}
	port := h.E.Conn.PortName()
	baud := h.E.Conn.Baud()
	h.E.Out.Info("Reconnecting to %s...", port)
	h.cmdDisconnect(nil)
	h.cmdConnect([]string{port, strconv.Itoa(baud)})
}

func (h *Handler) cmdPorts(_ []string) {
	ports := protocol.ListPorts()
	if len(ports) == 0 {
		h.E.Out.Info("No serial ports found")
		return
	}
	for _, p := range ports {
		h.E.Out.Printf("  %s\n", p)
	}
}

func (h *Handler) cmdInit(_ []string) {
	if !h.E.RequireConn() {
		return
	}

	r := h.E.API.Core.Init()
	if r.OK && r.Response != nil && r.Response.IsInitReady() {
		info := engine.ParseInitReady(r.Response.Payload)
		if info != nil {
			h.E.Info = info
			h.E.ControllerType = info.ControllerType
			h.E.Initialized = true
			h.E.Out.OK("INIT_READY")
			h.E.PrintInitReadyInfo(info)
		} else {
			h.E.Initialized = true
			h.E.Out.OK("INIT_READY (parse error)")
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		h.E.Out.Error("INIT NACK: %s", r.Response.ErrorMessage())
	} else if r.Error != "" {
		h.E.Out.Error("INIT failed: %s", r.Error)
	} else {
		h.E.Out.Warning("Unexpected response: 0x%02X", r.Response.PacketType)
	}
}

func (h *Handler) doIdentify() {
	r := h.E.API.Core.Identify()
	if r.Error != "" {
		if h.E.Verbose {
			h.E.Out.Warning("IDENTIFY failed (%s), trying INIT", r.Error)
		}
		h.cmdInit(nil)
		return
	}

	if r.OK && r.Response != nil && (r.Response.IsIdentify() || r.Response.IsInitReady()) {
		info := engine.ParseInitReady(r.Response.Payload)
		if info != nil {
			h.E.Info = info
			h.E.ControllerType = info.ControllerType
			h.E.Out.OK("Identified: %s v%s (build %d) [%s]",
				info.Name, info.Version, info.Build, info.ControllerType)
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		if h.E.Verbose {
			h.E.Out.Warning("IDENTIFY not supported, trying INIT")
		}
		h.cmdInit(nil)
	}
}

func (h *Handler) cmdIdentify(_ []string) {
	if !h.E.RequireConn() {
		return
	}

	r := h.E.API.Core.Identify()
	if r.OK && r.Response != nil && (r.Response.IsIdentify() || r.Response.IsInitReady()) {
		info := engine.ParseInitReady(r.Response.Payload)
		if info != nil {
			h.E.Info = info
			h.E.ControllerType = info.ControllerType
			h.E.Out.OK("IDENTIFY response")
			h.E.PrintInitReadyInfo(info)
		}
	} else if r.Response != nil && r.Response.IsNACK() {
		h.E.Out.Error("IDENTIFY NACK: %s", r.Response.ErrorMessage())
	} else if r.Error != "" {
		h.E.Out.Error("IDENTIFY failed: %s", r.Error)
	} else {
		h.E.Out.Warning("Unexpected response: 0x%02X", r.Response.PacketType)
	}
}

func (h *Handler) cmdShutdown(_ []string) {
	h.E.Ack(h.E.API.Core.Shutdown(), "Shutdown OK")
	h.E.Initialized = false
}

func (h *Handler) cmdStatus(_ []string) {
	if !h.E.RequireConn() {
		return
	}
	r := h.E.API.Core.Status()
	if r.OK && r.Response != nil && r.Response.PacketType == pcore.Status {
		h.E.Out.OK("STATUS")
		h.E.ParseStatusPayload(r.Response.Payload)
	} else if r.Error != "" {
		h.E.Out.Error("STATUS failed: %s", r.Error)
	}
}

func (h *Handler) cmdReboot(_ []string) {
	_ = h.E.API.Core.Reboot()
	h.E.Out.OK("Reboot sent")
	h.E.Initialized = false
}

func (h *Handler) cmdBootsel(_ []string) {
	_ = h.E.API.Core.Bootsel()
	h.E.Out.OK("BOOTSEL/DFU sent — device will disconnect")
	h.E.Initialized = false
}

func (h *Handler) cmdI2CScan(_ []string) {
	if !h.E.RequireConn() {
		return
	}
	r := h.E.API.Core.I2CScan()
	if r.OK && r.Response != nil {
		if r.Response.PacketType == pcore.I2cScanRes {
			h.E.Out.OK("I2C Scan")
			h.E.ParseI2CScanResult(r.Response.Payload)
		} else if r.Response.IsACK() {
			h.E.Out.OK("I2C scan ACK (no result packet)")
		}
	} else if r.Error != "" {
		h.E.Out.Error("I2C scan: %s", r.Error)
	}
}

func (h *Handler) cmdDiagHistory(args []string) {
	count := byte(20)
	if len(args) > 0 {
		if n := engine.Atoi(args[0]); n > 0 && n <= 255 {
			count = byte(n)
		}
	}
	h.E.Ack(h.E.API.Core.DiagHistory(count), fmt.Sprintf("Requested %d log entries", count))
}

func (h *Handler) cmdKeepalive(_ []string) {
	h.E.Ack(h.E.API.Core.Keepalive(), "Keepalive OK")
}

func (h *Handler) cmdVerbose(args []string) {
	if len(args) > 0 {
		h.E.Verbose = engine.ParseBool(args[0])
	} else {
		h.E.Verbose = !h.E.Verbose
	}
	if h.E.Conn != nil {
		h.E.Conn.SetVerbose(h.E.Verbose)
	}
	h.E.Out.Info("Verbose: %s", engine.OnOff(h.E.Verbose))
}
