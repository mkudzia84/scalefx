package main

// GUIOutput implements engine.Output for the Wails GUI.
// Instead of printing to a terminal, each message is emitted as a
// "console:output" event that the Svelte frontend picks up and renders.

import (
	"context"
	"fmt"
	"scalefx/engine"
	"strings"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// ConsoleMessage is the event payload sent to the frontend.
type ConsoleMessage struct {
	Type    string `json:"type"`    // "ok", "error", "info", "warning", "output", "command"
	Content string `json:"content"` // HTML-safe content (with <span> color tags)
}

// GUIOutput implements engine.Output by emitting Wails events.
type GUIOutput struct {
	ctx context.Context
}

func (g *GUIOutput) emit(msgType, content string) {
	if g.ctx == nil {
		return
	}
	wailsRT.EventsEmit(g.ctx, "console:output", ConsoleMessage{
		Type:    msgType,
		Content: content,
	})
}

func (g *GUIOutput) OK(format string, args ...any) {
	g.emit("ok", fmt.Sprintf(format, args...))
}

func (g *GUIOutput) Error(format string, args ...any) {
	g.emit("error", fmt.Sprintf(format, args...))
}

func (g *GUIOutput) Info(format string, args ...any) {
	g.emit("info", fmt.Sprintf(format, args...))
}

func (g *GUIOutput) Warning(format string, args ...any) {
	g.emit("warning", fmt.Sprintf(format, args...))
}

func (g *GUIOutput) Printf(format string, args ...any) {
	text := fmt.Sprintf(format, args...)
	if text != "" {
		g.emit("output", text)
	}
}

func (g *GUIOutput) Debug(format string, args ...any) {
	if !engine.DebugBuild {
		return
	}
	g.emit("debug", escapeHTML(fmt.Sprintf(format, args...)))
}

func (g *GUIOutput) Println(a ...any) {
	text := fmt.Sprintln(a...)
	g.emit("output", text)
}

// C wraps text in an HTML span with a color class.
// The frontend CSS maps .c-red, .c-green, etc. to the actual colors.
func (g *GUIOutput) C(color engine.Color, text string) string {
	name := colorClassName(color)
	if name == "" {
		return escapeHTML(text)
	}
	return fmt.Sprintf(`<span class="c-%s">%s</span>`, name, escapeHTML(text))
}

func colorClassName(c engine.Color) string {
	switch c {
	case engine.ColorRed:
		return "red"
	case engine.ColorGreen:
		return "green"
	case engine.ColorYellow:
		return "yellow"
	case engine.ColorBlue:
		return "blue"
	case engine.ColorMagenta:
		return "magenta"
	case engine.ColorCyan:
		return "cyan"
	case engine.ColorWhite:
		return "white"
	case engine.ColorGray:
		return "gray"
	case engine.ColorBold:
		return "bold"
	case engine.ColorDim:
		return "dim"
	default:
		return ""
	}
}

func escapeHTML(s string) string {
	s = strings.ReplaceAll(s, "&", "&amp;")
	s = strings.ReplaceAll(s, "<", "&lt;")
	s = strings.ReplaceAll(s, ">", "&gt;")
	return s
}
