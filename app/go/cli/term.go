package main

// ANSI palette + small render helpers for the CLI.  Modeled on the
// archived `engine/output.go` Output interface: a logical color enum
// + ✓ / ✗ / ℹ / ⚠ status prefixes, plus key/value/header decoration
// for tabular sections.  Every command handler renders through these
// instead of bare `fmt.Println` so the look stays uniform.
//
// Win 10+ / Windows Terminal / VS Code terminal support ANSI natively.
// Set `--no-color` (or env `NO_COLOR=1`) to strip escapes for pipes.

import (
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

// ─── Palette ─────────────────────────────────────────────────────────

const (
	ansiReset   = "\033[0m"
	ansiBold    = "\033[1m"
	ansiDim     = "\033[2m"
	ansiRed     = "\033[91m"
	ansiGreen   = "\033[92m"
	ansiYellow  = "\033[93m"
	ansiBlue    = "\033[94m"
	ansiMagenta = "\033[95m"
	ansiCyan    = "\033[96m"
	ansiWhite   = "\033[97m"
	ansiGray    = "\033[90m"
)

// useColor is set at startup; flipped off by NO_COLOR or `--no-color`.
var useColor = true

func init() {
	if os.Getenv("NO_COLOR") != "" {
		useColor = false
	}
}

// wrap applies an ANSI sequence and the reset trailer (or returns the
// raw text when colors are disabled).
func wrap(code, text string) string {
	if !useColor || code == "" {
		return text
	}
	return code + text + ansiReset
}

// ─── Inline decorators ───────────────────────────────────────────────

func cBold(s string) string    { return wrap(ansiBold, s) }
func cDim(s string) string     { return wrap(ansiDim, s) }
func cRed(s string) string     { return wrap(ansiRed, s) }
func cGreen(s string) string   { return wrap(ansiGreen, s) }
func cYellow(s string) string  { return wrap(ansiYellow, s) }
func cCyan(s string) string    { return wrap(ansiCyan, s) }
func cMagenta(s string) string { return wrap(ansiMagenta, s) }
func cWhite(s string) string   { return wrap(ansiWhite, s) }
func cGray(s string) string    { return wrap(ansiGray, s) }

// ─── Status lines ────────────────────────────────────────────────────

// Ok prints a green "✓" success line.
func Ok(format string, args ...any) {
	fmt.Printf("%s %s\n", cGreen("✓"), fmt.Sprintf(format, args...))
}

// Err prints a red "✗" error line to stderr.
func Err(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "%s %s\n", cRed("✗"), fmt.Sprintf(format, args...))
}

// Info prints a cyan "ℹ" info line.
func Info(format string, args ...any) {
	fmt.Printf("%s %s\n", cCyan("ℹ"), fmt.Sprintf(format, args...))
}

// Warn prints a yellow "⚠" warning line.
func Warn(format string, args ...any) {
	fmt.Printf("%s %s\n", cYellow("⚠"), fmt.Sprintf(format, args...))
}

// Note prints a dim plain line (no icon) — used for soft hints.
func Note(format string, args ...any) {
	fmt.Println(cDim(fmt.Sprintf(format, args...)))
}

// ─── Sectional output ────────────────────────────────────────────────

// Hdr prints a bold cyan section header underlined with a dim rule.
func Hdr(title string) {
	fmt.Println(cBold(cCyan(title)))
}

// kvKeyWidth is the column width used by KV to right-pad the key.
const kvKeyWidth = 14

// KV prints a "  Key : value" row, key dim + right-padded.
func KV(key, val string) {
	fmt.Printf("  %s : %s\n", cDim(padRight(key, kvKeyWidth)), val)
}

// KVf is KV with printf-style value formatting.
func KVf(key, format string, args ...any) {
	KV(key, fmt.Sprintf(format, args...))
}

// KVList prints a "  key : v1" row followed by a continuation line per
// remaining item, aligned to the same value column.  `(none)` is shown
// dim when the list is empty.
func KVList(key string, items []string) {
	if len(items) == 0 {
		KV(key, cDim("(none)"))
		return
	}
	KV(key, items[0])
	for _, it := range items[1:] {
		// 2 indent + kvKeyWidth filler + 3 (" : ") = same value column.
		fmt.Printf("  %s   %s\n", strings.Repeat(" ", kvKeyWidth), it)
	}
}

// padRight pads `s` to `width` runes with spaces (no truncation).
func padRight(s string, width int) string {
	if n := width - len(s); n > 0 {
		return s + strings.Repeat(" ", n)
	}
	return s
}

// ─── Phase / state highlighters (used by effect handlers) ────────────

// Phase renders a state name in a colour matching its severity:
//
//	idle/stopped/off    → dim
//	running/on/deployed → green
//	starting/stopping/transitioning → yellow
//	error/critical      → red
func Phase(name string) string {
	switch strings.ToLower(name) {
	case "error", "critical", "fault":
		return cRed(name)
	case "running", "on", "deployed", "active", "armed", "firing":
		return cGreen(name)
	case "starting", "stopping", "deploying", "retracting", "warning":
		return cYellow(name)
	case "off", "stopped", "retracted", "idle", "unconfigured", "none":
		return cDim(name)
	}
	return name
}

// Bool renders a boolean as green "yes" / dim "no".
func Bool(v bool) string {
	if v {
		return cGreen("yes")
	}
	return cDim("no")
}

// HexU32 renders a uint32 as "0x%08X" in dim.
func HexU32(v uint32) string {
	return cDim(fmt.Sprintf("0x%08X", v))
}

// ─── Progress bar ────────────────────────────────────────────────────

// ProgressBar renders an inline progress line:
//
//	[████░░░░░░] 42% 1.2/2.9 MB  450 KB/s  ETA 4s
//
// Matches the archived CLI's `engine.FormatProgressBar` look.  The
// caller is expected to print with a leading "\r" and follow up with
// `fmt.Println()` once done so the cursor moves to the next line.
func ProgressBar(current, total int64, started time.Time, width int) string {
	if total <= 0 {
		return ""
	}
	pct := int(current * 100 / total)
	if pct > 100 {
		pct = 100
	}
	filled := int(current * int64(width) / total)
	if filled > width {
		filled = width
	}
	bar := strings.Repeat("█", filled) + cDim(strings.Repeat("░", width-filled))

	sizeStr := fmt.Sprintf("%s/%s",
		humanBytes(uint64(current)), humanBytes(uint64(total)))

	rate := "-- KB/s"
	eta := ""
	elapsed := time.Since(started).Seconds()
	if elapsed > 0 && current > 0 {
		bps := float64(current) / elapsed
		rate = fmt.Sprintf("%s/s", humanBytes(uint64(bps)))
		if bps > 0 && current < total {
			etaSec := float64(total-current) / bps
			eta = fmt.Sprintf("ETA %ds", int(etaSec+0.5))
		}
	}

	return fmt.Sprintf("[%s] %s%3d%%%s  %s  %s  %s",
		bar,
		cBold(""), pct, "",
		cDim(sizeStr),
		cCyan(rate),
		cDim(eta))
}


// Quote returns the string wrapped in dim double-quotes.
func Quote(s string) string {
	return cDim(`"`) + s + cDim(`"`)
}

// stderrColorless is here only to keep the io import warm — sub-files
// extend with their own writers and reference this so vet stays happy.
var _ io.Writer = os.Stderr
