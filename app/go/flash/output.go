package main

// ScaleFX Flash CLI - Terminal Output
// Standalone ANSI-colored output — no engine dependency.

import (
	"fmt"
	"strings"

	fw "scalefx/firmware"
)

// ─── ANSI Codes ───

const (
	reset   = "\033[0m"
	red     = "\033[91m"
	green   = "\033[92m"
	yellow  = "\033[93m"
	blue    = "\033[94m"
	magenta = "\033[95m"
	cyan    = "\033[96m"
	gray    = "\033[90m"
	bold    = "\033[1m"
	dim     = "\033[2m"
)

func colorize(color, text string) string {
	return color + text + reset
}

// ─── Structured Messages ───

func printOK(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(green, "✓"), msg)
}

func printError(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(red, "✗"), msg)
}

func printInfo(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(cyan, "ℹ"), msg)
}

func printWarning(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(yellow, "⚠"), msg)
}

func printStep(step, total int, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s [%d/%d] %s\n", colorize(cyan, "→"), step, total, msg)
}

// ─── Firmware Event Handler ───

// fwEventHandler translates firmware library events to colored terminal output.
func fwEventHandler(evt fw.Event) {
	switch evt.Type {
	case fw.EventInfo:
		printInfo("%s", evt.Message)
	case fw.EventOK:
		printOK("%s", evt.Message)
	case fw.EventWarning:
		printWarning("%s", evt.Message)
	case fw.EventError:
		printError("%s", evt.Message)
	case fw.EventStep:
		printStep(evt.Step, evt.Total, "%s", evt.Message)
	case fw.EventProgress:
		fmt.Printf("  Progress: %d%%\n", evt.Progress)
	}
}

// ─── Banner ───

func printBanner() {
	fmt.Println()
	fmt.Printf("  %s  %s\n",
		colorize(bold+magenta, "ScaleFX Flash"),
		colorize(gray, "— firmware build & flash tool"))
	fmt.Println()
}

// ─── Size Formatting ───

func fmtSize(bytes int64) string {
	if bytes >= 1048576 {
		return fmt.Sprintf("%.1f MB", float64(bytes)/1048576)
	}
	if bytes >= 1024 {
		return fmt.Sprintf("%.1f KB", float64(bytes)/1024)
	}
	return fmt.Sprintf("%d B", bytes)
}

// ─── Release Notes Summary ───

func noteSummary(body string, maxLen int) string {
	if body == "" {
		return ""
	}
	for _, line := range strings.Split(body, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "#") || strings.HasPrefix(line, "|") || strings.HasPrefix(line, "---") {
			continue
		}
		if strings.HasPrefix(line, "**") && strings.Contains(line, ":**") {
			continue
		}
		if len(line) > maxLen {
			return line[:maxLen-3] + "..."
		}
		return line
	}
	return ""
}
