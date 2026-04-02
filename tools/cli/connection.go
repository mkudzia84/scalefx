package main

// ScaleFX CLI - Serial Connection
// Manages USB serial communication with ScaleFX controllers.
// COBS packet framing, tag-correlated send/receive via single reader goroutine.

import (
	"bytes"
	"fmt"
	"sync"
	"time"

	"go.bug.st/serial"
)

const (
	DefaultBaud    = 6000000
	DefaultTimeout = 2 * time.Second
	TagAsync       = 0x00
)

// Response represents a parsed packet from the controller.
type Response struct {
	PacketType byte
	Tag        byte
	Payload    []byte
	Raw        []byte
}

func (r *Response) IsACK() bool       { return r.PacketType == CoreACK }
func (r *Response) IsNACK() bool      { return r.PacketType == CoreNACK }
func (r *Response) IsInitReady() bool  { return r.PacketType == CoreINIT_READY }
func (r *Response) IsIdentify() bool   { return r.PacketType == CoreIDENTIFY }

func (r *Response) ErrorCode() byte {
	if r.IsNACK() && len(r.Payload) > 0 {
		return r.Payload[0]
	}
	return 0
}

func (r *Response) ErrorMessage() string {
	code := r.ErrorCode()
	if code != 0 {
		name := ErrorName(code)
		if r.IsNACK() && len(r.Payload) > 1 {
			return fmt.Sprintf("%s: %s", name, string(r.Payload[1:]))
		}
		return name
	}
	return ""
}

// AsyncCallback is called for unsolicited async packets (tag=0) or unmatched responses.
type AsyncCallback func(resp *Response)

// Connection manages serial communication with a ScaleFX controller.
// A single reader goroutine reads all packets from the serial port and
// dispatches them: tagged responses go to registered waiters via channels,
// async/unmatched packets go to the async callback.
type Connection struct {
	port     serial.Port
	portName string
	baud     int
	timeout  time.Duration
	verbose  bool

	writeMu sync.Mutex // protects serial writes
	nextTag byte

	// Reader goroutine → waiter delivery
	waiterMu      sync.Mutex
	tagWaiters    map[byte]chan *Response
	streamWaiters map[byte]chan *Response // multi-response streams

	asyncCB AsyncCallback

	readerStop chan struct{}
	readerDone chan struct{}
}

// NewConnection creates a new connection instance.
func NewConnection(portName string, baud int, verbose bool) *Connection {
	if baud == 0 {
		baud = DefaultBaud
	}
	return &Connection{
		portName:      portName,
		baud:          baud,
		timeout:       DefaultTimeout,
		verbose:       verbose,
		nextTag:       1,
		tagWaiters:    make(map[byte]chan *Response),
		streamWaiters: make(map[byte]chan *Response),
	}
}

// Connect opens the serial port and starts the reader goroutine.
func (c *Connection) Connect() error {
	mode := &serial.Mode{
		BaudRate: c.baud,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}

	port, err := serial.Open(c.portName, mode)
	if err != nil {
		return fmt.Errorf("failed to open %s: %w", c.portName, err)
	}

	port.SetReadTimeout(100 * time.Millisecond)

	c.port = port
	c.tagWaiters = make(map[byte]chan *Response)
	c.streamWaiters = make(map[byte]chan *Response)

	// Wait for device to settle, drain boot output
	time.Sleep(500 * time.Millisecond)
	c.drain()

	// Start the reader goroutine
	c.startReader()

	return nil
}

// IsConnected returns true if serial port is open.
func (c *Connection) IsConnected() bool {
	return c.port != nil
}

// Close stops the reader and closes the serial port.
func (c *Connection) Close() {
	c.stopReader()
	if c.port != nil {
		c.port.Close()
		c.port = nil
	}
	c.waiterMu.Lock()
	c.tagWaiters = make(map[byte]chan *Response)
	c.streamWaiters = make(map[byte]chan *Response)
	c.waiterMu.Unlock()
}

// Reconnect closes and reopens the serial port.
func (c *Connection) Reconnect() error {
	c.Close()
	time.Sleep(300 * time.Millisecond)
	return c.Connect()
}

// SetCallback sets the async packet callback.
func (c *Connection) SetCallback(cb AsyncCallback) {
	c.asyncCB = cb
}

// NextTag returns the next correlation tag (1-255).
func (c *Connection) NextTag() byte {
	tag := c.nextTag
	c.nextTag++
	if c.nextTag == 0 {
		c.nextTag = 1
	}
	return tag
}

// Send writes raw bytes to the serial port.
func (c *Connection) Send(data []byte) error {
	if c.port == nil {
		return fmt.Errorf("not connected")
	}

	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	if c.verbose {
		ptype, tag, payload, ok := ParsePacket(data)
		if ok {
			name := PacketTypeName(ptype)
			fmt.Printf("  → TX: %s tag=%d [%d bytes]\n", name, tag, len(payload))
		}
	}

	_, err := c.port.Write(data)
	return err
}

// SendAndWait sends a packet with auto-assigned tag and waits for the matching response.
// The single reader goroutine delivers tagged responses via channels.
func (c *Connection) SendAndWait(data []byte) (*Response, error) {
	if c.port == nil {
		return nil, fmt.Errorf("not connected")
	}

	tag := c.NextTag()
	tagged := c.injectTag(data, tag)

	// Register a waiter channel for this tag
	ch := make(chan *Response, 1)
	c.waiterMu.Lock()
	c.tagWaiters[tag] = ch
	c.waiterMu.Unlock()

	if err := c.Send(tagged); err != nil {
		c.waiterMu.Lock()
		delete(c.tagWaiters, tag)
		c.waiterMu.Unlock()
		return nil, err
	}

	// Wait for the reader goroutine to deliver the response
	select {
	case resp := <-ch:
		return resp, nil
	case <-time.After(c.timeout):
		c.waiterMu.Lock()
		delete(c.tagWaiters, tag)
		c.waiterMu.Unlock()
		return nil, fmt.Errorf("timeout waiting for response")
	}
}

// SendExpectACK sends a packet and expects ACK/NACK response.
func (c *Connection) SendExpectACK(data []byte) (*Response, error) {
	return c.SendAndWait(data)
}

// StreamResult holds the reassembled data from a streamed response.
type StreamResult struct {
	Data       []byte
	TotalSegs  uint16
	TotalBytes uint32
	CRC        uint16
}

// SendAndReceiveStream sends a packet that triggers a streaming response
// (STREAM_BEGIN → STREAM_DATA × N → STREAM_END) and collects all data.
func (c *Connection) SendAndReceiveStream(data []byte, timeout time.Duration) (*StreamResult, error) {
	if c.port == nil {
		return nil, fmt.Errorf("not connected")
	}

	tag := c.NextTag()
	tagged := c.injectTag(data, tag)

	// Register a multi-response waiter channel (large buffer for stream chunks)
	ch := make(chan *Response, 512)
	c.waiterMu.Lock()
	c.streamWaiters[tag] = ch
	c.waiterMu.Unlock()

	defer func() {
		c.waiterMu.Lock()
		delete(c.streamWaiters, tag)
		c.waiterMu.Unlock()
	}()

	if err := c.Send(tagged); err != nil {
		return nil, err
	}

	var result StreamResult
	deadline := time.After(timeout)

	for {
		select {
		case resp := <-ch:
			if resp.IsNACK() {
				return nil, fmt.Errorf("NACK: %s", resp.ErrorMessage())
			}

			switch resp.PacketType {
			case StreamBEGIN:
				if len(resp.Payload) >= 4 {
					result.TotalBytes = ReadU32LE(resp.Payload, 0)
				}

			case StreamDATA:
				if len(resp.Payload) >= 4 {
					// [seq:u16][crc:u16][data...]
					chunk := resp.Payload[4:]
					result.Data = append(result.Data, chunk...)
				}

			case StreamEND:
				if len(resp.Payload) >= 8 {
					result.TotalSegs = ReadU16LE(resp.Payload, 0)
					result.TotalBytes = ReadU32LE(resp.Payload, 2)
					result.CRC = ReadU16LE(resp.Payload, 6)
				}
				return &result, nil
			}

		case <-deadline:
			return nil, fmt.Errorf("stream timeout")
		}
	}
}

// drain discards pending serial data (called before reader starts).
func (c *Connection) drain() {
	if c.port == nil {
		return
	}
	buf := make([]byte, 4096)
	for {
		n, err := c.port.Read(buf)
		if n == 0 || err != nil {
			break
		}
	}
}

// Drain clears pending tag waiters.
func (c *Connection) Drain() {
	c.waiterMu.Lock()
	c.tagWaiters = make(map[byte]chan *Response)
	c.streamWaiters = make(map[byte]chan *Response)
	c.waiterMu.Unlock()
}

// ─── Single Reader Goroutine ───

// startReader launches the persistent reader goroutine.
// It is the ONLY goroutine that reads from the serial port.
func (c *Connection) startReader() {
	c.readerStop = make(chan struct{})
	c.readerDone = make(chan struct{})

	go func() {
		defer close(c.readerDone)

		rxBuf := make([]byte, 0, 8192) // local to this goroutine — no races
		readBuf := make([]byte, 4096)

		for {
			select {
			case <-c.readerStop:
				return
			default:
			}

			// Read available data from serial (100ms timeout set on port)
			n, _ := c.port.Read(readBuf)
			if n > 0 {
				rxBuf = append(rxBuf, readBuf[:n]...)
			}

			// Extract and dispatch complete packets (0x00 delimited)
			for {
				idx := bytes.IndexByte(rxBuf, 0x00)
				if idx < 0 {
					break
				}

				if idx > 0 {
					// We have packet data before the delimiter
					packetData := make([]byte, idx+1)
					copy(packetData, rxBuf[:idx])
					packetData[idx] = 0x00 // append delimiter for ParsePacket

					ptype, tag, payload, ok := ParsePacket(packetData)
					if ok {
						resp := &Response{
							PacketType: ptype,
							Tag:        tag,
							Payload:    payload,
							Raw:        rxBuf[:idx],
						}
						if c.verbose {
							name := PacketTypeName(ptype)
							fmt.Printf("  ← RX: %s tag=%d [%d bytes]\n", name, tag, len(payload))
						}
						c.dispatchResponse(resp)
					}
				}

				// Advance past this packet (including delimiter)
				rxBuf = rxBuf[idx+1:]
			}

			// Prevent unbounded growth if no delimiters found
			if len(rxBuf) > 16384 {
				rxBuf = rxBuf[len(rxBuf)-4096:]
			}
		}
	}()
}

// stopReader signals the reader goroutine to stop and waits for it.
func (c *Connection) stopReader() {
	if c.readerStop == nil {
		return
	}
	select {
	case <-c.readerStop:
		// Already closed
	default:
		close(c.readerStop)
	}
	if c.readerDone != nil {
		<-c.readerDone
	}
}

// dispatchResponse delivers a response to the correct consumer.
func (c *Connection) dispatchResponse(resp *Response) {
	if resp.Tag != TagAsync {
		c.waiterMu.Lock()

		// Check stream waiters first (multi-response: deliver without removing)
		sch, sok := c.streamWaiters[resp.Tag]
		if sok {
			c.waiterMu.Unlock()
			sch <- resp
			return
		}

		// Then check single-response tag waiters (deliver and remove)
		ch, ok := c.tagWaiters[resp.Tag]
		if ok {
			delete(c.tagWaiters, resp.Tag)
			c.waiterMu.Unlock()
			ch <- resp
			return
		}

		c.waiterMu.Unlock()
	}

	// Async or unmatched — deliver to callback
	if c.asyncCB != nil {
		c.asyncCB(resp)
	}
}

// injectTag re-encodes a packet with a new correlation tag.
func (c *Connection) injectTag(data []byte, tag byte) []byte {
	ptype, _, payload, ok := ParsePacket(data)
	if !ok {
		return data
	}
	return BuildPacket(ptype, payload, tag)
}

// ListPorts returns available serial ports.
func ListPorts() []string {
	ports, err := serial.GetPortsList()
	if err != nil {
		return nil
	}
	return ports
}
