package protocol

// ScaleFX — TCP-as-serial transport.
//
// Lets a `Connection` open a TCP socket using a `tcp://host:port` port
// name. Used by the virtual-board test harness in tests/virtual_board so
// Studio + scalefx-cli can talk to it without com0com or a real device.
//
// The adapter implements just enough of go.bug.st/serial.Port for our
// reader goroutine: Read, Write, Close, SetReadTimeout, Drain. The other
// methods (modem bits, baud changes, RTS/DTR, break) are no-ops since
// they have no analogue on a TCP stream.

import (
	"errors"
	"net"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
)

const tcpScheme = "tcp://"

// IsTCPPort returns true if `name` is a `tcp://host:port` virtual port.
func IsTCPPort(name string) bool {
	return strings.HasPrefix(strings.ToLower(name), tcpScheme)
}

// openTCPPort dials a TCP endpoint and wraps it in a serial.Port adapter.
func openTCPPort(name string) (serial.Port, error) {
	addr := name[len(tcpScheme):]
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	return &tcpPort{conn: conn}, nil
}

// tcpPort is a serial.Port-shaped TCP wrapper. ReadDeadline is rolled
// forward each Read so the reader goroutine's 100ms timeout works the
// same way it does on a real COM port.
type tcpPort struct {
	mu          sync.Mutex
	conn        net.Conn
	readTimeout time.Duration
	closed      bool
}

func (p *tcpPort) Read(buf []byte) (int, error) {
	p.mu.Lock()
	c := p.conn
	rt := p.readTimeout
	closed := p.closed
	p.mu.Unlock()
	if closed || c == nil {
		return 0, errors.New("tcp port closed")
	}
	if rt > 0 {
		_ = c.SetReadDeadline(time.Now().Add(rt))
	} else {
		_ = c.SetReadDeadline(time.Time{})
	}
	n, err := c.Read(buf)
	// Match go.bug.st/serial semantics: a read timeout is the idle path
	// and reports (0, nil) so the connection's reader loop does NOT treat
	// it as port-loss. Real failures (EOF, reset) keep the error so the
	// loop can fail over correctly.
	if err != nil {
		var ne net.Error
		if errors.As(err, &ne) && ne.Timeout() {
			return n, nil
		}
	}
	return n, err
}

func (p *tcpPort) Write(buf []byte) (int, error) {
	p.mu.Lock()
	c := p.conn
	closed := p.closed
	p.mu.Unlock()
	if closed || c == nil {
		return 0, errors.New("tcp port closed")
	}
	return c.Write(buf)
}

func (p *tcpPort) Close() error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return nil
	}
	p.closed = true
	if p.conn != nil {
		err := p.conn.Close()
		p.conn = nil
		return err
	}
	return nil
}

func (p *tcpPort) SetReadTimeout(t time.Duration) error {
	p.mu.Lock()
	p.readTimeout = t
	p.mu.Unlock()
	return nil
}

func (p *tcpPort) Drain() error                            { return nil }
func (p *tcpPort) ResetInputBuffer() error                 { return nil }
func (p *tcpPort) ResetOutputBuffer() error                { return nil }
func (p *tcpPort) SetMode(*serial.Mode) error              { return nil }
func (p *tcpPort) SetDTR(bool) error                       { return nil }
func (p *tcpPort) SetRTS(bool) error                       { return nil }
func (p *tcpPort) GetModemStatusBits() (*serial.ModemStatusBits, error) {
	return &serial.ModemStatusBits{}, nil
}
func (p *tcpPort) Break(time.Duration) error               { return nil }
