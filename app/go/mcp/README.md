# scalefx-mcp

A [Model Context Protocol](https://modelcontextprotocol.io) server that exposes
**read-only ScaleFX device/diagnostic tools** over stdio, so an agent (Claude
Code, etc.) can inspect a connected HubFX directly — no human driving the
CLI/Studio.

It connects through the same `client` package the CLI uses (single-threaded,
no keepalive/live-view/thread-safety hazards — Rules 53–56). Each tool connects,
does one read, and closes.

## Build

```bash
cd app/go && go build -o scalefx-mcp.exe ./mcp/
```

## Register with Claude Code

Add to a project `.mcp.json` (or your user MCP config):

```json
{
  "mcpServers": {
    "scalefx": { "command": "app/go/scalefx-mcp.exe" }
  }
}
```

Then the tools below are available to the agent.

## Tools

| Tool | Args | Returns |
|------|------|---------|
| `scalefx_ports` | — | Detected ScaleFX serial ports (name, VID/PID, and — if reachable — device name / firmware version / build / capabilities). No board activation. |
| `scalefx_identify` | `port?` | Connect + IDENTIFY: device name, firmware version, build, platform, capabilities. Safe (does not activate hardware). |
| `scalefx_device_model` | `port?` | Connect + topology: every hub-local + expander port and its attached role, as JSON. |
| `scalefx_servo_profiles` | `port?` | Connect + the LIVE motion profile (min/max/center/reversed/speed) of every attached hub servo — the fast way to spot a stale/clamped calibration. |

`port` is auto-detected (CH343 `1A86:55D3`) when omitted.

## Protocol

Newline-delimited JSON-RPC 2.0 on stdin/stdout (the MCP stdio transport).
Handshake: `initialize` → `notifications/initialized` → `tools/list` →
`tools/call`. Tool failures are returned as `isError: true` text content (so the
model can read the message), not as JSON-RPC protocol errors.

## Smoke-test without an agent

```bash
printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"scalefx_ports","arguments":{}}}' \
  | ./scalefx-mcp.exe
```

## Scope

Deliberately **read-only** — inspection/diagnostics for agent-driven testing.
Mutating tools (set profile, deploy, run arbitrary CLI) are intentionally NOT
exposed here; add them behind an explicit opt-in if you want an agent to drive
hardware actions.
