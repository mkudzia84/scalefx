package main

// Console event payload + HTML escaping for GUI-side console output.
//
// The legacy `GUIOutput` type implemented the old `engine.Output`
// interface (the engine's command dispatcher wrote formatted, colored
// lines to it).  That engine was retired with the move to the typed
// `scalefx/client` API; Studio now emits console lines directly from the
// App's echo helpers (see app.go).  All that remains here is the wire
// payload + the shared HTML escaper.

import "strings"

// ConsoleMessage is the event payload sent to the frontend console.
type ConsoleMessage struct {
	Type    string `json:"type"`    // "ok", "error", "info", "warning", "output", "command", "debug"
	Content string `json:"content"` // HTML-safe content
}

// escapeHTML makes a string safe to drop into the console's innerHTML.
func escapeHTML(s string) string {
	s = strings.ReplaceAll(s, "&", "&amp;")
	s = strings.ReplaceAll(s, "<", "&lt;")
	s = strings.ReplaceAll(s, ">", "&gt;")
	return s
}
