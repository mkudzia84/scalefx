// Package client is the high-level ScaleFX Go client.  It speaks the
// generic-expander wire protocol (hub + every connected expander,
// addressed by GUID through the hub's TopologyServicePolicy) and
// exposes hub-local subsystems (audio, storage) as separate facets.
//
//	c, err := client.Open("COM5")
//	if err != nil { ... }
//	defer c.Close()
//
//	info, _ := c.Hub.SystemInfo()
//	fmt.Println(info.Hub.DeviceName, len(info.Expanders), "expanders")
//
//	ports, _ := c.Topology.PortList("")      // hub's own ports
//	ports2, _ := c.Topology.PortList("3C4D") // a specific expander
//
// All commands return typed responses or a typed `*ProtocolError`
// when the firmware NACK'd the request.  Async events flow through the
// `Events` facet.
package client

import (
	"errors"
	"fmt"
	"time"

	"scalefx/protocol"
	"scalefx/protocol/core"
)

// DefaultBaud is the baseline ScaleFX wire baud rate.
const DefaultBaud = protocol.DefaultBaud

// Client is the top-level handle.  Sub-facets are exposed as struct
// members so callers write `c.Topology.PortList(...)` rather than
// threading the connection through every call site.
type Client struct {
	conn *protocol.Connection

	Hub           *Hub
	Audio         *Audio
	Storage       *Storage
	Expanders     *Expanders
	Topology      *Topology
	LightFx       *LightFx
	LandingLights *LandingLights
	Gear          *Gear
	Engine        *Engine
	Gun           *Gun
	Alerts        *Alerts
	Events        *Events
}

// Options configures a Client at open time.
type Options struct {
	Baud      int           // 0 → DefaultBaud
	Timeout   time.Duration // 0 → 2 s
	Verbose   bool          // log TX/RX packet types to stdout
}

// Open opens the given serial port and starts the reader goroutine.
// A "tcp://host:port" port name short-circuits to the virtual-board
// TCP transport (same as scalefx-cli accepted in the legacy code).
func Open(portName string) (*Client, error) {
	return OpenWith(portName, Options{})
}

// OpenWith opens the port with explicit options.
func OpenWith(portName string, opts Options) (*Client, error) {
	conn := protocol.NewConnection(portName, opts.Baud, opts.Verbose)
	if opts.Timeout > 0 {
		conn.SetTimeout(opts.Timeout)
	}
	if err := conn.Connect(); err != nil {
		return nil, fmt.Errorf("open %s: %w", portName, err)
	}
	c := &Client{conn: conn}
	c.Hub = &Hub{c: c}
	c.Audio = &Audio{c: c}
	c.Storage = &Storage{c: c}
	c.Expanders = &Expanders{c: c}
	c.Topology = &Topology{c: c}
	c.LightFx = &LightFx{c: c}
	c.LandingLights = &LandingLights{c: c}
	c.Gear = &Gear{c: c}
	c.Engine = &Engine{c: c}
	c.Gun = &Gun{c: c}
	c.Alerts = &Alerts{c: c}
	c.Events = newEvents(c)
	return c, nil
}

// Close releases the serial port and stops the reader goroutine.
func (c *Client) Close() {
	if c == nil || c.conn == nil {
		return
	}
	c.Events.shutdown()
	c.conn.Close()
}

// Conn returns the underlying transport — for advanced callers that need
// to send raw packets or hook the async callback directly.
func (c *Client) Conn() *protocol.Connection { return c.conn }

// PortName returns the serial port the client is connected to.
func (c *Client) PortName() string { return c.conn.PortName() }

// SetVerbose toggles wire-level logging on the underlying connection.
func (c *Client) SetVerbose(v bool) { c.conn.SetVerbose(v) }

// SetTimeout sets the default response timeout for synchronous commands.
func (c *Client) SetTimeout(d time.Duration) { c.conn.SetTimeout(d) }

// ─── Send helpers ─────────────────────────────────────────────────────

// sendExpectACK sends a pre-built packet and decodes the ACK/NACK reply.
func (c *Client) sendExpectACK(packet []byte) error {
	resp, err := c.conn.SendExpectACK(packet)
	if err != nil {
		return err
	}
	return checkResponse(resp)
}

// sendForResp sends a pre-built packet and returns the response packet
// (without forcing ACK/NACK semantics — used for query/response shapes).
// NACK replies are surfaced as `*ProtocolError`.
func (c *Client) sendForResp(packet []byte, expect protocol.PacketType) (*protocol.Response, error) {
	resp, err := c.conn.SendAndWait(packet)
	if err != nil {
		return nil, err
	}
	if resp.IsNACK() {
		return nil, errFromNack(resp)
	}
	if resp.PacketType != expect {
		return nil, fmt.Errorf("expected %s, got %s",
			protocol.PacketTypeName(expect), protocol.PacketTypeName(resp.PacketType))
	}
	return resp, nil
}

// ─── Errors ──────────────────────────────────────────────────────────

// ProtocolError carries a NACK from the firmware.  The error message is
// the symbolic name plus any reason string the firmware appended.
type ProtocolError struct {
	Code    protocol.ErrorCode
	Reason  string // optional inline reason after the error byte
}

func (e *ProtocolError) Error() string {
	name := protocol.ErrorName(e.Code)
	if e.Reason != "" {
		return fmt.Sprintf("%s: %s", name, e.Reason)
	}
	return name
}

// IsCode reports whether err is a *ProtocolError with the given code.
func IsCode(err error, code protocol.ErrorCode) bool {
	var pe *ProtocolError
	if errors.As(err, &pe) {
		return pe.Code == code
	}
	return false
}

func errFromNack(resp *protocol.Response) error {
	if !resp.IsNACK() || len(resp.Payload) == 0 {
		return fmt.Errorf("unexpected non-NACK: %s", protocol.PacketTypeName(resp.PacketType))
	}
	pe := &ProtocolError{Code: protocol.ErrorCode(resp.Payload[0])}
	if len(resp.Payload) > 1 {
		pe.Reason = string(resp.Payload[1:])
	}
	return pe
}

func checkResponse(resp *protocol.Response) error {
	if resp.IsACK() {
		return nil
	}
	if resp.IsNACK() {
		return errFromNack(resp)
	}
	return fmt.Errorf("unexpected response: %s", protocol.PacketTypeName(resp.PacketType))
}

// ─── Port enumeration ────────────────────────────────────────────────

// ListSerialPorts returns the list of attached serial port names.
func ListSerialPorts() []string { return protocol.ListPorts() }

// PortDetail mirrors protocol.PortDetail (re-exported here so callers
// don't need the `protocol` import directly).
type PortDetail = protocol.PortDetail

// ListSerialPortsDetailed returns the detailed port list (VID/PID etc.).
func ListSerialPortsDetailed() []PortDetail { return protocol.ListPortsDetailed() }

// Compile-time reference so vet doesn't drop the core import when this
// file is built in isolation.
var _ = core.Init
