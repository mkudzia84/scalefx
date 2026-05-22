package main

// Interactive command console — drives the SAME `scalefx/console` command
// set the CLI uses, with output rendered into the GUI console.  The shared
// session writes ANSI-colored text to `consoleSink`, which buffers by line,
// converts ANSI escapes to <span class="c-*"> markup (the console CSS
// already styles those classes), and emits one console:output event per
// line.

import (
	"fmt"
	"strings"
	"sync"

	"scalefx/console"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// setupConsole wires the shared command session + its output sink.  Called
// once from startup() after the Wails context is available.
func (a *App) setupConsole() {
	a.con = console.NewApp(0, false)
	console.SetColor(true) // keep ANSI — consoleSink converts it to HTML
	console.SetOutput(&consoleSink{a: a})
}

// dispatchConsole runs one command line through the shared session against
// the live client.  Connection verbs are handled by Studio's own UI, so
// they're intercepted here rather than dispatched.
func (a *App) dispatchConsole(line string) {
	a.echoCommand(line)
	first := strings.Fields(line)
	if len(first) == 0 {
		return
	}
	switch first[0] {
	case "connect", "disconnect":
		a.echoError("use Studio's Connect / Disconnect controls for connection management")
		return
	}

	a.mu.Lock()
	if a.c == nil {
		a.mu.Unlock()
		a.echoError("not connected")
		return
	}
	a.con.Attach(a.c, a.id.Capabilities, a.id.EnabledCapabilities, a.kind, a.id.DeviceName)
	err := a.con.Dispatch(line)
	a.mu.Unlock()
	if err != nil {
		a.echoError("%v", err)
	}
}

// ─── ANSI → HTML console sink ─────────────────────────────────────────

// consoleSink is the io.Writer the shared command package renders to.  It
// accumulates bytes, splits on newlines, and emits each complete line as a
// console:output event with ANSI escapes converted to span markup.
type consoleSink struct {
	a   *App
	mu  sync.Mutex
	buf []byte
}

func (s *consoleSink) Write(p []byte) (int, error) {
	s.mu.Lock()
	s.buf = append(s.buf, p...)
	var lines []string
	for {
		i := indexByte(s.buf, '\n')
		if i < 0 {
			break
		}
		lines = append(lines, string(s.buf[:i]))
		s.buf = s.buf[i+1:]
	}
	s.mu.Unlock()

	for _, ln := range lines {
		ln = strings.TrimRight(ln, "\r")
		s.a.emitConsoleHTML(ansiToHTML(ln))
	}
	return len(p), nil
}

func indexByte(b []byte, c byte) int {
	for i, x := range b {
		if x == c {
			return i
		}
	}
	return -1
}

// ─── Console echo helpers ─────────────────────────────────────────────

// echoCommand renders the user's typed line back into the console.
func (a *App) echoCommand(cmd string) {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{Type: "command", Content: escapeHTML(cmd)})
}

// echoOutput emits a plain output line.
func (a *App) echoOutput(text string) {
	if a.ctx == nil || text == "" {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{Type: "output", Content: escapeHTML(text)})
}

// echoOK emits a success line.
func (a *App) echoOK(format string, args ...any) {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{Type: "ok", Content: escapeHTML(fmt.Sprintf(format, args...))})
}

// echoError emits an error line.
func (a *App) echoError(format string, args ...any) {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{Type: "error", Content: escapeHTML(fmt.Sprintf(format, args...))})
}

// emitConsoleHTML pushes a pre-rendered HTML line to the GUI console.
func (a *App) emitConsoleHTML(html string) {
	if a.ctx == nil {
		return
	}
	wailsRT.EventsEmit(a.ctx, "console:output", ConsoleMessage{Type: "output", Content: html})
}

// ansiToHTML converts a single line of ANSI-colored text into HTML, mapping
// the console palette to `c-*` span classes.  A reset (\033[0m) closes every
// open span; nested wraps (e.g. bold+cyan) emit two resets, the second a
// no-op.  Literal text is HTML-escaped.
func ansiToHTML(s string) string {
	var b strings.Builder
	open := 0
	i := 0
	for i < len(s) {
		// Detect an escape sequence: ESC '[' ... 'm'
		if s[i] == 0x1b && i+1 < len(s) && s[i+1] == '[' {
			j := i + 2
			for j < len(s) && s[j] != 'm' {
				j++
			}
			if j < len(s) {
				code := s[i+2 : j]
				cls := ansiClass(code)
				if code == "0" {
					for ; open > 0; open-- {
						b.WriteString("</span>")
					}
				} else if cls != "" {
					b.WriteString(`<span class="` + cls + `">`)
					open++
				}
				i = j + 1
				continue
			}
		}
		// Literal char — HTML-escape.
		switch s[i] {
		case '&':
			b.WriteString("&amp;")
		case '<':
			b.WriteString("&lt;")
		case '>':
			b.WriteString("&gt;")
		default:
			b.WriteByte(s[i])
		}
		i++
	}
	for ; open > 0; open-- {
		b.WriteString("</span>")
	}
	return b.String()
}

// ansiClass maps an ANSI SGR code to a console color class.
func ansiClass(code string) string {
	switch code {
	case "1":
		return "c-bold"
	case "2":
		return "c-dim"
	case "90":
		return "c-gray"
	case "91":
		return "c-red"
	case "92":
		return "c-green"
	case "93":
		return "c-yellow"
	case "94":
		return "c-blue"
	case "95":
		return "c-magenta"
	case "96":
		return "c-cyan"
	case "97":
		return "c-white"
	}
	return ""
}
