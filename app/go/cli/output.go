package main

// ScaleFX CLI - Colored Output Helpers
// ANSI terminal colors, formatted output, and help rendering.

import (
	"fmt"
	"runtime"
	"scalefx/protocol"
	"scalefx/protocol/core"
	"sort"
	"strings"
	"time"
)

// ─── ANSI Color Codes ───

const (
	colorReset   = "\033[0m"
	colorRed     = "\033[91m"
	colorGreen   = "\033[92m"
	colorYellow  = "\033[93m"
	colorBlue    = "\033[94m"
	colorMagenta = "\033[95m"
	colorCyan    = "\033[96m"
	colorWhite   = "\033[97m"
	colorGray    = "\033[90m"
	colorBold    = "\033[1m"
	colorDim     = "\033[2m"
)

var useColor bool

func init() {
	useColor = true
}

func colorize(color, text string) string {
	if !useColor {
		return text
	}
	return color + text + colorReset
}

// ─── Standard Output ───

func PrintOK(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(colorGreen, "✓"), msg)
}

func PrintError(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(colorRed, "✗"), msg)
}

func PrintInfo(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(colorCyan, "ℹ"), msg)
}

func PrintWarning(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	fmt.Printf("%s %s\n", colorize(colorYellow, "⚠"), msg)
}

// PrintACKResult prints ACK or NACK result for a command.
func PrintACKResult(resp *protocol.Response, okMsg string) {
	if resp == nil {
		PrintError("No response (timeout)")
	} else if resp.IsACK() {
		PrintOK(okMsg)
	} else if resp.IsNACK() {
		code := resp.ErrorCode()
		name := protocol.ErrorName(code)
		msg := resp.ErrorMessage()
		PrintError("NACK: %s (0x%02X) %s", name, code, msg)
	} else {
		PrintWarning("Unexpected: 0x%02X", resp.PacketType)
	}
}

// ─── Prompt ───

// controllerColors maps controller types to ANSI colors.
var controllerColors = map[string]string{
	core.CtrlGearControl: colorGreen,
	core.CtrlGunFX:       colorRed,
	core.CtrlHubFX:       colorCyan,
	core.CtrlLightFX:     colorBlue,
	core.CtrlNoOp:        colorMagenta,
}

// controllerLabels maps controller types to display names.
var controllerLabels = map[string]string{
	core.CtrlGearControl: "GearControl",
	core.CtrlGunFX:       "GunFX",
	core.CtrlHubFX:       "HubFX",
	core.CtrlLightFX:     "LightFX",
	core.CtrlNoOp:        "NoOp",
}

// GetPrompt generates prompt based on connection state.
func GetPrompt(controllerType string, connected bool) string {
	if controllerType != "" {
		color := controllerColors[controllerType]
		if color == "" {
			color = colorCyan
		}
		label := controllerLabels[controllerType]
		if label == "" {
			label = controllerType
		}
		return colorize(color, label+">") + " "
	}
	if connected {
		return colorize(colorYellow, "connected>") + " "
	}
	return colorize(colorGray, "scalefx>") + " "
}

// ─── Help Rendering ───

// PrintGroupHelp renders a single command group with colored header and aligned columns.
// Automatically sorts commands from the group's command map.
func PrintGroupHelp(group *cmdGroup) {
	if len(group.Commands) == 0 {
		return
	}

	// Collect and sort command names
	commands := make([]string, 0, len(group.Commands))
	for name := range group.Commands {
		commands = append(commands, name)
	}
	sort.Strings(commands)

	// Colored group header with horizontal rule
	header := fmt.Sprintf("── %s ", group.Name)
	padLen := 50 - len(header)
	if padLen < 2 {
		padLen = 2
	}
	header += strings.Repeat("─", padLen)
	fmt.Printf("\n%s%s%s\n", group.Color, header, colorReset)

	// Find max usage width for alignment
	maxWidth := 0
	for _, name := range commands {
		entry := group.Commands[name]
		if len(entry.usage) > maxWidth {
			maxWidth = len(entry.usage)
		}
	}
	if maxWidth > 40 {
		maxWidth = 40
	}

	// Print each command: colored usage + dim description
	fmtStr := fmt.Sprintf("  %%s%%-%ds%%s  %s%%s%s\n", maxWidth, colorGray, colorReset)
	for _, name := range commands {
		entry := group.Commands[name]
		fmt.Printf(fmtStr, group.Color, entry.usage, colorReset, entry.description)
	}
}

// PrintBanner prints the CLI startup banner.
func PrintBanner() {
	fmt.Printf("%s%sScaleFX CLI%s %s(Go)%s — type %shelp%s for commands, %squit%s to exit\n",
		colorBold, colorCyan, colorReset,
		colorGray, colorReset,
		colorWhite, colorReset,
		colorWhite, colorReset)
}

// PrintConnectionStatus prints a summary of the current connection state.
func PrintConnectionStatus(controllerType string, initialized bool, info *InitReadyInfo) {
	if info != nil {
		label := controllerLabels[controllerType]
		if label == "" {
			label = controllerType
		}
		color := controllerColors[controllerType]
		if color == "" {
			color = colorCyan
		}
		status := colorize(colorYellow, "connected")
		if initialized {
			status = colorize(colorGreen, "initialized")
		}
		fmt.Printf("  %s%s%s v%s (build %d) — %s\n",
			color, label, colorReset, info.Version, info.Build, status)
	}
}

// ─── Progress Bar ───

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

	// Build bar: filled blocks + empty blocks
	bar := strings.Repeat("\u2588", filled) + strings.Repeat("\u2591", width-filled)

	// Size string
	sizeStr := fmt.Sprintf("%d/%d", current, total)

	// Speed and ETA
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

// ─── Utilities ───

// EnableVirtualTerminal enables ANSI escape processing on Windows console.
func EnableVirtualTerminal() {
	if runtime.GOOS != "windows" {
		return
	}
	// Windows 10+ supports ANSI natively in most terminals.
	// For older Windows cmd.exe, we would need to call SetConsoleMode
	// with ENABLE_VIRTUAL_TERMINAL_PROCESSING. Most users will be on
	// Windows Terminal or VS Code terminal which support ANSI natively.
}
