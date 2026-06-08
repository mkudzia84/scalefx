// scalefx-mcp — a Model Context Protocol server that exposes read-only ScaleFX
// device/diagnostic tools over stdio, so an agent (Claude Code, etc.) can
// inspect a connected HubFX without a human driving the CLI/Studio.
//
// Transport: newline-delimited JSON-RPC 2.0 on stdin/stdout (the MCP stdio
// transport). Each tool connects to the board via the shared `client` package
// (same wire path the CLI uses), does its read, and closes — single-threaded,
// no keepalive/live-view hazards (Rules 53-56).
//
// Register with Claude Code (project .mcp.json or user config):
//
//	{ "mcpServers": { "scalefx": { "command": "app/go/scalefx-mcp.exe" } } }
//
// Build: cd app/go && go build -o scalefx-mcp.exe ./mcp/
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"time"

	"scalefx/client"
	"scalefx/firmware"
	"scalefx/protocol/roles"
)

const serverVersion = "0.1.0"
const protocolVersion = "2024-11-05"

// ─── JSON-RPC 2.0 ─────────────────────────────────────────────────────

type rpcReq struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id,omitempty"` // absent for notifications
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}
type rpcResp struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Result  interface{}     `json:"result,omitempty"`
	Error   *rpcErr         `json:"error,omitempty"`
}
type rpcErr struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func main() {
	in := bufio.NewReaderSize(os.Stdin, 1<<20)
	out := bufio.NewWriter(os.Stdout)
	enc := json.NewEncoder(out)

	for {
		line, err := in.ReadBytes('\n')
		if len(line) > 0 {
			handleLine(line, enc, out)
		}
		if err != nil {
			return // EOF / stdin closed
		}
	}
}

func handleLine(line []byte, enc *json.Encoder, out *bufio.Writer) {
	var req rpcReq
	if json.Unmarshal(line, &req) != nil {
		return
	}
	// Notifications (no id) get no response.
	if len(req.ID) == 0 {
		return
	}
	resp := rpcResp{JSONRPC: "2.0", ID: req.ID}
	switch req.Method {
	case "initialize":
		resp.Result = map[string]interface{}{
			"protocolVersion": protocolVersion,
			"capabilities":    map[string]interface{}{"tools": map[string]interface{}{}},
			"serverInfo":      map[string]interface{}{"name": "scalefx-mcp", "version": serverVersion},
		}
	case "tools/list":
		resp.Result = map[string]interface{}{"tools": toolDefs()}
	case "tools/call":
		resp.Result, resp.Error = callTool(req.Params)
	default:
		resp.Error = &rpcErr{Code: -32601, Message: "method not found: " + req.Method}
	}
	_ = enc.Encode(resp)
	_ = out.Flush()
}

// ─── Tools ────────────────────────────────────────────────────────────

func toolDefs() []map[string]interface{} {
	portArg := map[string]interface{}{
		"type": "object",
		"properties": map[string]interface{}{
			"port": map[string]interface{}{"type": "string", "description": "serial port (e.g. COM15); auto-detected if omitted"},
		},
	}
	return []map[string]interface{}{
		{"name": "scalefx_ports", "description": "List detected ScaleFX serial ports (name, VID/PID, description).",
			"inputSchema": map[string]interface{}{"type": "object", "properties": map[string]interface{}{}}},
		{"name": "scalefx_identify", "description": "Connect + IDENTIFY a HubFX: device name, firmware version, build, platform, capabilities. Safe (does not activate hardware).",
			"inputSchema": portArg},
		{"name": "scalefx_device_model", "description": "Connect + read the topology (hub-local + expander ports and their attached roles) as JSON.",
			"inputSchema": portArg},
		{"name": "scalefx_servo_profiles", "description": "Connect + dump the LIVE motion profile (min/max/center/reversed/speed) of every attached hub servo — the fast way to spot a stale/clamped calibration.",
			"inputSchema": portArg},
	}
}

type callParams struct {
	Name      string `json:"name"`
	Arguments struct {
		Port string `json:"port"`
	} `json:"arguments"`
}

func callTool(raw json.RawMessage) (interface{}, *rpcErr) {
	var p callParams
	if json.Unmarshal(raw, &p) != nil {
		return nil, &rpcErr{Code: -32602, Message: "invalid params"}
	}
	var text string
	var err error
	switch p.Name {
	case "scalefx_ports":
		text, err = toolPorts()
	case "scalefx_identify":
		text, err = toolIdentify(p.Arguments.Port)
	case "scalefx_device_model":
		text, err = toolDeviceModel(p.Arguments.Port)
	case "scalefx_servo_profiles":
		text, err = toolServoProfiles(p.Arguments.Port)
	default:
		return nil, &rpcErr{Code: -32602, Message: "unknown tool: " + p.Name}
	}
	if err != nil {
		// MCP convention: tool errors are reported in the result with isError,
		// not as a protocol error, so the model can read the message.
		return toolResult("error: "+err.Error(), true), nil
	}
	return toolResult(text, false), nil
}

func toolResult(text string, isErr bool) map[string]interface{} {
	return map[string]interface{}{
		"content": []map[string]interface{}{{"type": "text", "text": text}},
		"isError": isErr,
	}
}

func asJSON(v interface{}) string {
	b, _ := json.MarshalIndent(v, "", "  ")
	return string(b)
}

// connect resolves the port (arg or auto-detect) and connects.
func connect(port string) (*client.Client, client.Identity, error) {
	if port == "" {
		p, err := firmware.DetectAnyPort()
		if err != nil {
			return nil, client.Identity{}, fmt.Errorf("no port given and auto-detect failed: %w", err)
		}
		port = p
	}
	c, id, err := client.Connect(port, client.Options{Timeout: 3 * time.Second})
	if err != nil {
		return nil, client.Identity{}, fmt.Errorf("connect %s: %w", port, err)
	}
	return c, id, nil
}

func toolPorts() (string, error) {
	ports, err := firmware.ListScaleFXPortsWithIdentity()
	if err != nil || len(ports) == 0 {
		// Fall back to a plain serial enumeration.
		return asJSON(client.ListSerialPortsDetailed()), nil
	}
	return asJSON(ports), nil
}

func toolIdentify(port string) (string, error) {
	c, id, err := connect(port)
	if err != nil {
		return "", err
	}
	defer c.Close()
	return asJSON(id), nil
}

func toolDeviceModel(port string) (string, error) {
	c, _, err := connect(port)
	if err != nil {
		return "", err
	}
	defer c.Close()
	ports, perr := c.Topology.PortList("")
	if perr != nil {
		return "", fmt.Errorf("port list: %w", perr)
	}
	rolesByBoard, _ := c.Topology.RoleList("")
	return asJSON(map[string]interface{}{"ports": ports, "roles": rolesByBoard}), nil
}

func toolServoProfiles(port string) (string, error) {
	c, _, err := connect(port)
	if err != nil {
		return "", err
	}
	defer c.Close()
	boards, lerr := c.Topology.RoleList("")
	if lerr != nil {
		return "", fmt.Errorf("role list: %w", lerr)
	}
	type entry struct {
		PortIdx byte                `json:"portIdx"`
		Profile roles.ServoMotionProfile `json:"profile"`
		Err     string              `json:"error,omitempty"`
	}
	var out []entry
	for _, b := range boards {
		for _, r := range b.Roles {
			if r.RoleKind != roles.KindServoActuator {
				continue
			}
			e := entry{PortIdx: r.PortIdx}
			if p, perr := c.Role("").ServoGetProfile(r.PortIdx); perr != nil {
				e.Err = perr.Error()
			} else {
				e.Profile = p
			}
			out = append(out, e)
		}
	}
	if out == nil {
		return "no servo-actuator roles attached on the hub", nil
	}
	return asJSON(out), nil
}
