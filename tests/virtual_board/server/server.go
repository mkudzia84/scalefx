// Virtual board — TCP server + per-client packet pump.
//
// Generic across boards: handles framing (COBS/CRC8), the core packet
// triad (INIT/IDENTIFY/KEEPALIVE/STATUS_REQ + REBOOT/BOOTSEL/SHUTDOWN),
// and the 1 Hz STATUS broadcast loop. Board-specific packets are
// forwarded to `Board.HandlePacket`.

package server

import (
	"errors"
	"fmt"
	"log"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	"scalefx/tests/virtual_board/wirelog"
)

// Options configures a Server instance.
type Options struct {
	Addr    string // TCP listen address ("host:port" or ":port")
	Verbose bool   // Log every packet sent / received
}

// Server hosts one Board over a TCP listener. Only one client at a time.
type Server struct {
	board Board
	opts  Options

	mu         sync.Mutex
	conn       net.Conn
	statusStop chan struct{}

	statusCounter atomic.Uint32
	keepalives    atomic.Uint32
	lastActivity  atomic.Int64 // nanos
	initFlags     atomic.Uint32
	initialized   atomic.Bool
	bootTime      time.Time

	// Captured INIT mode for STATUS boardState reporting.
	initMode atomic.Uint32
}

// New constructs a Server bound to the given Board.
func New(b Board, opts Options) *Server {
	return &Server{board: b, opts: opts, bootTime: time.Now()}
}

// Run starts the listener and accepts clients sequentially. Returns
// only on listener shutdown / error.
func (s *Server) Run() error {
	ln, err := net.Listen("tcp", s.opts.Addr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", s.opts.Addr, err)
	}
	defer ln.Close()
	log.Printf("virtual_board: %s '%s' listening on %s",
		s.board.BoardKind(), s.board.Name(), ln.Addr())

	for {
		c, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			log.Printf("accept: %v", err)
			continue
		}
		s.dropExistingClient()
		s.startClient(c)
	}
}

// Addr returns the actual address the listener is bound to (useful when
// the caller passed `:0`).
func (s *Server) Addr() string { return s.opts.Addr }

func (s *Server) dropExistingClient() {
	s.mu.Lock()
	c := s.conn
	stop := s.statusStop
	s.conn = nil
	s.statusStop = nil
	s.mu.Unlock()
	if stop != nil {
		select {
		case <-stop:
		default:
			close(stop)
		}
	}
	if c != nil {
		_ = c.Close()
	}
}

func (s *Server) startClient(c net.Conn) {
	s.mu.Lock()
	s.conn = c
	stop := make(chan struct{})
	s.statusStop = stop
	s.mu.Unlock()

	s.initialized.Store(false)
	s.initFlags.Store(0)
	s.statusCounter.Store(0)
	s.keepalives.Store(0)
	log.Printf("virtual_board: client connected from %s", c.RemoteAddr())

	go s.runStatusLoop(stop)
	go s.runReader(c, stop)
}

// ─── Sender interface — exposed to Board.HandlePacket ───

func (s *Server) Send(ptype protocol.PacketType, tag byte, payload []byte) {
	s.mu.Lock()
	c := s.conn
	s.mu.Unlock()
	if c == nil {
		return
	}
	frame := protocol.BuildPacket(ptype, payload, tag)
	if s.opts.Verbose {
		log.Print(wirelog.FormatPacket(wirelog.TX, ptype, tag, payload))
	}
	_, _ = c.Write(frame)
}

func (s *Server) Ack(tag byte) {
	s.Send(pcore.Ack, tag, nil)
}

func (s *Server) Nack(tag byte, code protocol.ErrorCode) {
	s.Send(pcore.Nack, tag, []byte{byte(code)})
}

// ─── Reader loop ───

func (s *Server) runReader(c net.Conn, stop chan struct{}) {
	defer func() {
		s.mu.Lock()
		if s.conn == c {
			s.conn = nil
			if s.statusStop != nil {
				select {
				case <-s.statusStop:
				default:
					close(s.statusStop)
				}
				s.statusStop = nil
			}
		}
		s.mu.Unlock()
		_ = c.Close()
		log.Printf("virtual_board: client disconnected")
	}()

	rxBuf := make([]byte, 0, 4096)
	readBuf := make([]byte, 1024)
	for {
		select {
		case <-stop:
			return
		default:
		}
		_ = c.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, err := c.Read(readBuf)
		if n > 0 {
			rxBuf = append(rxBuf, readBuf[:n]...)
			s.lastActivity.Store(time.Now().UnixNano())
			rxBuf = s.consumeFrames(rxBuf)
		}
		if err != nil {
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() {
				continue
			}
			return
		}
	}
}

func (s *Server) consumeFrames(buf []byte) []byte {
	for {
		idx := -1
		for i, b := range buf {
			if b == 0x00 {
				idx = i
				break
			}
		}
		if idx < 0 {
			return buf
		}
		frame := buf[:idx]
		buf = buf[idx+1:]
		if len(frame) == 0 {
			continue
		}
		ptype, tag, payload, ok := protocol.ParsePacket(frame)
		if !ok {
			if s.opts.Verbose {
				log.Printf("rx: parse failed (%d bytes)", len(frame))
			}
			continue
		}
		if s.opts.Verbose {
			log.Print(wirelog.FormatPacket(wirelog.RX, ptype, tag, payload))
		}
		s.dispatch(ptype, tag, payload)
	}
}

// dispatch handles core packets locally and forwards everything else to
// the Board. Unrecognised packets get a NACK(InvalidCommand).
func (s *Server) dispatch(ptype protocol.PacketType, tag byte, payload []byte) {
	switch ptype {
	case pcore.Init:
		s.handleInit(tag, payload)
		return
	case pcore.Identify:
		s.handleIdentify(tag)
		return
	case pcore.Keepalive:
		s.keepalives.Add(1)
		s.Ack(tag)
		return
	case pcore.StatusReq, pcore.Status:
		s.sendStatus(tag)
		return
	case pcore.Reboot:
		s.Ack(tag)
		s.initialized.Store(false)
		s.initFlags.Store(0)
		return
	case pcore.Bootsel:
		s.Ack(tag)
		s.initialized.Store(false)
		return
	case pcore.Shutdown:
		s.Ack(tag)
		s.initialized.Store(false)
		return
	}

	// FILE_* handled centrally if the board has a filesystem — saves
	// every board from re-implementing the same protocol shape.
	if s.handleFileIfFS(ptype, tag, payload) {
		return
	}

	if s.board.HandlePacket(s, ptype, tag, payload) {
		return
	}

	s.Nack(tag, pcore.ErrInvalidCommand)
}

// ─── Init / Identify / Status ───

func (s *Server) handleInit(tag byte, payload []byte) {
	var mode, flags byte
	if len(payload) >= 1 {
		mode = payload[0]
	}
	if len(payload) >= 2 {
		flags = payload[1]
	}
	s.initMode.Store(uint32(mode))
	s.initFlags.Store(uint32(flags))
	s.initialized.Store(true)
	s.Send(pcore.InitReady, tag, s.initReadyPayload())
}

func (s *Server) handleIdentify(tag byte) {
	// IDENTIFY does NOT activate hardware (CLAUDE.md). Reply with the
	// same payload but on the IDENTIFY packet type.
	s.Send(pcore.Identify, tag, s.initReadyPayload())
}

const (
	defaultVersion  = "0.99.0-virt"
	defaultPlatform = "VIRT-x86"
	defaultCPUMHz   = 100
	defaultFreeRAM  = 200000
	defaultBuild    = 1
)

func (s *Server) initReadyPayload() []byte {
	name := s.board.Name()
	ver := s.board.Version()
	if ver == "" {
		ver = defaultVersion
	}
	plat := s.board.Platform()
	if plat == "" {
		plat = defaultPlatform
	}
	caps := s.board.Capabilities()

	out := make([]byte, 0, 64)
	out = append(out, byte(len(name)))
	out = append(out, []byte(name)...)
	out = append(out, byte(len(ver)))
	out = append(out, []byte(ver)...)
	out = append(out, byte(len(plat)))
	out = append(out, []byte(plat)...)
	out = append(out, protocol.U32LE(defaultCPUMHz)...)
	out = append(out, protocol.U32LE(defaultFreeRAM)...)
	out = append(out, protocol.U32LE(defaultBuild)...)
	out = append(out, protocol.U32LE(caps)...)
	return out
}

// runStatusLoop drives two cadences off independent tickers:
//
//   - A 50 ms simulation tick — calls Board.Tick(now) so LED sequences,
//     landing-light phase machines, etc. advance often enough that the
//     reported state isn't aliased by the (much slower) broadcast
//     period. Brightness for a 200 ms flash event sampled at 50 ms is
//     within 25 ms of truth.
//
//   - A 1 s STATUS broadcast tick — emits the STATUS packet to the
//     connected client. Matches what the firmware does so the GUI's
//     update rate is the same as on real hardware.
//
// The simulation tick runs unconditionally — the board model needs to
// stay current even before INIT (ping/pong via STATUS_REQ still gets
// fresh data). The broadcast tick is gated on `initialized` + verbose.
func (s *Server) runStatusLoop(stop chan struct{}) {
	const simTickInterval = 50 * time.Millisecond
	const broadcastInterval = 1 * time.Second

	simTick := time.NewTicker(simTickInterval)
	defer simTick.Stop()
	bcastTick := time.NewTicker(broadcastInterval)
	defer bcastTick.Stop()

	for {
		select {
		case <-stop:
			return
		case now := <-simTick.C:
			s.board.Tick(s, now)
		case <-bcastTick.C:
			if !s.initialized.Load() {
				continue
			}
			if (s.initFlags.Load() & uint32(pcore.InitFlagVerbose)) == 0 {
				continue
			}
			s.sendStatus(protocol.TagAsync)
		}
	}
}

func (s *Server) sendStatus(tag byte) {
	s.board.Tick(s, time.Now())

	counter := s.statusCounter.Add(1)
	uptimeMs := uint32(time.Since(s.bootTime) / time.Millisecond)
	lastActMs := uint32(0)
	if last := s.lastActivity.Load(); last > 0 {
		lastActMs = uint32(time.Since(time.Unix(0, last)) / time.Millisecond)
	}
	keepalives := s.keepalives.Load()

	boardState := pcore.BoardStateIdle
	if s.initialized.Load() {
		switch byte(s.initMode.Load()) {
		case pcore.InitModeDirect:
			boardState = pcore.BoardStateDirect
		case pcore.InitModeSlave:
			boardState = pcore.BoardStateSlave
		default:
			boardState = pcore.BoardStateStandalone
		}
	}
	flags := byte(s.initFlags.Load())

	header := make([]byte, 0, 22)
	header = append(header, protocol.U32LE(counter)...)
	header = append(header, protocol.U32LE(uptimeMs)...)
	header = append(header, protocol.U32LE(defaultFreeRAM)...)
	header = append(header, protocol.U32LE(lastActMs)...)
	header = append(header, protocol.U32LE(keepalives)...)
	header = append(header, boardState, flags)

	moduleData := s.board.BuildStatusModuleData()
	payload := append(header, moduleData...)
	s.Send(pcore.Status, tag, payload)
}
