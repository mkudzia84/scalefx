package protocol

// ScaleFX CLI - Serial Connection
// Manages USB serial communication with ScaleFX controllers.
// COBS packet framing, tag-correlated send/receive via single reader goroutine.

import (
	"bytes"
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

const (
	DefaultBaud    = 6000000
	DefaultTimeout = 2 * time.Second
	TagAsync       = 0x00
)

// Response represents a parsed packet from the controller.
type Response struct {
	PacketType PacketType
	Tag        byte
	Payload    []byte
	Raw        []byte
}

// Core packet types used by Response helpers. Defined as raw hex to avoid
// circular import with protocol/core sub-package.
func (r *Response) IsACK() bool       { return r.PacketType == 0xF6 } // core.Ack
func (r *Response) IsNACK() bool      { return r.PacketType == 0xF7 } // core.Nack
func (r *Response) IsInitReady() bool  { return r.PacketType == 0xF3 } // core.InitReady
func (r *Response) IsIdentify() bool   { return r.PacketType == 0xFE } // core.Identify

func (r *Response) ErrorCode() ErrorCode {
	if r.IsNACK() && len(r.Payload) > 0 {
		return ErrorCode(r.Payload[0])
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

// WireLogger is an optional per-packet trace hook.  Set via
// SetWireLogger; called on every TX (`dir="TX"`) and RX (`dir="RX"`)
// whenever the connection's verbose flag is on.  Studio plugs in a
// callback that re-emits the line through its diag system (so the
// console shows every wire packet when `/diag debug on` is enabled);
// CLI tools leave it nil and keep the legacy stdout printf path.
type WireLogger func(dir, name string, tag byte, payloadLen int)

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

	writeMu      sync.Mutex // protects serial writes
	nextTag      byte
	lastSendTime time.Time // time of last packet sent (any type)

	// Reader goroutine → waiter delivery
	waiterMu      sync.Mutex
	tagWaiters    map[byte]chan *Response
	streamWaiters map[byte]chan *Response      // multi-response streams
	asyncFilters  map[PacketType]chan *Response // type-based async packet intercept

	asyncCB    AsyncCallback
	wireLogger WireLogger // optional per-packet trace; nil = legacy stdout printf path

	// Port-loss detection: set once when a persistent I/O error occurs
	portDead    atomic.Bool
	OnPortError func() // called once when port-loss is detected

	readerStop chan struct{}
	readerDone chan struct{}

	// Async dispatch goroutine — async/unmatched packets are handed off here
	// instead of being delivered INLINE in the reader.  A slow async consumer
	// (Studio's Wails event emit at 50 Hz) must NEVER block the reader, or the
	// serial buffer backs up and command responses time out.  The queue is
	// bounded; async broadcasts (live channel view) are lossy, so on overflow
	// we drop the frame and count it rather than block.
	asyncQueue chan *Response
	asyncStop  chan struct{}
	asyncDone  chan struct{}

	// Keepalive goroutine — mirrors C++ SerialBus::processKeepalive()
	keepaliveStop chan struct{}
	keepaliveDone chan struct{}

	// ── Link instrumentation (atomic; snapshot via Stats()) ──────────
	// Counts + timings to diagnose intermittent command timeouts under an
	// async-broadcast flood.  The reader goroutine is the only writer of the
	// RX counters; SendAndWait/* increment the timeout counter.  asyncCB
	// timing reveals whether slow async consumers (Studio Wails emits) are
	// stalling the reader and starving tagged-response delivery.
	stat statCounters
}

type statCounters struct {
	rxFrames     atomic.Uint64 // complete COBS frames parsed
	rxTagged     atomic.Uint64 // tag!=0 delivered to a waiter
	rxAsync      atomic.Uint64 // tag==0 → async callback
	rxUnmatched  atomic.Uint64 // tag!=0 but no waiter (late/orphaned)
	txPackets    atomic.Uint64 // packets sent (Send)
	timeouts     atomic.Uint64 // SendAndWait/* deadline fired
	asyncCBNanos atomic.Uint64 // cumulative ns spent in asyncCB (dispatcher goroutine)
	asyncCBMax   atomic.Uint64 // worst single asyncCB ns
	asyncDropped atomic.Uint64 // async frames dropped on queue overflow (slow consumer)
	dispatchMax  atomic.Uint64 // worst single dispatchResponse ns (reader stall proxy)
}

// LinkStats is a point-in-time snapshot of the link instrumentation.
type LinkStats struct {
	RxFrames, RxTagged, RxAsync, RxUnmatched uint64
	TxPackets, Timeouts, AsyncDropped        uint64
	AsyncCBTotal, AsyncCBMax, DispatchMax    time.Duration
}

// Stats returns a snapshot of the link instrumentation counters.
func (c *Connection) Stats() LinkStats {
	return LinkStats{
		RxFrames:     c.stat.rxFrames.Load(),
		RxTagged:     c.stat.rxTagged.Load(),
		RxAsync:      c.stat.rxAsync.Load(),
		RxUnmatched:  c.stat.rxUnmatched.Load(),
		TxPackets:    c.stat.txPackets.Load(),
		Timeouts:     c.stat.timeouts.Load(),
		AsyncDropped: c.stat.asyncDropped.Load(),
		AsyncCBTotal: time.Duration(c.stat.asyncCBNanos.Load()),
		AsyncCBMax:   time.Duration(c.stat.asyncCBMax.Load()),
		DispatchMax:  time.Duration(c.stat.dispatchMax.Load()),
	}
}

// ResetStats zeroes the instrumentation counters.
func (c *Connection) ResetStats() {
	c.stat.rxFrames.Store(0)
	c.stat.rxTagged.Store(0)
	c.stat.rxAsync.Store(0)
	c.stat.rxUnmatched.Store(0)
	c.stat.txPackets.Store(0)
	c.stat.timeouts.Store(0)
	c.stat.asyncDropped.Store(0)
	c.stat.asyncCBNanos.Store(0)
	c.stat.asyncCBMax.Store(0)
	c.stat.dispatchMax.Store(0)
}

// storeMax atomically raises *a to v if v is larger.
func storeMax(a *atomic.Uint64, v uint64) {
	for {
		cur := a.Load()
		if v <= cur || a.CompareAndSwap(cur, v) {
			return
		}
	}
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
		asyncFilters:  make(map[PacketType]chan *Response),
	}
}

// PortName returns the serial port name.
func (c *Connection) PortName() string { return c.portName }

// Baud returns the baud rate.
func (c *Connection) Baud() int { return c.baud }

// Verbose returns the verbose flag.
func (c *Connection) Verbose() bool { return c.verbose }

// SetVerbose updates the verbose flag.
func (c *Connection) SetVerbose(v bool) { c.verbose = v }

// SetWireLogger installs a per-packet trace callback (TX + RX).  Pass
// nil to fall back to the legacy stdout printf trace.  Calls are
// gated on the verbose flag — flip that on independently.
func (c *Connection) SetWireLogger(l WireLogger) { c.wireLogger = l }

// SetTimeout updates the default response timeout.
func (c *Connection) SetTimeout(d time.Duration) { c.timeout = d }

// Timeout returns the current response timeout.
func (c *Connection) Timeout() time.Duration { return c.timeout }

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

	// Wait for device to settle, drain boot output (bounded — see drain()).
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
	c.StopKeepalive()
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

// GetCallback returns the currently-installed async callback (or nil).
// Lets observers chain — read the existing one, install a wrapper that
// calls it after observing.
func (c *Connection) GetCallback() AsyncCallback {
	return c.asyncCB
}

// RegisterAsyncFilter registers a channel that intercepts async packets
// of a specific type before they reach the general asyncCB callback.
// Returns the channel. Caller must call UnregisterAsyncFilter when done.
func (c *Connection) RegisterAsyncFilter(ptype PacketType, ch chan *Response) {
	c.waiterMu.Lock()
	c.asyncFilters[ptype] = ch
	c.waiterMu.Unlock()
}

// UnregisterAsyncFilter removes a type-based async packet filter.
func (c *Connection) UnregisterAsyncFilter(ptype PacketType) {
	c.waiterMu.Lock()
	delete(c.asyncFilters, ptype)
	c.waiterMu.Unlock()
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
			if c.wireLogger != nil {
				c.wireLogger("TX", name, tag, len(payload))
			} else {
				fmt.Printf("  → TX: %s tag=%d [%d bytes]\n", name, tag, len(payload))
			}
		}
	}

	_, err := c.port.Write(data)
	if err == nil {
		c.lastSendTime = time.Now()
		c.stat.txPackets.Add(1)
	}
	return err
}

// SendRaw writes raw bytes to the serial port without COBS framing or verbose logging.
func (c *Connection) SendRaw(data []byte) error {
	if c.port == nil {
		return fmt.Errorf("not connected")
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	_, err := c.port.Write(data)
	if err == nil {
		c.lastSendTime = time.Now()
	}
	return err
}

// FlushOutput waits until all data in the serial port output buffer has been
// transmitted on the wire.  This is critical for stream uploads: the OS may
// buffer megabytes of raw data and return from Write() instantly, but the
// wire transfer (USB CDC) takes much longer.  Without this, subsequent COBS
// packets (like UPLOAD_END) get queued behind the raw data and the response
// timeout fires before the device can process them.
func (c *Connection) FlushOutput() {
	if c.port == nil {
		return
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	c.port.Drain() // blocks until all TX data is on the wire
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
		c.stat.timeouts.Add(1)
		return nil, fmt.Errorf("timeout waiting for response")
	}
}

// SendExpectACK sends a packet and expects ACK/NACK response.
// Uses a stream waiter to handle firmware that sends async status updates
// (e.g., GEAR_SEQ_STATUS) with the same correlation tag before the ACK.
// Any non-ACK/NACK responses are forwarded to the async callback.
func (c *Connection) SendExpectACK(data []byte) (*Response, error) {
	return c.SendExpectACKTimeout(data, c.timeout)
}

// SendExpectACKTimeout sends a packet and expects ACK/NACK with a custom timeout.
// Use this for operations that may take longer than the default timeout
// (e.g., UPLOAD_END after a stream transfer where the server finalises on SD).
func (c *Connection) SendExpectACKTimeout(data []byte, timeout time.Duration) (*Response, error) {
	if c.port == nil {
		return nil, fmt.Errorf("not connected")
	}

	tag := c.NextTag()
	tagged := c.injectTag(data, tag)

	// Use stream waiter (multi-response) so same-tag async packets
	// don't consume the single-shot slot before the ACK arrives.
	ch := make(chan *Response, 32)
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

	deadline := time.After(timeout)
	for {
		select {
		case resp := <-ch:
			if resp.IsACK() || resp.IsNACK() {
				return resp, nil
			}
			// Forward non-ACK/NACK (e.g., GEAR_SEQ_STATUS) to async handler
			if c.asyncCB != nil {
				c.asyncCB(resp)
			}
		case <-deadline:
			c.stat.timeouts.Add(1)
			return nil, fmt.Errorf("timeout waiting for response")
		}
	}
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
			case StreamBegin:
				if len(resp.Payload) >= 4 {
					result.TotalBytes = ReadU32LE(resp.Payload, 0)
				}

			case StreamData:
				if len(resp.Payload) >= 4 {
					// [seq:u16][crc:u16][data...]
					chunk := resp.Payload[4:]
					result.Data = append(result.Data, chunk...)
				}

			case StreamEnd:
				if len(resp.Payload) >= 8 {
					result.TotalSegs = ReadU16LE(resp.Payload, 0)
					result.TotalBytes = ReadU32LE(resp.Payload, 2)
					result.CRC = ReadU16LE(resp.Payload, 6)
				}
				return &result, nil
			}

		case <-deadline:
			c.stat.timeouts.Add(1)
			return nil, fmt.Errorf("stream timeout")
		}
	}
}

// StartKeepalive starts a background goroutine that sends KEEPALIVE packets
// when no other traffic has been sent within the given interval. This mirrors
// the C++ SerialBus::processKeepalive() pattern: only send when idle.
// The server-side CONNECTION_TIMEOUT is 15s; a 5s interval gives 3x margin.
func (c *Connection) StartKeepalive(interval time.Duration) {
	c.StopKeepalive()

	keepalivePacket := BuildPacket(0xF2, nil, 0) // CorePacket::KEEPALIVE

	c.keepaliveStop = make(chan struct{})
	c.keepaliveDone = make(chan struct{})

	go func() {
		defer close(c.keepaliveDone)
		ticker := time.NewTicker(500 * time.Millisecond) // check frequently
		defer ticker.Stop()

		for {
			select {
			case <-c.keepaliveStop:
				return
			case <-ticker.C:
				if c.portDead.Load() {
					return
				}
				c.writeMu.Lock()
				idle := time.Since(c.lastSendTime)
				c.writeMu.Unlock()

				if idle >= interval {
					if err := c.Send(keepalivePacket); err != nil {
						c.signalPortDead()
						return
					}
				}
			}
		}
	}()
}

// StopKeepalive stops the keepalive goroutine.
func (c *Connection) StopKeepalive() {
	if c.keepaliveStop != nil {
		select {
		case <-c.keepaliveStop:
			// already closed
		default:
			close(c.keepaliveStop)
		}
		if c.keepaliveDone != nil {
			<-c.keepaliveDone
		}
		c.keepaliveStop = nil
		c.keepaliveDone = nil
	}
}

// drain discards pending serial data (called before reader starts).
func (c *Connection) drain() {
	if c.port == nil {
		return
	}
	// BOUNDED.  Discard immediately-available stale bytes (boot text, a stale
	// broadcast backlog) but NEVER loop indefinitely: a board in its verbose
	// window broadcasts live input frames continuously (~50 Hz), so it never
	// goes quiet, and an unbounded drain would block the connect for the whole
	// ~8 s window (the "tree/file.list times out in the GUI" bug).  Cap the
	// total drain; the reader goroutine filters async broadcasts from command
	// responses, so any leftover backlog is handled there, not here.
	buf := make([]byte, 4096)
	deadline := time.Now().Add(300 * time.Millisecond)
	for time.Now().Before(deadline) {
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

	// Async dispatcher goroutine + its bounded handoff queue (256 ≈ 5 s of
	// 50 Hz broadcast).  Started before the reader so the first frame has a
	// consumer.  See dispatchResponse.
	c.asyncQueue = make(chan *Response, 256)
	c.asyncStop = make(chan struct{})
	c.asyncDone = make(chan struct{})
	go c.asyncDispatcher()

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
			n, err := c.port.Read(readBuf)
			if err != nil && n == 0 {
				// Distinguish timeout (n==0, err!=nil on some drivers) from
				// real port-loss. A timeout returns 0 bytes but is not fatal;
				// a physical disconnect returns a persistent error.
				// Check by attempting a second read after a short pause.
				time.Sleep(50 * time.Millisecond)
				n2, err2 := c.port.Read(readBuf)
				if err2 != nil && n2 == 0 {
					c.signalPortDead()
					return
				}
				if n2 > 0 {
					rxBuf = append(rxBuf, readBuf[:n2]...)
				}
			} else if n > 0 {
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
						c.stat.rxFrames.Add(1)
						resp := &Response{
							PacketType: ptype,
							Tag:        tag,
							Payload:    payload,
							// packetData is a fresh per-packet copy; rxBuf is
							// reused, so an async resp queued for later delivery
							// must NOT alias rxBuf (would corrupt under the
							// dispatcher goroutine).
							Raw: packetData[:idx],
						}
						if c.verbose {
							name := PacketTypeName(ptype)
							if c.wireLogger != nil {
								c.wireLogger("RX", name, tag, len(payload))
							} else {
								fmt.Printf("  ← RX: %s tag=%d [%d bytes]\n", name, tag, len(payload))
							}
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

	// Stop the async dispatcher after the reader (which feeds its queue).
	if c.asyncStop != nil {
		select {
		case <-c.asyncStop:
		default:
			close(c.asyncStop)
		}
		if c.asyncDone != nil {
			<-c.asyncDone
		}
		c.asyncStop = nil
		c.asyncDone = nil
	}
}

// signalPortDead sets the portDead flag and fires the OnPortError callback once.
func (c *Connection) signalPortDead() {
	if c.portDead.CompareAndSwap(false, true) {
		if c.OnPortError != nil {
			go c.OnPortError()
		}
	}
}

// dispatchResponse delivers a response to the correct consumer.
func (c *Connection) dispatchResponse(resp *Response) {
	dStart := time.Now()
	defer func() { storeMax(&c.stat.dispatchMax, uint64(time.Since(dStart))) }()

	if resp.Tag != TagAsync {
		c.waiterMu.Lock()

		// Check stream waiters first (multi-response: deliver without removing)
		sch, sok := c.streamWaiters[resp.Tag]
		if sok {
			c.waiterMu.Unlock()
			c.stat.rxTagged.Add(1)
			sch <- resp
			return
		}

		// Then check single-response tag waiters (deliver and remove)
		ch, ok := c.tagWaiters[resp.Tag]
		if ok {
			delete(c.tagWaiters, resp.Tag)
			c.waiterMu.Unlock()
			c.stat.rxTagged.Add(1)
			ch <- resp
			return
		}

		c.waiterMu.Unlock()
		c.stat.rxUnmatched.Add(1) // tag set but no waiter (late / orphaned)
	}

	// Async or unmatched — hand off to the dispatcher goroutine.  NON-BLOCKING:
	// the reader must NEVER stall on a slow async consumer (Studio's Wails emit
	// at 50 Hz), or the serial buffer backs up and command responses time out
	// (proven: 0% timeouts at delay=0, 86% at delay=150ms, same flood).  Drop
	// on overflow — async broadcasts (live channel view) are lossy.
	c.stat.rxAsync.Add(1)
	if c.asyncQueue != nil {
		select {
		case c.asyncQueue <- resp:
		default:
			c.stat.asyncDropped.Add(1)
		}
	}
}

// asyncDispatcher delivers async/unmatched packets to filters + the general
// callback OFF the reader goroutine, so a slow consumer can't stall reads.
func (c *Connection) asyncDispatcher() {
	defer close(c.asyncDone)
	for {
		select {
		case <-c.asyncStop:
			return
		case resp := <-c.asyncQueue:
			c.deliverAsync(resp)
		}
	}
}

// deliverAsync routes one async packet to its type filter (if any) or the
// general callback.  Runs in the dispatcher goroutine — slowness here costs
// async latency / drops, never command-response timeouts.
func (c *Connection) deliverAsync(resp *Response) {
	c.waiterMu.Lock()
	fch, fok := c.asyncFilters[resp.PacketType]
	c.waiterMu.Unlock()
	if fok {
		// Respect shutdown so a vanished filter consumer can't wedge Close.
		select {
		case fch <- resp:
		case <-c.asyncStop:
		}
		return
	}
	if c.asyncCB != nil {
		cbStart := time.Now()
		c.asyncCB(resp)
		dur := uint64(time.Since(cbStart))
		c.stat.asyncCBNanos.Add(dur)
		storeMax(&c.stat.asyncCBMax, dur)
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

// ListPorts returns available serial port names.
func ListPorts() []string {
	ports, err := serial.GetPortsList()
	if err != nil {
		ports = nil
	}
	return ports
}

// PortDetail holds a port name and its USB product description (if available).
type PortDetail struct {
	Name        string
	Description string
}

// ListPortsDetailed returns available serial ports with USB product
// descriptions.
func ListPortsDetailed() []PortDetail {
	var result []PortDetail
	if detailed, err := enumerator.GetDetailedPortsList(); err == nil {
		result = make([]PortDetail, 0, len(detailed))
		for _, p := range detailed {
			result = append(result, PortDetail{Name: p.Name, Description: p.Product})
		}
	} else {
		// Fallback to basic list.
		names, _ := serial.GetPortsList()
		result = make([]PortDetail, 0, len(names))
		for _, n := range names {
			result = append(result, PortDetail{Name: n})
		}
	}
	return result
}
