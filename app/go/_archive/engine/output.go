package engine

// ScaleFX Engine - Output Interface
// Abstracts message rendering so the same command engine can drive
// both the CLI terminal and the GUI console.

import (
	"fmt"
	"os"
	"strings"
	"time"
)

// Color represents a logical color for output formatting.
type Color int

const (
	ColorReset Color = iota
	ColorRed
	ColorGreen
	ColorYellow
	ColorBlue
	ColorMagenta
	ColorCyan
	ColorWhite
	ColorGray
	ColorBold
	ColorDim
)

// Output is the interface that both CLI and GUI implement to receive
// formatted text from the command engine. All handler and parser output
// flows through this interface instead of calling fmt.Printf directly.
type Output interface {
	// Structured messages (with status icons)
	OK(format string, args ...any)
	Error(format string, args ...any)
	Info(format string, args ...any)
	Warning(format string, args ...any)

	// Debug emits engine-level trace output. Bodies are compile-time gated by
	// DebugBuild — release builds compile to a no-op.
	Debug(format string, args ...any)

	// Printf-style raw output (for parsers and formatted sections)
	Printf(format string, args ...any)
	Println(a ...any)

	// C wraps text in a logical color. The receiver decides how to render
	// (ANSI codes, HTML spans, or plain text). Returns the decorated string.
	C(color Color, text string) string
}

// ─── ANSI Terminal Output ───

// ansiCodes maps logical colors to ANSI escape sequences.
var ansiCodes = map[Color]string{
	ColorReset:   "\033[0m",
	ColorRed:     "\033[91m",
	ColorGreen:   "\033[92m",
	ColorYellow:  "\033[93m",
	ColorBlue:    "\033[94m",
	ColorMagenta: "\033[95m",
	ColorCyan:    "\033[96m",
	ColorWhite:   "\033[97m",
	ColorGray:    "\033[90m",
	ColorBold:    "\033[1m",
	ColorDim:     "\033[2m",
}

// TerminalOutput implements Output for ANSI-capable terminals.
type TerminalOutput struct{}

func (t TerminalOutput) OK(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", t.C(ColorGreen, "✓"), msg)
}

func (t TerminalOutput) Error(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", t.C(ColorRed, "✗"), msg)
}

func (t TerminalOutput) Info(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", t.C(ColorCyan, "ℹ"), msg)
}

func (t TerminalOutput) Warning(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", t.C(ColorYellow, "⚠"), msg)
}

func (t TerminalOutput) Printf(format string, args ...any) {
	fmt.Printf(format, args...)
}

func (t TerminalOutput) Debug(format string, args ...any) {
	if !DebugBuild {
		return
	}
	msg := fmt.Sprintf(format, args...)
	fmt.Fprintf(os.Stderr, "%s\n", t.C(ColorGray, "[dbg] "+msg))
}

func (t TerminalOutput) Println(a ...any) {
	fmt.Println(a...)
}

func (t TerminalOutput) C(color Color, text string) string {
	code, ok := ansiCodes[color]
	if !ok {
		return text
	}
	return code + text + ansiCodes[ColorReset]
}

// ─── Progress Bar (shared, returns string) ───

// FormatProgressBar builds a progress bar string for inline display.
// Matches the Python CLI format: [████░░░░] 50% 1234/5678 123.4 KB/s ETA 5s
func FormatProgressBar(current, total int, startTime time.Time, width int) string {
	if total <= 0 {
		return ""
	}
	pct := current * 100 / total
	if pct > 100 {
		pct = 100
	}
	filled := current * width / total
	if filled > width {
		filled = width
	}

	bar := strings.Repeat("\u2588", filled) + strings.Repeat("\u2591", width-filled)

	sizeStr := fmt.Sprintf("%d/%d", current, total)

	speedStr := "-- KB/s"
	etaStr := ""
	elapsed := time.Since(startTime).Seconds()
	if elapsed > 0 && current > 0 {
		speed := float64(current) / elapsed
		speedStr = fmt.Sprintf("%.1f KB/s", speed/1024)
		remaining := total - current
		if speed > 0 {
			eta := float64(remaining) / speed
			etaStr = fmt.Sprintf("ETA %.0fs", eta)
		}
	}

	return fmt.Sprintf("    [%s] %3d%% %s %s %s", bar, pct, sizeStr, speedStr, etaStr)
}
