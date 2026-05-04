package main

// Studio diagnostics — structured logger that mirrors every event into
// three sinks at once:
//
//   1. stdout (the terminal that ran `wails dev` / the launched binary).
//      One line per event with `[HH:MM:SS.mmm] LEVEL  TAG       message`,
//      easy to grep / pipe back into a chat session.
//
//   2. The GUI console panel — every diagnostic event also flows through
//      the existing `console:output` Wails event so the user sees it
//      without leaving the app. Gated by `debug` for chatty events.
//
//   3. A 200-event ring buffer accessible via the `DiagSnapshot()` Wails
//      binding. The frontend's `/diag` slash command (or `Tools → Copy
//      Diagnostics`) dumps it as JSON for pasting into a bug report.
//
// The package is deliberately blunt — no slog dependency, no log levels
// configurable at runtime beyond a single debug toggle. Goal is "I want
// to see exactly what the app did, in order, when something broke."

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

const (
	// Levels follow the slog convention but stringly typed for JSON / GUI.
	LvlDebug = "debug"
	LvlInfo  = "info"
	LvlWarn  = "warn"
	LvlError = "error"

	diagRingSize = 200
)

// DiagEvent is a single instrumentation record.
type DiagEvent struct {
	Time    string         `json:"time"`              // RFC3339 with milliseconds
	Level   string         `json:"level"`             // debug | info | warn | error
	Tag     string         `json:"tag"`               // CONN, CMD, RPC, EVT, FE, …
	Message string         `json:"message"`
	Fields  map[string]any `json:"fields,omitempty"`
}

// Diag is the logger. One per process; lives on App.
type Diag struct {
	ctx     context.Context
	debug   atomic.Bool
	mu      sync.Mutex
	ring    []DiagEvent
	logFile *os.File
	logPath string
}

// NewDiag returns a Diag with the ring buffer pre-allocated. ctx is
// installed via SetCtx after Wails startup. Also opens the on-disk log
// file (truncating any previous run's output) — Studio is a GUI binary
// on Windows, so a tail-able file is the only reliable way for the
// terminal / Claude Code agent to see what's happening at runtime.
func NewDiag() *Diag {
	d := &Diag{ring: make([]DiagEvent, 0, diagRingSize)}
	d.debug.Store(false)

	// Append, not truncate — `startupTrace` in main.go writes a few
	// lines before NewDiag runs, and we want those preserved so a
	// post-mortem can tell us how far the process got. The user (or the
	// `Diag: Clear Studio Log` task) clears it explicitly between runs.
	logPath := defaultLogPath()
	if f, err := os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
		d.logFile = f
		d.logPath = logPath
		fmt.Fprintf(f, "# scalefx-studio diagnostic log — opened %s pid=%d\n",
			time.Now().Format(time.RFC3339), os.Getpid())
	}
	return d
}

// LogPath returns the on-disk log file path (empty if the file could
// not be opened).
func (d *Diag) LogPath() string { return d.logPath }

// defaultLogPath puts the log file under %TEMP%\scalefx-studio.log on
// Windows, /tmp/scalefx-studio.log elsewhere. Predictable for the
// agent's `cat`/`tail` commands.
func defaultLogPath() string {
	dir := os.TempDir()
	return filepath.Join(dir, "scalefx-studio.log")
}

func (d *Diag) SetCtx(ctx context.Context) { d.ctx = ctx }

// SetDebug toggles whether DEBUG-level events flow to stdout / the GUI
// console (they always go to the ring buffer so a snapshot still has
// them). Default off — flip via `/diag debug on` in the console.
func (d *Diag) SetDebug(on bool) bool {
	d.debug.Store(on)
	d.Info("DIAG", "debug logging %s", onOff(on))
	return on
}

func (d *Diag) DebugEnabled() bool { return d.debug.Load() }

func onOff(b bool) string {
	if b {
		return "ON"
	}
	return "OFF"
}

// ─── Core log entry-points ───

func (d *Diag) Debug(tag, format string, args ...any) {
	d.log(LvlDebug, tag, fmt.Sprintf(format, args...), nil)
}
func (d *Diag) Info(tag, format string, args ...any) {
	d.log(LvlInfo, tag, fmt.Sprintf(format, args...), nil)
}
func (d *Diag) Warn(tag, format string, args ...any) {
	d.log(LvlWarn, tag, fmt.Sprintf(format, args...), nil)
}
func (d *Diag) Error(tag, format string, args ...any) {
	d.log(LvlError, tag, fmt.Sprintf(format, args...), nil)
}

// With logs with a structured fields map (useful when the message has
// many key/values that would otherwise be hard to grep).
func (d *Diag) With(level, tag, msg string, fields map[string]any) {
	d.log(level, tag, msg, fields)
}

func (d *Diag) log(level, tag, msg string, fields map[string]any) {
	now := time.Now()
	stamp := now.Format("15:04:05.000")
	tagPad := tag
	if len(tagPad) < 8 {
		tagPad = tagPad + strings.Repeat(" ", 8-len(tagPad))
	}

	// 1) Ring buffer (always — even when debug is off, snapshots need it).
	rec := DiagEvent{
		Time:    now.Format("2006-01-02T15:04:05.000Z07:00"),
		Level:   level,
		Tag:     tag,
		Message: msg,
		Fields:  fields,
	}
	d.mu.Lock()
	if len(d.ring) >= diagRingSize {
		copy(d.ring, d.ring[1:])
		d.ring = d.ring[:diagRingSize-1]
	}
	d.ring = append(d.ring, rec)
	d.mu.Unlock()

	// 2) Stdout + on-disk log file. The log file is the agent's primary
	//    sink because Studio runs as a Windows GUI binary (no console).
	//    Stdout is still useful for `wails dev` sessions.
	if level != LvlDebug || d.debug.Load() {
		extra := ""
		if len(fields) > 0 {
			if b, err := json.Marshal(fields); err == nil {
				extra = " " + string(b)
			}
		}
		line := fmt.Sprintf("[%s] %-5s %s %s%s\n",
			stamp, strings.ToUpper(level), tagPad, msg, extra)
		fmt.Fprint(os.Stdout, line)
		if d.logFile != nil {
			_, _ = d.logFile.WriteString(line)
		}
	} else if d.logFile != nil {
		// Even if stdout suppresses debug, the log file gets everything —
		// agents pulling a postmortem benefit from the full trace.
		extra := ""
		if len(fields) > 0 {
			if b, err := json.Marshal(fields); err == nil {
				extra = " " + string(b)
			}
		}
		fmt.Fprintf(d.logFile, "[%s] %-5s %s %s%s\n",
			stamp, strings.ToUpper(level), tagPad, msg, extra)
	}

	// 3) GUI console mirror.
	if d.ctx != nil {
		// Map onto the existing console:output types so styling Just Works.
		ctype := "info"
		switch level {
		case LvlDebug:
			ctype = "debug"
		case LvlWarn:
			ctype = "warning"
		case LvlError:
			ctype = "error"
		}
		// Keep the GUI uncluttered: send debug only when the user has
		// opted in (the frontend shows Debug-typed lines in dim grey).
		if level != LvlDebug || d.debug.Load() {
			content := fmt.Sprintf(`<span class="diag-tag">%s</span> %s`,
				escapeHTML(tag), escapeHTML(msg))
			if len(fields) > 0 {
				if b, err := json.Marshal(fields); err == nil {
					content += fmt.Sprintf(` <span class="diag-fields">%s</span>`,
						escapeHTML(string(b)))
				}
			}
			wailsRT.EventsEmit(d.ctx, "console:output", ConsoleMessage{
				Type:    ctype,
				Content: content,
			})
		}

		// Always emit a structured event for the optional Diag pane / dump.
		wailsRT.EventsEmit(d.ctx, "diag:event", rec)
	}
}

// ─── Process helpers ───
//
// Tiny wrappers so the call sites in app.go stay readable. Keep them
// here so anything else that wants the same identity bits (the
// heartbeat, the snapshot) doesn't have to re-import runtime / os.

func runtimeGoVersion() string { return runtime.Version() }
func runtimeOS() string        { return runtime.GOOS + "/" + runtime.GOARCH }
func processPID() int          { return os.Getpid() }
func workingDir() string {
	d, err := os.Getwd()
	if err != nil {
		return "?"
	}
	return d
}

// Snapshot returns a copy of the recent events ring buffer.
func (d *Diag) Snapshot() []DiagEvent {
	d.mu.Lock()
	defer d.mu.Unlock()
	out := make([]DiagEvent, len(d.ring))
	copy(out, d.ring)
	return out
}

// ─── Method instrumentation ───

// Around is the canonical wrapper for Wails-bound methods. It logs
// enter/exit, recovers panics (so a frontend bug doesn't kill the
// whole app silently), and warns when a call exceeds 500 ms.
//
// Use as:
//
//   defer a.diag.Around("Connect", map[string]any{"port": port})()
//
// which records the start time + arguments and runs the deferred
// finaliser when the function returns / panics.
func (d *Diag) Around(name string, fields map[string]any) func() {
	start := time.Now()
	d.With(LvlDebug, "RPC", name+" → enter", fields)
	return func() {
		dur := time.Since(start)
		if r := recover(); r != nil {
			buf := make([]byte, 8192)
			n := runtime.Stack(buf, false)
			d.With(LvlError, "PANIC", fmt.Sprintf("%s panicked: %v", name, r),
				map[string]any{"stack": string(buf[:n]), "duration_ms": dur.Milliseconds()})
			panic(r) // re-raise — don't swallow the failure semantics
		}
		f := map[string]any{"duration_ms": dur.Milliseconds()}
		if dur > 500*time.Millisecond {
			d.With(LvlWarn, "RPC", name+" slow", f)
		} else {
			d.With(LvlDebug, "RPC", name+" ← done", f)
		}
	}
}

// ─── Heartbeat ───

// StartHeartbeat logs a one-line snapshot every `interval`. Captures
// connection state, free goroutines, allocated heap. Useful for spotting
// lockups (the line stops appearing) or memory creep.
func (d *Diag) StartHeartbeat(interval time.Duration, getState func() map[string]any) chan<- struct{} {
	stop := make(chan struct{})
	go func() {
		t := time.NewTicker(interval)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				fields := map[string]any{
					"goroutines": runtime.NumGoroutine(),
				}
				var ms runtime.MemStats
				runtime.ReadMemStats(&ms)
				fields["heap_kb"] = ms.HeapAlloc / 1024
				if getState != nil {
					for k, v := range getState() {
						fields[k] = v
					}
				}
				d.With(LvlDebug, "HB", "heartbeat", fields)
			}
		}
	}()
	return stop
}
